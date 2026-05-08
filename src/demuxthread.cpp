// src/demuxthread.cpp
#include "demuxthread.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <QtDebug>

DemuxThread::~DemuxThread() {
    stop();
    wait();
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
}

bool DemuxThread::open(const QString& path,
                        FrameQueue<AVPacket*>* videoQueue,
                        FrameQueue<AVPacket*>* audioQueue) {
    if (avformat_open_input(&fmtCtx_,
                            path.toUtf8().constData(),
                            nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0) {
        avformat_close_input(&fmtCtx_);  // 防止 fmtCtx_ 泄漏
        return false;
    }

    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        auto type = fmtCtx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoIdx_ < 0) videoIdx_ = (int)i;
        if (type == AVMEDIA_TYPE_AUDIO && audioIdx_ < 0) audioIdx_ = (int)i;
    }

    duration_   = fmtCtx_->duration;
    videoQueue_ = videoQueue;
    audioQueue_ = audioQueue;
    return true;
}

void DemuxThread::stop() {
    abort_.store(true, std::memory_order_relaxed);
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

void DemuxThread::seek(double seconds) {
    seekTarget_.store(seconds, std::memory_order_relaxed);
}

void DemuxThread::handleSeek() {
    if (!fmtCtx_) { seekTarget_.store(-1.0, std::memory_order_relaxed); return; }

    double target = seekTarget_.exchange(-1.0, std::memory_order_relaxed);
    if (target < 0.0) return;

    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);

    // pop+free：不能只调 clear()，会泄漏 AVPacket*
    AVPacket* p;
    while (videoQueue_->tryPop(p, 0)) av_packet_free(&p);
    while (audioQueue_->tryPop(p, 0)) av_packet_free(&p);
}

void DemuxThread::run() {
    AVPacket* pkt = av_packet_alloc();

    while (!abort_.load(std::memory_order_relaxed)) {
        handleSeek();

        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[64];
            av_strerror(ret, errbuf, sizeof(errbuf));
            qWarning() << "DemuxThread: av_read_frame error:" << errbuf;
            break;
        }

        // 在 clone 前再次检查 abort_，减少 abort 时的 push-drop 泄漏窗口
        if (abort_.load(std::memory_order_relaxed)) break;

        if (pkt->stream_index == videoIdx_) {
            AVPacket* p = av_packet_clone(pkt);
            videoQueue_->push(p);
        } else if (pkt->stream_index == audioIdx_) {
            AVPacket* p = av_packet_clone(pkt);
            audioQueue_->push(p);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}
