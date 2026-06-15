// src/demuxthread.cpp
#include "demuxthread.h"
#include "localrecorder.h"
#include "muxthread.h"
#include "logger.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <QtDebug>

// 根据 URL scheme 判断是否为网络流，并构造对应协议的连接超时选项；
// 调用方在 avformat_open_input 后需 av_dict_free(&opts) 释放未被消费的选项。
static AVDictionary* buildNetworkOptions(const QString& url, bool& isNetwork) {
    AVDictionary* opts = nullptr;
    if (url.startsWith("rtsp://", Qt::CaseInsensitive)) {
        isNetwork = true;
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);   // 5s，微秒（RTSP TCP 连接超时）
    } else if (url.startsWith("http://", Qt::CaseInsensitive) ||
               url.startsWith("https://", Qt::CaseInsensitive)) {
        isNetwork = true;
        av_dict_set(&opts, "rw_timeout", "5000000", 0); // 5s，微秒（HTTP/HTTP-FLV 读写超时）
    } else if (url.startsWith("rtmp://", Qt::CaseInsensitive) ||
               url.startsWith("rtmps://", Qt::CaseInsensitive) ||
               url.startsWith("srt://", Qt::CaseInsensitive)) {
        isNetwork = true;
    } else {
        isNetwork = false;
    }
    return opts;
}

// 先 stop() 通知线程退出，再 wait() 等线程结束，最后释放 AVFormatContext。
// 顺序不能颠倒：必须确保 run() 不再访问 fmtCtx_ 后才能释放它。
DemuxThread::~DemuxThread() {
    stop();
    wait();
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
}

// 探测媒体文件/网络流：打开容器、读取流信息、记录第一条视频/音频流索引。
// 静态方法，不访问 this 的任何成员，可在独立 worker 线程调用，避免
// avformat_open_input/avformat_find_stream_info 阻塞 UI 线程（网络流 DNS/握手可能耗时数秒）。
// 任一步骤失败均返回 ok=false，并确保探测过程中分配的 fmtCtx 已释放。
DemuxThread::ProbeResult DemuxThread::probeOpen(const QString& path) {
    ProbeResult r;
    r.url = path;

    // 步骤1：打开容器文件，分配并填充 AVFormatContext（包含封装格式、I/O 缓冲等）
    // path 转 UTF-8 是为了兼容中文路径；网络流（rtmp/rtsp/http/srt）额外传超时选项，
    // 避免连接失败时 avformat_open_input 永久阻塞
    AVDictionary* opts = buildNetworkOptions(path, r.isNetwork);
    int openRet = avformat_open_input(&r.fmtCtx, path.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (openRet < 0) {
        char errbuf[64];
        av_strerror(openRet, errbuf, sizeof(errbuf));
        qWarning() << "DemuxThread::probeOpen: avformat_open_input failed for" << path
                   << "error:" << errbuf;
        return r;
    }

    // 步骤2：读取若干帧数据，推断每条流的编解码参数（帧率、采样率等）
    // 部分格式（如 MPEG-TS）无法从文件头直接得到完整参数，必须靠此步骤补全
    int probeRet = avformat_find_stream_info(r.fmtCtx, nullptr);
    if (probeRet < 0) {
        char errbuf[64];
        av_strerror(probeRet, errbuf, sizeof(errbuf));
        qWarning() << "DemuxThread::probeOpen: avformat_find_stream_info failed for" << path
                   << "error:" << errbuf;
        avformat_close_input(&r.fmtCtx);  // 防止 fmtCtx 泄漏
        return r;
    }

    // 步骤3：遍历所有流，记录第一条视频流和第一条音频流的索引
    // 只取第一条是因为播放器不支持多视角/多音轨切换；videoIdx/audioIdx 初值为 -1
    for (unsigned i = 0; i < r.fmtCtx->nb_streams; ++i) {
        auto type = r.fmtCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && r.videoIdx < 0) r.videoIdx = (int)i;
        if (type == AVMEDIA_TYPE_AUDIO && r.audioIdx < 0) r.audioIdx = (int)i;
    }

    r.duration = r.fmtCtx->duration;  // 总时长，单位 AV_TIME_BASE=1e6 微秒
    r.ok = true;
    return r;
}

