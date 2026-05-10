#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include "framequeue.h"
#include "avsync.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// VideoRenderer 是视频渲染组件，继承 QWidget。
// 由 1ms QTimer 驱动 onTimer()，每次从 FrameQueue<AVFrame*> 取一帧，
// 查询 AVSync::videoDelay() 决定等待或丢弃，然后用 sws_scale 将
// YUV420P 转换为 RGB32 写入 QImage，并调用 update() 触发 paintEvent()。
// paintEvent() 用 QPainter 保持宽高比居中绘制，背景填充黑色。
class VideoRenderer : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderer(QWidget* parent = nullptr);
    ~VideoRenderer() override;

    void init(int width, int height, AVRational timeBase,
              AVSync* sync, FrameQueue<AVFrame*>* frameQueue);
    void startRendering();
    void stopRendering();
    void flushPendingFrame();   // seek 时清除残留的 pendingFrame_，防止旧帧卡住队列

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTimer();

private:
    QImage currentFrame_;               // 当前待绘制的 RGB32 帧
    QMutex frameMutex_;                 // 保护 currentFrame_ 的读写
    QTimer* timer_ = nullptr;           // 1ms 定时器，驱动帧拉取
    AVSync* sync_ = nullptr;            // 音频主时钟，用于计算视频延迟
    FrameQueue<AVFrame*>* frameQueue_ = nullptr; // 来自 VideoDecodeThread 的帧队列
    SwsContext* swsCtx_ = nullptr;      // sws 上下文，YUV420P → RGB32
    int srcW_ = 0, srcH_ = 0;          // 视频原始宽高，用于宽高比计算
    AVRational timeBase_{1, 1};         // 视频流时间基，用于 pts → 秒换算
    AVFrame* pendingFrame_ = nullptr;   // 未到渲染时间的帧，暂存避免推回队列阻塞主线程
};
