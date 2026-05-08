// src/demuxthread.h
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
}

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
    AVFormatContext*       fmtCtx_     = nullptr;
    FrameQueue<AVPacket*>* videoQueue_ = nullptr;
    FrameQueue<AVPacket*>* audioQueue_ = nullptr;
    int                    videoIdx_   = -1;
    int                    audioIdx_   = -1;
    int64_t                duration_   = 0;
    std::atomic<bool>      abort_{false};
    std::atomic<double>    seekTarget_{-1.0};

    void handleSeek();
};