// 在主线程接管 probeOpen() 的探测结果：关闭旧 fmtCtx_，写入流参数和队列指针。
// 必须在主线程调用（操作 fmtCtx_ 等成员并发出 Qt 信号）。
void DemuxThread::adopt(const ProbeResult& r,
                         FrameQueue<AVPacket*>* videoQueue,
                         FrameQueue<AVPacket*>* audioQueue) {
    // 重置 abort_ 标志，允许 run() 在下次 start() 后正常循环
    abort_.store(false, std::memory_order_relaxed);

    // 关闭上一个文件，避免新 fmtCtx_ 覆盖时旧上下文泄漏
    //（例如播放中通过最近文件菜单切换视频时，fmtCtx_ 仍指向旧文件上下文）
    if (fmtCtx_) { avformat_close_input(&fmtCtx_); }

    fmtCtx_     = r.fmtCtx;
    videoIdx_   = r.videoIdx;
    audioIdx_   = r.audioIdx;
    duration_   = r.duration;
    url_        = r.url;
    isNetwork_  = r.isNetwork;
    videoQueue_ = videoQueue;
    audioQueue_ = audioQueue;
    qInfo() << "DemuxThread::open ok" << r.url
            << "duration=" << duration_ / AV_TIME_BASE << "s"
            << "videoIdx=" << videoIdx_ << "audioIdx=" << audioIdx_
            << "isNetwork=" << isNetwork_;
    if (isNetwork_) emit networkStateChanged(static_cast<int>(NetworkState::Connected));
}

// 同步便捷封装：probeOpen() 失败直接返回 false，成功则 adopt()。供单元测试使用。
bool DemuxThread::open(const QString& path,
                        FrameQueue<AVPacket*>* videoQueue,
                        FrameQueue<AVPacket*>* audioQueue) {
    ProbeResult r = probeOpen(path);
    if (!r.ok) return false;
    adopt(r, videoQueue, audioQueue);
    return true;
}

// 设置 abort_ 标志，并对所有队列调用 abort()，唤醒阻塞线程使 run() 能退出。
// 同时中止所有推流分叉队列。本地录制器在 clearRestreamQueues 前单独停止。
void DemuxThread::stop() {
    abort_.store(true, std::memory_order_relaxed);
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
    {
        QMutexLocker lk(&restreamMtx_);
        for (auto* r : localRecorders_) r->stop();
    }
    clearRestreamQueues();
}

// 线程安全地添加视频推流分叉队列（网络推流用 tryPush）
void DemuxThread::addRestreamVideoQueue(FrameQueue<AVPacket*>* q) {
    QMutexLocker lk(&restreamMtx_);
    restreamVideoQueues_.push_back(q);
    logNextRestreamVideo_.store(true, std::memory_order_relaxed);
}

// 线程安全地添加音频推流分叉队列
void DemuxThread::addRestreamAudioQueue(FrameQueue<AVPacket*>* q) {
    QMutexLocker lk(&restreamMtx_);
    restreamAudioQueues_.push_back(q);
}

// 线程安全地注册 MuxThread，seek 时通知其进入关键帧抑制期
void DemuxThread::addMuxThread(MuxThread* m) {
    QMutexLocker lk(&restreamMtx_);
    muxThreads_.push_back(m);
    qInfo() << "DemuxThread: addMuxThread total=" << muxThreads_.size() << "ptr=" << (void*)m;
}

// 线程安全地添加本地录制器（直接写 FLV，不经过队列）
void DemuxThread::addLocalRecorder(LocalRecorder* r) {
    QMutexLocker lk(&restreamMtx_);
    localRecorders_.push_back(r);
    logNextRestreamVideo_.store(true, std::memory_order_relaxed);
    qInfo() << "DemuxThread: addLocalRecorder total=" << localRecorders_.size()
            << "ptr=" << (void*)r;
}

