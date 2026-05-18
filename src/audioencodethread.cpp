#include "audioencodethread.h"
#include <QDebug>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

AudioEncodeThread::~AudioEncodeThread() {
    stop(); wait();
    if (codecCtx_)  avcodec_free_context(&codecCtx_);
    if (swrCtx_)    swr_free(&swrCtx_);
    if (fifo_)      av_audio_fifo_free(fifo_);
    if (swrFrame_)  av_frame_free(&swrFrame_);
}

// 打开 AAC 编码器，分配 AVAudioFifo 和 swr 输出帧占位。
// SwrContext 在第一帧到来时懒初始化（此时才知道输入格式）。
bool AudioEncodeThread::init(int sampleRate, int channels, int bitrate) {
    if (codecCtx_)  avcodec_free_context(&codecCtx_);
    if (swrCtx_)  { swr_free(&swrCtx_);       swrCtx_ = nullptr; }
    if (fifo_)    { av_audio_fifo_free(fifo_); fifo_   = nullptr; }
    if (swrFrame_)  av_frame_free(&swrFrame_);

    abort_.store(false, std::memory_order_relaxed);
    nextPts_    = 0;
    sampleRate_ = sampleRate;
    channels_   = channels;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) { qWarning() << "AudioEncodeThread: AAC encoder not found"; return false; }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;

    codecCtx_->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    codecCtx_->sample_rate = sampleRate;
    codecCtx_->bit_rate    = bitrate;
    codecCtx_->time_base   = {1, sampleRate};
    av_channel_layout_default(&codecCtx_->ch_layout, channels);
    // FLV/RTMP 封装需要 SPS 存于 extradata
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        qWarning() << "AudioEncodeThread: avcodec_open2 failed";
        return false;
    }

    fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, 1);
    if (!fifo_) return false;

    swrFrame_ = av_frame_alloc();
    if (!swrFrame_) return false;

    qInfo() << "AudioEncodeThread::init ok sr=" << sampleRate
            << "ch=" << channels << "frameSize=" << codecCtx_->frame_size;
    return true;
}

// 设置停止标志并 abort 所有关联队列
void AudioEncodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
    for (auto* q : outputQueues_) q->abort();
}

void AudioEncodeThread::addOutputQueue(FrameQueue<AVPacket*>* q) {
    outputQueues_.push_back(q);
}

void AudioEncodeThread::clearOutputQueues() { outputQueues_.clear(); }

// 将编码包 clone 后 fan-out 推入所有输出队列
static void fanOut(AVPacket* pkt,
                   std::vector<FrameQueue<AVPacket*>*>& queues) {
    for (auto* q : queues) {
        AVPacket* copy = av_packet_clone(pkt);
        q->push(copy);
    }
}

// 主循环：取原始音频帧 → swr 转 fltp → AVAudioFifo 缓冲 → 每 1024 samples 编码一次 → fan-out
void AudioEncodeThread::run() {
    AVFrame* frame = nullptr;

    while (!abort_) {
        if (!inputQueue_->tryPop(frame, 20)) continue;

        // SwrContext 懒初始化：首帧到来时才知道输入格式
        if (!swrCtx_) {
            swrCtx_ = swr_alloc();
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, channels_);
            av_opt_set_chlayout(swrCtx_,   "in_chlayout",   &frame->ch_layout, 0);
            av_opt_set_int(swrCtx_,         "in_sample_rate",  frame->sample_rate, 0);
            av_opt_set_sample_fmt(swrCtx_,  "in_sample_fmt",  (AVSampleFormat)frame->format, 0);
            av_opt_set_chlayout(swrCtx_,   "out_chlayout",  &outLayout, 0);
            av_opt_set_int(swrCtx_,         "out_sample_rate", sampleRate_, 0);
            av_opt_set_sample_fmt(swrCtx_,  "out_sample_fmt",  AV_SAMPLE_FMT_FLTP, 0);
            av_channel_layout_uninit(&outLayout);
            if (swr_init(swrCtx_) < 0) {
                qWarning() << "AudioEncodeThread: swr_init failed";
                swr_free(&swrCtx_); swrCtx_ = nullptr;
                av_frame_free(&frame);
                continue;
            }
            qInfo() << "AudioEncodeThread: swr lazy init"
                    << av_get_sample_fmt_name((AVSampleFormat)frame->format)
                    << frame->sample_rate << "Hz -> fltp" << sampleRate_ << "Hz";
        }

        // 计算转换后的采样数并执行 swr_convert
        int outSamples = (int)av_rescale_rnd(
            swr_get_delay(swrCtx_, frame->sample_rate) + frame->nb_samples,
            sampleRate_, frame->sample_rate, AV_ROUND_UP);

        av_frame_unref(swrFrame_);
        swrFrame_->sample_rate = sampleRate_;
        av_channel_layout_default(&swrFrame_->ch_layout, channels_);
        swrFrame_->format      = (int)AV_SAMPLE_FMT_FLTP;
        swrFrame_->nb_samples  = outSamples;
        av_frame_get_buffer(swrFrame_, 0);

        int n = swr_convert(swrCtx_,
                            swrFrame_->data, outSamples,
                            (const uint8_t**)frame->data, frame->nb_samples);
        av_frame_free(&frame);
        if (n <= 0) continue;

        av_audio_fifo_write(fifo_, (void**)swrFrame_->data, n);

        // 凑够 frame_size（AAC 固定 1024）才喂编码器
        bool firstPacket = (nextPts_ == 0);
        while (!abort_ && av_audio_fifo_size(fifo_) >= codecCtx_->frame_size) {
            AVFrame* enc = av_frame_alloc();
            enc->nb_samples  = codecCtx_->frame_size;
            av_channel_layout_copy(&enc->ch_layout, &codecCtx_->ch_layout);
            enc->format      = (int)codecCtx_->sample_fmt;
            enc->sample_rate = codecCtx_->sample_rate;
            av_frame_get_buffer(enc, 0);

            av_audio_fifo_read(fifo_, (void**)enc->data, codecCtx_->frame_size);
            enc->pts  = nextPts_;
            nextPts_ += codecCtx_->frame_size;

            if (avcodec_send_frame(codecCtx_, enc) >= 0) {
                AVPacket* pkt = av_packet_alloc();
                while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
                    if (firstPacket) {
                        qInfo() << "AudioEncodeThread: first AAC packet size=" << pkt->size
                                << "pts=" << pkt->pts << "targets=" << outputQueues_.size();
                        firstPacket = false;
                    }
                    fanOut(pkt, outputQueues_);
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
            av_frame_free(&enc);
        }
    }

    // 排空编码器残留帧
    if (codecCtx_) {
        avcodec_send_frame(codecCtx_, nullptr);
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
            fanOut(pkt, outputQueues_);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    qInfo() << "AudioEncodeThread::run finished";
    emit finished();
}
