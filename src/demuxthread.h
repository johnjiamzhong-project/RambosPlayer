// src/demuxthread.h
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
}

// 解复用线程：在独立线程中循环调用 av_read_frame 读取媒体文件，
// 按流索引将 AVPacket* 分发到视频队列（videoQueue）和音频队列（audioQueue）。
// 使用方式：open() → start() → （播放中可调 seek()）→ stop() + wait()。
// 注意：每个实例只应调用一次 open()；stop() 通过 abort 标志和队列唤醒安全退出线程。
class DemuxThread : public QThread {
    Q_OBJECT
public:
    ~DemuxThread() override;

    // 失败时返回 false；AVFormatContext 已清理，可安全析构。
    bool open(const QString& path,
              FrameQueue<AVPacket*>* videoQueue,
              FrameQueue<AVPacket*>* audioQueue);
    void stop();
    void seek(double seconds);  // 秒；原子存储，run() 下次循环顶部生效

    int64_t duration()       const { return duration_; }   // 微秒
    int videoStreamIdx()     const { return videoIdx_; }
    int audioStreamIdx()     const { return audioIdx_; }
    // 仅在 open() 成功后到析构前有效；调用方不得释放。
    AVFormatContext* formatContext() const { return fmtCtx_; }

protected:
    void run() override;

private:
    AVFormatContext*       fmtCtx_     = nullptr;  // 媒体文件上下文，open() 分配，析构时释放
    FrameQueue<AVPacket*>* videoQueue_ = nullptr;  // 视频包输出队列，由外部持有
    FrameQueue<AVPacket*>* audioQueue_ = nullptr;  // 音频包输出队列，由外部持有
    int                    videoIdx_   = -1;        // 视频流在 fmtCtx_ 中的索引，-1 表示无视频流
    int                    audioIdx_   = -1;        // 音频流在 fmtCtx_ 中的索引，-1 表示无音频流
    int64_t                duration_   = 0;         // 文件总时长，单位微秒（AV_TIME_BASE）
    std::atomic<bool>      abort_{false};           // stop() 置 true，run() 循环检查后退出
    std::atomic<double>    seekTarget_{-1.0};       // seek 目标（秒），-1 表示无待处理 seek

    void handleSeek();  // 在 run() 每次循环顶部检查并执行 seek
};