// 中止并清空所有推流分叉队列；本地录制器仅从列表移除（不调 stop，恢复时复用）。
// run() 持互斥锁期间不会读取这些容器。
void DemuxThread::clearRestreamQueues() {
    QMutexLocker lk(&restreamMtx_);
    for (auto* q : restreamVideoQueues_) q->abort();
    for (auto* q : restreamAudioQueues_) q->abort();
    // 本地录制器不 stop——恢复推流时需继续使用；完整停止由 DemuxThread::stop() 处理
    restreamVideoQueues_.clear();
    restreamAudioQueues_.clear();
    localRecorders_.clear();
    muxThreads_.clear();
}

// 将 seek 目标原子写入 seekTarget_，seek 前播放位置写入 seekFromPosition_；
// 实际跳转在 run() 下次循环顶部的 handleSeek() 中执行。
void DemuxThread::seek(double seconds, double fromSeconds) {
    seekFromPosition_.store(fromSeconds, std::memory_order_relaxed);
    seekTarget_.store(seconds, std::memory_order_relaxed);
}

// 检查是否有待处理的 seek 请求。若有，调用 av_seek_frame 跳转到目标关键帧，
// 然后逐一 pop+free 清空两条队列中的残留包，避免解码线程消费过期数据。
// 使用 pop+free 而非 clear()，是因为 clear() 不释放队列中的 AVPacket* 指针。
// seek 后设置 seekExactTarget_，使 run() 跳过 PTS 低于目标的包，
// 实现从关键帧"快进解码"到精确目标位置，消除 GOP 粒度导致的跳转偏差。
void DemuxThread::handleSeek() {
    if (!fmtCtx_) { seekTarget_.store(-1.0, std::memory_order_relaxed); return; }

    double target  = seekTarget_.exchange(-1.0, std::memory_order_relaxed);
    if (target < 0.0) return;
    double fromPos = seekFromPosition_.exchange(-1.0, std::memory_order_relaxed);

    qInfo() << "DemuxThread::handleSeek target =" << target;

    // 按视频流的时间基计算 seek 时间戳，比用 AV_TIME_BASE 更精确
    int streamIdx = videoIdx_ >= 0 ? videoIdx_ : audioIdx_;
    if (streamIdx < 0) return;

    AVRational tb = fmtCtx_->streams[streamIdx]->time_base;
    int64_t ts = av_rescale_q((int64_t)(target * AV_TIME_BASE), AV_TIME_BASE_Q, tb);
    av_seek_frame(fmtCtx_, streamIdx, ts, AVSEEK_FLAG_BACKWARD);

    AVPacket* p;
    int vclear = 0, aclear = 0;
    while (videoQueue_->tryPop(p, 0)) { av_packet_free(&p); ++vclear; }
    while (audioQueue_->tryPop(p, 0)) { av_packet_free(&p); ++aclear; }
    qInfo() << "DemuxThread::handleSeek cleared" << vclear << "video" << aclear
            << "audio pkts from queues";

    // seek 后清空推流队列，并各推一个 nullptr sentinel：
    // MuxThread 收到 sentinel 后重置 PTS base，保证新位置的包从 0 重新计时。
    // 本地录制器调用 resetPtsBase 保持 PTS 连续（不停止录制）。
    {
        QMutexLocker lk(&restreamMtx_);
        int vsClear = 0, asClear = 0, vSentinelFail = 0, aSentinelFail = 0;
        for (auto* q : restreamVideoQueues_) {
            while (q->tryPop(p, 0)) { av_packet_free(&p); ++vsClear; }
            if (!q->tryPush(nullptr)) ++vSentinelFail;
        }
        for (auto* q : restreamAudioQueues_) {
            while (q->tryPop(p, 0)) { av_packet_free(&p); ++asClear; }
            if (!q->tryPush(nullptr)) ++aSentinelFail;
        }
        qInfo() << "DemuxThread::handleSeek sentinel pushed to"
                << restreamVideoQueues_.size() << "vQueues,"
                << restreamAudioQueues_.size() << "aQueues"
                << "(cleared" << vsClear << "vPkts," << asClear << "aPkts"
                << "recorders=" << localRecorders_.size()
                << "muxThreads=" << muxThreads_.size() << ")"
                << "sentinelFail v=" << vSentinelFail << "a=" << aSentinelFail;
        if (vSentinelFail || aSentinelFail)
            qWarning() << "DemuxThread::handleSeek sentinel tryPush FAILED"
                       << "vFail=" << vSentinelFail << "aFail=" << aSentinelFail
                       << "— MuxThread waitingForStart will never clear!";
        for (auto* m : muxThreads_) {
            m->setSuppressUntilKeyframe(target);
            qInfo() << "DemuxThread: setSuppressUntilKeyframe target=" << target
                    << "MuxThread=" << (void*)m;
        }
        for (auto* r : localRecorders_) r->resetPtsBase(fromPos, target);
    }

    // 设置精确目标，run() 读取到 >= target 的包之前会丢弃中间帧
    seekExactTarget_.store(target, std::memory_order_relaxed);
    seekExactAudioTarget_.store(target, std::memory_order_relaxed);
    logNextAudioPush_.store(true, std::memory_order_relaxed);
    qInfo() << "DemuxThread::handleSeek keyframe seek done, exact target =" << target;
}

