// src/demuxthread.h
#pragma once
#include <QThread>
#include <QMutex>
#include <atomic>
#include <vector>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
}

class LocalRecorder;
class MuxThread;

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
    // 秒；原子存储，run() 下次循环顶部生效。
    // fromSeconds 为 seek 前播放器显示位置，传给本地录制器精确截断超前帧。
    void seek(double seconds, double fromSeconds = -1.0);

    // 推流分叉接口（-c copy 模式）：网络推流用 tryPush 丢帧不阻塞播放
    void addRestreamVideoQueue(FrameQueue<AVPacket*>* q);
    void addRestreamAudioQueue(FrameQueue<AVPacket*>* q);
    // 本地录制接口：DemuxThread 直接写 FLV，不用队列避免超前读
    void addLocalRecorder(LocalRecorder* r);
    void addMuxThread(MuxThread* m);  // seek 时通知 MuxThread 进入抑制期
    void clearRestreamQueues();   // 中止并清空所有分叉队列和本地录制器

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
    std::atomic<double>    seekTarget_{-1.0};        // seek 目标（秒），-1 表示无待处理 seek
    std::atomic<double>    seekFromPosition_{-1.0}; // seek 前播放器显示位置，用于录制器精确截断超前帧
    std::atomic<double>    seekExactTarget_{-1.0};       // 视频精确 seek 目标（秒），视频帧到达时清除
    std::atomic<double>    seekExactAudioTarget_{-1.0};  // 音频精确 seek 目标（秒），独立于视频，音频帧到达时清除
    std::atomic<bool>      logNextAudioPush_{false}; // seek 完成后打印第一个推入音频包的 PTS

    // 推流分叉（受 restreamMtx_ 保护，run() 持读侧，add/clear 持写侧）
    mutable QMutex                          restreamMtx_;
    std::vector<FrameQueue<AVPacket*>*>     restreamVideoQueues_;    // tryPush（网络推流）
    std::vector<FrameQueue<AVPacket*>*>     restreamAudioQueues_;
    std::vector<LocalRecorder*>             localRecorders_;        // 本地录制（直接写）
    std::vector<MuxThread*>                 muxThreads_;            // 网络推流线程（seek 抑制通知）
    std::atomic<bool>                       logNextRestreamVideo_{false};

    void handleSeek();  // 在 run() 每次循环顶部检查并执行 seek
};
