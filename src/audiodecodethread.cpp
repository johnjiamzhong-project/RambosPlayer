#include "audiodecodethread.h"
#include "logger.h"
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcAudio, "rambos.audio", QtWarningMsg)

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

// 暂停/恢复：run() 检测到 paused_==true 时挂起 QAudioOutput，避免声音继续输出
void AudioDecodeThread::setPaused(bool p) { paused_ = p; }

// 线程安全设置音量，run() 循环中检测并应用
void AudioDecodeThread::setVolume(float v) { pendingVolume_.store(v); }

// 主循环：取包 → 解码 → swr_convert → QAudioOutput::write → 更新音频时钟。
// QAudioOutput 在此处创建，保证创建/start/write/stop 都在同一线程，满足 Qt 线程亲和性。
// 时钟用 processedUSecs()（硬件实际播放量）而非解码位置，避免缓冲区超前导致视频跑飞。
void AudioDecodeThread::run() {
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(2);
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setCodec("audio/pcm");
    sink_ = new QAudioOutput(fmt);
    sink_->setBufferSize(16384);  // ~93ms；缓冲区过大会让时钟超前实际播放
    device_ = sink_->start();

    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;
    double lastLogClock = -1.0;  // 上次打印时钟的值，每推进 1s 打一次

    while (!abort_) {
        // 暂停：挂起硬件输出并自旋等待，恢复时 resume sink
        if (paused_.load()) {
            if (sink_->state() == QAudio::ActiveState)
                sink_->suspend();
            QThread::msleep(10);
            continue;
        }
        if (sink_->state() == QAudio::SuspendedState)
            sink_->resume();

        float vol = pendingVolume_.exchange(-1.f);
        if (vol >= 0.f) sink_->setVolume(vol);

        if (flush_.exchange(false)) {
            // 第一轮清空：刷掉队列中已有的旧包
            AVPacket* stale;
            while (inputQueue_->tryPop(stale, 0)) av_packet_free(&stale);

            avcodec_flush_buffers(codecCtx_);
            swr_convert(swrCtx_, nullptr, 0, nullptr, 0);
            sink_->stop();
            sink_->setBufferSize(16384);
            device_ = sink_->start();

            // 第二轮清空：处理 flush 期间新推入的包
            //（DemuxThread 可能在 handleSeek 前还在推旧位置的包）
            while (inputQueue_->tryPop(stale, 0)) av_packet_free(&stale);

            // gen 在最后递增——确保两轮清空之后收到的帧才允许更新时钟
            int64_t gen = flushGen_.fetch_add(1, std::memory_order_relaxed) + 1;
            qInfo() << "AudioDecodeThread: flush gen" << gen << "done";
        }

        if (!inputQueue_->tryPop(pkt, 20)) continue;

        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            // 记下收帧时的世代，后续只在该世代未变时更新时钟
            int64_t gen = flushGen_.load(std::memory_order_relaxed);

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

            if (n > 0 && device_) {
                while (!abort_ && sink_->bytesFree() < n * 4)
                    QThread::msleep(2);
                if (!abort_)
                    device_->write((const char*)outBuf, n * 4);
            }

            // 只有世代未变时才更新时钟，防止旧帧将时钟拉回 seek 前的位置
            if (frame->pts != AV_NOPTS_VALUE &&
                gen == flushGen_.load(std::memory_order_relaxed)) {
                double clock = frame->pts * av_q2d(timeBase_);
                sync_->setAudioClock(clock);
            } else if (frame->pts != AV_NOPTS_VALUE) {
                qInfo() << "AudioDecodeThread: clock update BLOCKED gen" << gen
                                 << "current flushGen" << flushGen_.load()
                                 << "pts" << frame->pts;
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
