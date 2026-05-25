#include "streamdecoder.h"
#include <QDebug>

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

// 主循环：取包 → 解码 → 推帧。nullptr 包作为 seek sentinel，刷新解码器缓冲而非触发 EOF。
void StreamDecoder::run() {
    AVPacket* pkt   = nullptr;
    AVFrame*  frame = av_frame_alloc();

    while (!abort_.load(std::memory_order_relaxed)) {
        if (!inputQueue_->tryPop(pkt, 20)) continue;

        if (!pkt) {
            // sentinel：flush 解码器缓冲，丢弃残留帧，准备接收新位置的包
            avcodec_flush_buffers(codecCtx_);
            if (outputQueue_) outputQueue_->clear();
            continue;
        }

        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
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
