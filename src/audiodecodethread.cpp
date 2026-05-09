#include "audiodecodethread.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

// sink_ 正常在 run() 退出时销毁；若线程未启动则 sink_ 为 nullptr 无需清理。
AudioDecodeThread::~AudioDecodeThread() {
    stop(); wait();
    if (sink_) { sink_->stop(); delete sink_; sink_ = nullptr; }
    if (swrCtx_)   swr_free(&swrCtx_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

// 打开解码器 + 初始化 SwrContext（→ S16 Stereo 44100）。
// QAudioOutput 延迟到 run() 中创建，保证线程亲和性（创建和使用在同一线程）。
bool AudioDecodeThread::init(AVCodecParameters* params,
                              AVRational timeBase,
                              AVSync* sync) {
    abort_.store(false, std::memory_order_relaxed);

    timeBase_ = timeBase;
    sync_ = sync;

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) return false;

    swrCtx_ = swr_alloc();
    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(swrCtx_, "in_chlayout", &codecCtx_->ch_layout, 0);
    av_opt_set_int(swrCtx_, "in_sample_rate", codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);
    av_opt_set_chlayout(swrCtx_, "out_chlayout", &outChLayout, 0);
    av_opt_set_int(swrCtx_, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(swrCtx_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    if (swr_init(swrCtx_) < 0) return false;

    return true;
}

// 设置停止标志并 abort 输入队列，解除 run() 阻塞
void AudioDecodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
}

// 标记需要 flush，run() 检测后清空解码器缓冲和 swr 缓冲
void AudioDecodeThread::flush() { flush_ = true; }

// 线程安全设置音量，run() 循环中检测并应用
void AudioDecodeThread::setVolume(float v) { pendingVolume_.store(v); }

// 主循环：取包 → 解码 → swr_convert → QAudioOutput::write → 更新音频时钟。
// QAudioOutput 在此处创建，保证创建/start/write/stop 都在同一线程，满足 Qt 线程亲和性。
void AudioDecodeThread::run() {
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(2);
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setCodec("audio/pcm");
    sink_ = new QAudioOutput(fmt);
    device_ = sink_->start();

    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;

    while (!abort_) {
        float vol = pendingVolume_.exchange(-1.f);
        if (vol >= 0.f) sink_->setVolume(vol);

        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            swr_convert(swrCtx_, nullptr, 0, nullptr, 0);
        }

        if (!inputQueue_->tryPop(pkt, 20)) continue;

        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            int outSamples = av_rescale_rnd(
                swr_get_delay(swrCtx_, codecCtx_->sample_rate) + frame->nb_samples,
                44100, codecCtx_->sample_rate, AV_ROUND_UP);

            int needed = outSamples * 2 * 2;
            if (needed > outBufSize) {
                av_free(outBuf);
                outBuf = (uint8_t*)av_malloc(needed);
                outBufSize = needed;
            }
            uint8_t* out[1] = { outBuf };
            int n = swr_convert(swrCtx_, out, outSamples,
                                (const uint8_t**)frame->data, frame->nb_samples);
            if (n > 0 && device_)
                device_->write((const char*)outBuf, n * 4);

            if (frame->pts != AV_NOPTS_VALUE) {
                double pts = frame->pts * av_q2d(timeBase_)
                           + (double)frame->nb_samples / codecCtx_->sample_rate;
                sync_->setAudioClock(pts);
            }
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    av_free(outBuf);
    sink_->stop();
    delete sink_;
    sink_ = nullptr;
    device_ = nullptr;
    emit finished();
}
