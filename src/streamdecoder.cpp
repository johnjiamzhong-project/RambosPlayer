#include "streamdecoder.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
}

StreamDecoder::~StreamDecoder() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

// 打开解码器（视频/音频通用），不做格式转换
bool StreamDecoder::init(AVCodecParameters* par) {
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    abort_.store(false, std::memory_order_relaxed);

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        qWarning() << "StreamDecoder: decoder not found for"
                   << avcodec_get_name(par->codec_id);
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;

    if (avcodec_parameters_to_context(codecCtx_, par) < 0) {
        qWarning() << "StreamDecoder: avcodec_parameters_to_context failed";
        return false;
    }

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        qWarning() << "StreamDecoder: avcodec_open2 failed for" << codec->name;
        return false;
    }

    qInfo() << "StreamDecoder::init ok codec=" << codec->name
            << "sampleRate=" << codecCtx_->sample_rate
            << "channels=" << codecCtx_->ch_layout.nb_channels;
    return true;
}

// 设置停止标志并 abort 两侧队列，解除 run() 阻塞
void StreamDecoder::stop() {
    abort_ = true;
    if (inputQueue_)  inputQueue_->abort();
    if (outputQueue_) outputQueue_->abort();
}

// 设置 seek 后允许输出到下游编码器的最小时间戳。
// 解码器仍会消费目标点前的包完成 H.264 参考帧预滚，但丢弃对应解码帧。
void StreamDecoder::setMinOutputSeconds(double seconds) {
    if (seconds < 0.0 || inputTimeBase_.num <= 0 || inputTimeBase_.den <= 0) {
        minOutputPts_.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
        return;
    }
    int64_t pts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, inputTimeBase_);
    minOutputPts_.store(pts, std::memory_order_relaxed);
    prerollDropLogCount_.store(0, std::memory_order_relaxed);
    qInfo() << "StreamDecoder: min output after seek seconds=" << seconds
            << "pts=" << pts;
}

// 主循环：取包 → 解码 → 推帧。nullptr 包作为 seek sentinel，刷新解码器缓冲而非触发 EOF。
void StreamDecoder::run() {
    AVPacket* pkt   = nullptr;
    AVFrame*  frame = av_frame_alloc();

    while (!abort_.load(std::memory_order_relaxed)) {
        if (!inputQueue_->tryPop(pkt, 20)) continue;

        if (!pkt) {
            // sentinel（seek）：flush 解码器缓冲，向下游传播 nullptr 触发 EncodeThread flush
            qInfo() << "StreamDecoder: seek sentinel received, flushing codec buffers";
            avcodec_flush_buffers(codecCtx_);
            if (outputQueue_) {
                outputQueue_->clear();
                bool ok = outputQueue_->tryPush(nullptr);
                qInfo() << "StreamDecoder: nullptr pushed downstream ok=" << ok;
            } else {
                qWarning() << "StreamDecoder: no outputQueue, sentinel not propagated";
            }
            continue;
        }

        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            int64_t minPts = minOutputPts_.load(std::memory_order_relaxed);
            if (minPts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
                if (frame->pts < minPts) {
                    int n = prerollDropLogCount_.fetch_add(1, std::memory_order_relaxed);
                    if (n < 3)
                        qInfo() << "StreamDecoder: drop preroll frame pts=" << frame->pts
                                << "minPts=" << minPts;
                    av_frame_unref(frame);
                    continue;
                }
                minOutputPts_.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
                qInfo() << "StreamDecoder: min output reached pts=" << frame->pts
                        << "minPts=" << minPts
                        << "dropped=" << prerollDropLogCount_.load(std::memory_order_relaxed);
            }
            if (outputQueue_) {
                outputQueue_->push(frame);
                frame = av_frame_alloc();
            } else {
                av_frame_unref(frame);
            }
        }
    }

    av_frame_free(&frame);
    emit finished();
}