// 解复用主循环：持续读取 AVPacket 并按流索引分发到对应队列。
// 每轮循环先处理 seek 请求，再读一个包，clone 后 push 进队列（clone 使所有权独立）。
// 遇到 EOF 或读取错误时退出；stop() 设置 abort_ 后下次循环检查时也会退出。
// seekExactTarget_ 活跃时丢弃 PTS 低于目标的包，实现关键帧到精确目标的快进。
void DemuxThread::run() {
    if (!fmtCtx_) return;

    AVPacket* pkt = av_packet_alloc();
    int64_t preTargetVideoCount = 0;     // 当前 seek 中已转发到网络推流队列的预目标视频帧数
    int64_t preTargetAudioDiscarded = 0; // 当前 seek 中丢弃的预目标音频帧数

    statsBytes_ = 0;
    statsVideoFrames_ = 0;
    statsTimer_.start();

    while (!abort_.load(std::memory_order_relaxed)) {
        handleSeek();

        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret == AVERROR_EOF || ret < 0) {
            if (ret == AVERROR_EOF) {
                qInfo() << "DemuxThread: EOF reached";
            } else {
                char errbuf[64];
                av_strerror(ret, errbuf, sizeof(errbuf));
                qWarning() << "DemuxThread: av_read_frame error:" << errbuf;
            }

            // 网络流（直播）读取中断不视为播放结束，尝试重连后继续读取；
            // 本地文件 EOF/错误仍按原逻辑结束线程
            if (isNetwork_ && !abort_.load(std::memory_order_relaxed)) {
                if (reconnect()) {
                    preTargetVideoCount = 0;
                    preTargetAudioDiscarded = 0;
                    statsBytes_ = 0;
                    statsVideoFrames_ = 0;
                    statsTimer_.restart();
                    continue;
                }
            }
            break;
        }

        // 在 clone 前再次检查 abort_，减少 abort 时的 push-drop 泄漏窗口
        if (abort_.load(std::memory_order_relaxed)) break;

        // 码率/帧率统计：累加字节数和视频包数，每满 1 秒发出一次 statsUpdated
        statsBytes_ += pkt->size;
        if (pkt->stream_index == videoIdx_) ++statsVideoFrames_;
        {
            qint64 elapsed = statsTimer_.elapsed();
            if (elapsed >= 1000) {
                int kbps = (int)(statsBytes_ * 8 / elapsed);
                double fps = statsVideoFrames_ * 1000.0 / elapsed;
                emit statsUpdated(kbps, fps);
                statsBytes_ = 0;
                statsVideoFrames_ = 0;
                statsTimer_.restart();
            }
        }

        // 精确 seek 过滤（GOP 重叠方案 — 本地录制从关键帧起全量写入）：
        // - 音频包：独立 exactAudioTarget 过滤，网络推流丢弃目标前音频；
        //   本地录制也写入目标前音频以维持 A/V 同步。
        // - 视频包：网络推流目标前标记 preTarget（tryPush 仍送入以保 GOP 链）；
        //   本地录制不跳帧，从关键帧起全部写入，保证 H.264 参考链完整不花屏。
        double exactTarget = seekExactTarget_.load(std::memory_order_relaxed);
        double exactAudioTarget = seekExactAudioTarget_.load(std::memory_order_relaxed);
        bool preTarget = false;  // 网络推流用：目标前标记
        if (pkt->pts != AV_NOPTS_VALUE) {
            AVRational* stb = &fmtCtx_->streams[pkt->stream_index]->time_base;
            double pktSec = pkt->pts * av_q2d(*stb);

            // 音频过滤：独立 exactAudioTarget，不被视频清除影响
            if (pkt->stream_index == audioIdx_ && exactAudioTarget >= 0.0) {
                if (pktSec < exactAudioTarget - 0.05) {
                    ++preTargetAudioDiscarded;
                    if (preTargetAudioDiscarded <= 3)
                        qInfo() << "DemuxThread: pre-target audio discarded for network #"
                                << preTargetAudioDiscarded
                                << "PTS=" << pktSec << "exactAudioTarget=" << exactAudioTarget;
                    // 本地录制不跳音频帧，保持 A/V 同步；网络队列仍丢弃
                    {
                        std::vector<LocalRecorder*> recCopy;
                        {
                            QMutexLocker lk(&restreamMtx_);
                            recCopy = localRecorders_;
                        }
                        for (auto* r : recCopy) r->writeAudioPacket(pkt);
                    }
                    av_packet_unref(pkt);
                    continue;
                } else {
                    seekExactAudioTarget_.store(-1.0, std::memory_order_relaxed);
                    qInfo() << "DemuxThread: audio target reached, PTS=" << pktSec
                            << "preTargetAudioDiscarded=" << preTargetAudioDiscarded;
                    preTargetAudioDiscarded = 0;
                }
            }

            // 视频过滤：网络推流标记 preTarget，本地录制不跳帧
            if (exactTarget >= 0.0 && pkt->stream_index == videoIdx_) {
                if (pktSec >= exactTarget - 0.05) {
                    seekExactTarget_.store(-1.0, std::memory_order_relaxed);
                    qInfo() << "DemuxThread: video target reached, PTS=" << pktSec
                            << "preTargetVideoFrames=" << preTargetVideoCount;
                    preTargetVideoCount = 0;
                } else {
                    preTarget = true;
                    ++preTargetVideoCount;
                    if (preTargetVideoCount <= 3)
                        qInfo() << "DemuxThread: pre-target video #" << preTargetVideoCount
                                << "PTS=" << pktSec << "exactTarget=" << exactTarget
                                << "(recorder writes all, network marks preTarget)";
                }
            }
        }

        if (pkt->stream_index == videoIdx_) {
            AVPacket* p = av_packet_clone(pkt);
            videoQueue_->push(p);
            std::vector<LocalRecorder*> recordersCopy;
            {
                QMutexLocker lk(&restreamMtx_);
                if (!restreamVideoQueues_.empty() || !localRecorders_.empty()) {
                    if (logNextRestreamVideo_.exchange(false, std::memory_order_relaxed)) {
                        AVRational* stb = &fmtCtx_->streams[pkt->stream_index]->time_base;
                        double pktSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(*stb) : -1.0;
                        qInfo() << "DemuxThread: first restream video fork PTS=" << pktSec
                                << "s (DemuxThread read-ahead position)";
                    }
                }
                for (auto* q : restreamVideoQueues_) {
                    AVPacket* copy = av_packet_clone(pkt);
                    if (!q->tryPush(copy)) {
                        av_packet_free(&copy);
                        ++restreamDropCount_;
                        if (restreamDropCount_ == 1 || restreamDropCount_ % 100 == 0)
                            qWarning() << "DemuxThread: restream tryPush drop #" << restreamDropCount_
                                       << "mux queue full — consider tcp_nodelay or larger queue";
                    }
                }
                if (preTarget && preTargetVideoCount <= 3 && !restreamVideoQueues_.empty()) {
                    double pktSec = (pkt->pts != AV_NOPTS_VALUE)
                        ? pkt->pts * av_q2d(fmtCtx_->streams[pkt->stream_index]->time_base) : -1.0;
                    qInfo() << "DemuxThread: pre-target video #" << preTargetVideoCount
                            << "PTS=" << pktSec
                            << "→ pushed to" << restreamVideoQueues_.size()
                            << "network queue(s), recorder also writes (GOP overlap)";
                }
                recordersCopy = localRecorders_;  // GOP 重叠：从关键帧起全量写入录制器
            }
            for (auto* r : recordersCopy) {
                if (!r->writeVideoPacket(pkt))
                    qInfo() << "DemuxThread: local recorder video write returned false, PTS="
                            << (pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(fmtCtx_->streams[pkt->stream_index]->time_base) : -1.0);
            }
        } else if (pkt->stream_index == audioIdx_) {
            if (logNextAudioPush_.exchange(false, std::memory_order_relaxed)) {
                AVRational* stb = &fmtCtx_->streams[pkt->stream_index]->time_base;
                double pts = (pkt->pts != AV_NOPTS_VALUE)
                    ? pkt->pts * av_q2d(*stb) : -1.0;
                qInfo() << "DemuxThread: first audio push after seek, PTS=" << pts;
            }
            AVPacket* p = av_packet_clone(pkt);
            audioQueue_->push(p);
            std::vector<LocalRecorder*> recordersCopy;
            {
                QMutexLocker lk(&restreamMtx_);
                for (auto* q : restreamAudioQueues_) {
                    AVPacket* copy = av_packet_clone(pkt);
                    if (!q->tryPush(copy)) av_packet_free(&copy);
                }
                recordersCopy = localRecorders_;  // GOP 重叠：音频也全量写入保持 A/V 同步
            }
            for (auto* r : recordersCopy) {
                if (!r->writeAudioPacket(pkt))
                    qInfo() << "DemuxThread: local recorder audio write returned false, PTS="
                            << (pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(fmtCtx_->streams[pkt->stream_index]->time_base) : -1.0);
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

// 网络流读取出错或断开时调用：关闭旧连接，按固定间隔反复尝试重新打开同一 URL，
// 直到成功或 abort() 被调用（用户点击"断开"/stop()）。成功后重新探测流信息和流索引。
// 返回 false 表示因 abort 放弃重连，run() 据此退出循环结束线程。
bool DemuxThread::reconnect() {
    emit networkStateChanged(static_cast<int>(NetworkState::Reconnecting));
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
    videoIdx_ = -1;
    audioIdx_ = -1;

    constexpr int kRetryIntervalMs = 2000;
    while (!abort_.load(std::memory_order_relaxed)) {
        AVDictionary* opts = buildNetworkOptions(url_, isNetwork_);
        int ret = avformat_open_input(&fmtCtx_, url_.toUtf8().constData(), nullptr, &opts);
        av_dict_free(&opts);

        if (ret == 0 && avformat_find_stream_info(fmtCtx_, nullptr) >= 0) {
            for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
                auto type = fmtCtx_->streams[i]->codecpar->codec_type;
                if (type == AVMEDIA_TYPE_VIDEO && videoIdx_ < 0) videoIdx_ = (int)i;
                if (type == AVMEDIA_TYPE_AUDIO && audioIdx_ < 0) audioIdx_ = (int)i;
            }
            duration_ = fmtCtx_->duration;
            qInfo() << "DemuxThread: reconnect succeeded" << url_
                    << "videoIdx=" << videoIdx_ << "audioIdx=" << audioIdx_;
            emit networkStateChanged(static_cast<int>(NetworkState::Connected));
            return true;
        }

        if (fmtCtx_) avformat_close_input(&fmtCtx_);
        qWarning() << "DemuxThread: reconnect attempt failed, retry in" << kRetryIntervalMs << "ms";

        // 等待重试间隔，期间分段检查 abort，便于用户点击"断开"后立即退出
        for (int waited = 0; waited < kRetryIntervalMs && !abort_.load(std::memory_order_relaxed); waited += 100)
            QThread::msleep(100);
    }

    qInfo() << "DemuxThread: reconnect aborted by stop()";
    emit networkStateChanged(static_cast<int>(NetworkState::Disconnected));
    return false;
}
