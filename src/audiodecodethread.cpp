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

    qInfo() << "AudioDecodeThread::init ok codec=" << codec->name
            << "sampleRate=" << codecCtx_->sample_rate
            << "channels=" << codecCtx_->ch_layout.nb_channels;
    return true;
}

// 设置停止标志并 abort 输入队列，解除 run() 阻塞
void AudioDecodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
}

// 标记需要 flush，并记录 seek 目标供 run() 过滤竞态旧帧。
// seekTargetSec < 0 表示非 seek 触发的 flush，不启用 PTS 过滤。
void AudioDecodeThread::flush(double seekTargetSec) {
    minAcceptablePts_.store(seekTargetSec, std::memory_order_relaxed);
    flush_ = true;
}

// 暂停/恢复：run() 检测到 paused_==true 时挂起 QAudioOutput，避免声音继续输出
void AudioDecodeThread::setPaused(bool p) { paused_ = p; }

// 线程安全设置音量，run() 循环中检测并应用
void AudioDecodeThread::setVolume(float v) { pendingVolume_.store(v); }

// 主循环：取包 → 解码 → swr_convert → QAudioOutput::write → 更新音频时钟。
// QAudioOutput 在此处创建，保证创建/start/write/stop 都在同一线程，满足 Qt 线程亲和性。
// 时钟用 processedUSecs()（硬件实际播放量）而非解码位置，避免缓冲区超前导致视频跑飞。
void AudioDecodeThread::run() {
    qInfo() << "AudioDecodeThread::run start";
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
    double lastLogClock = -1.0;    // 上次打印时钟的值，每推进 1s 打一次
    int64_t lastPopFlushGen = -1;  // 用于识别每次 flush 后的第一个 pkt

    while (!abort_) {
        // 暂停：先处理 flush（seek while paused），再挂起硬件输出并自旋等待。
        // flush 必须在 paused 分支内检查，否则 continue 会绕过主循环下方的 flush 块，
        // 导致 seek while paused 后 codec/swr 缓冲不被清空，位置错乱。
        if (paused_.load()) {
            if (flush_.exchange(false)) {
                avcodec_flush_buffers(codecCtx_);
                swr_convert(swrCtx_, nullptr, 0, nullptr, 0);
                sink_->stop();
                sink_->setBufferSize(16384);
                device_ = sink_->start();
                int64_t gen = flushGen_.fetch_add(1, std::memory_order_relaxed) + 1;
                qInfo() << "AudioDecodeThread: flush gen" << gen << "done (paused)"
                        << "minAcceptablePts=" << minAcceptablePts_.load();
            }
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
            // 不在此处清包：PlayerController::seek() 已调 audioPacketQ_.clear()，
            // DemuxThread::handleSeek() 紧接着也 clear 一次；两步之后队列里已是
            // seek 目标位置之后的有效新包。若此处再清，DemuxThread 在两次 clear 之间
            // 以文件 I/O 全速推入的 500+ 个有效包会全部丢失，导致消费起点超前 ~11s。
            // 极少数竞态漏入的旧包（< 2 个）由 minAcceptablePts_ 时钟过滤器兜底。

            avcodec_flush_buffers(codecCtx_);
            swr_convert(swrCtx_, nullptr, 0, nullptr, 0);
            sink_->stop();
            sink_->setBufferSize(16384);
            device_ = sink_->start();

            // gen 在最后递增——确保 flush 完成后收到的帧才允许更新时钟
            int64_t gen = flushGen_.fetch_add(1, std::memory_order_relaxed) + 1;
            qInfo() << "AudioDecodeThread: flush gen" << gen << "done"
                    << "minAcceptablePts=" << minAcceptablePts_.load();
        }

        if (!inputQueue_->tryPop(pkt, 20)) continue;

        // 每次 flush 后打印第一个被消费的 pkt PTS，便于确认消费起点
        {
            int64_t curGen = flushGen_.load(std::memory_order_relaxed);
            if (curGen != lastPopFlushGen) {
                lastPopFlushGen = curGen;
                double pktPts = (pkt->pts != AV_NOPTS_VALUE)
                    ? pkt->pts * av_q2d(timeBase_) : -1.0;
                qInfo() << "AudioDecodeThread: first pkt after flush gen" << curGen
                        << "PTS=" << pktPts;
            }
        }

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

            // 只有世代未变且没有待处理 flush 时才更新时钟，
            // 防止旧帧在 seek 后将时钟拉回错误位置。
            // minAcceptablePts_ 过滤竞态漏入的旧包（PTS 远低于 seek 目标的帧），
            // 首帧通过后立即清除该过滤器。
            if (frame->pts != AV_NOPTS_VALUE &&
                gen == flushGen_.load(std::memory_order_relaxed) &&
                !flush_.load(std::memory_order_relaxed)) {
                double clock = frame->pts * av_q2d(timeBase_);
                double minPts = minAcceptablePts_.load(std::memory_order_relaxed);
                if (minPts < 0.0 || clock >= minPts - 1.0) {
                    sync_->setAudioClock(clock);
                    if (minPts >= 0.0) {
                        qInfo() << "AudioDecodeThread: first valid clock after seek"
                                << "PTS=" << clock << "seekTarget=" << minPts
                                << "diff=" << (clock - minPts);
                        minAcceptablePts_.store(-1.0, std::memory_order_relaxed);
                    }
                } else {
                    qInfo() << "AudioDecodeThread: clock filtered by minPts=" << minPts
                            << "PTS=" << clock << "diff=" << (clock - minPts);
                }
            } else if (frame->pts != AV_NOPTS_VALUE) {
                qInfo() << "AudioDecodeThread: clock update BLOCKED gen" << gen
                                 << "current flushGen" << flushGen_.load()
                                 << "flush pending" << flush_.load()
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
    qInfo() << "AudioDecodeThread::run finished";
    emit finished();
}
