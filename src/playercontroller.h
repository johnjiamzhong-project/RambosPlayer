#pragma once
#include <QObject>
#include <atomic>
#include "framequeue.h"
#include "avsync.h"
#include "demuxthread.h"
#include "videodecodethread.h"
#include "audiodecodethread.h"

class QTimer;

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoRenderer;

// PlayerController 是播放器的总控制器，持有并管理所有组件的生命周期。
// 对外暴露 open/play/pause/stop/seek/setVolume 接口；
// 内部通过三条 FrameQueue 串联 DemuxThread → 解码线程 → VideoRenderer，
// 以 100ms QTimer 轮询 AVSync 音频时钟向外发出 positionChanged 信号。
class PlayerController : public QObject {
    Q_OBJECT
public:
    explicit PlayerController(VideoRenderer* renderer, QObject* parent = nullptr);
    ~PlayerController() override;

    bool open(const QString& path);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setVolume(float v);    // 0.0–1.0

    int64_t duration() const;   // 毫秒
    bool isPlaying() const { return playing_; }

signals:
    void durationChanged(int64_t ms);
    void positionChanged(int64_t ms);   // 100ms 间隔
    void playbackFinished();

private slots:
    void onDemuxFinished();
    void updatePosition();

private:
    VideoRenderer* renderer_;                       // 视频渲染组件，由外部传入，不拥有所有权
    AVSync sync_;                                   // 音频主时钟
    // 队列必须在线程成员之前声明，保证析构时队列比线程活得更久。
    // C++ 成员析构顺序为声明逆序：若队列在线程之后声明，
    // 队列先被销毁，线程析构时调 stop()->abort() 会访问已析构的队列（UAF 崩溃）。
    FrameQueue<AVPacket*> videoPacketQ_{100};       // 视频包队列
    FrameQueue<AVPacket*> audioPacketQ_{400};       // 音频包队列（更大以应对音频帧小而密）
    FrameQueue<AVFrame*>  videoFrameQ_{15};         // 解码后视频帧队列
    DemuxThread demux_;                             // 解复用线程
    VideoDecodeThread videoDec_;                    // 视频解码线程
    AudioDecodeThread audioDec_;                    // 音频解码线程
    QTimer* posTimer_ = nullptr;                    // 100ms 定时器，驱动 positionChanged 信号
    std::atomic<bool> playing_{false};              // 播放状态，防止重复 play()

    void stopAllThreads();
};
