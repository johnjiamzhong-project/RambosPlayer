// src/demuxthread.cpp
#include "demuxthread.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <QtDebug>

// 先 stop() 通知线程退出，再 wait() 等线程结束，最后释放 AVFormatContext。
// 顺序不能颠倒：必须确保 run() 不再访问 fmtCtx_ 后才能释放它。
DemuxThread::~DemuxThread() {
    stop();
    wait();
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
}

// 打开媒体文件，探测流信息，记录第一条视频流和音频流的索引。
// 任一步骤失败均返回 false，并确保 fmtCtx_ 已释放（调用方可直接析构）。
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

// 设置 abort_ 标志，并对两条队列调用 abort()，
// 唤醒任何阻塞在 push/tryPop 的线程，使 run() 能在下次循环检查时退出。
void DemuxThread::stop() {
    abort_.store(true, std::memory_order_relaxed);
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

// 将 seek 目标原子写入 seekTarget_；实际跳转在 run() 下次循环顶部的 handleSeek() 中执行。
void DemuxThread::seek(double seconds) {
    seekTarget_.store(seconds, std::memory_order_relaxed);
}

// 检查是否有待处理的 seek 请求。若有，调用 av_seek_frame 跳转到目标关键帧，
// 然后逐一 pop+free 清空两条队列中的残留包，避免解码线程消费过期数据。
// 使用 pop+free 而非 clear()，是因为 clear() 不释放队列中的 AVPacket* 指针。
void DemuxThread::handleSeek() {
    if (!fmtCtx_) { seekTarget_.store(-1.0, std::memory_order_relaxed); return; }

    double target = seekTarget_.exchange(-1.0, std::memory_order_relaxed);
    if (target < 0.0) return;

    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);

    AVPacket* p;
    while (videoQueue_->tryPop(p, 0)) av_packet_free(&p);
    while (audioQueue_->tryPop(p, 0)) av_packet_free(&p);
}

// 解复用主循环：持续读取 AVPacket 并按流索引分发到对应队列。
// 每轮循环先处理 seek 请求，再读一个包，clone 后 push 进队列（clone 使所有权独立）。
// 遇到 EOF 或读取错误时退出；stop() 设置 abort_ 后下次循环检查时也会退出。
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
