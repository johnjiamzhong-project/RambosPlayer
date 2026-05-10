#include "videorenderer.h"
#include <QLoggingCategory>
#include <QPainter>
#include <QThread>

Q_LOGGING_CATEGORY(lcVideo, "rambos.video", QtWarningMsg)

extern "C" {
#include <libavutil/imgutils.h>
}

// 构造函数：设置黑色背景，创建 1ms QTimer 并连接到 onTimer()。
VideoRenderer::VideoRenderer(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setStyleSheet("background: black;");
    timer_ = new QTimer(this);
    timer_->setInterval(1);
    connect(timer_, &QTimer::timeout, this, &VideoRenderer::onTimer);
}

// 析构函数：停止定时器，释放暂存帧和 sws 上下文。
VideoRenderer::~VideoRenderer() {
    stopRendering();
    if (swsCtx_) sws_freeContext(swsCtx_);
}

// 初始化渲染参数：记录视频宽高和时间基，绑定 AVSync 与帧队列，
// 创建 sws 上下文（YUV420P → RGB32）并分配 QImage 缓冲区。
// 重新打开文件时会再次调用，需释放上一轮暂存的帧。
void VideoRenderer::init(int width, int height, AVRational timeBase,
                          AVSync* sync, FrameQueue<AVFrame*>* frameQueue) {
    if (pendingFrame_) av_frame_free(&pendingFrame_);
    srcW_ = width; srcH_ = height;
    timeBase_ = timeBase;
    sync_ = sync;
    frameQueue_ = frameQueue;
    swsCtx_ = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                              width, height, AV_PIX_FMT_RGB32,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    currentFrame_ = QImage(width, height, QImage::Format_RGB32);
}

// 启动/停止 1ms 定时器，控制帧拉取循环的运行状态。
// stopRendering 同时释放暂存帧，防止残留帧跨文件泄漏。
void VideoRenderer::startRendering() { timer_->start(); }

// seek 时由 PlayerController 调用，释放暂存的旧帧，避免旧 PTS 卡住帧队列消费。
void VideoRenderer::flushPendingFrame() {
    if (pendingFrame_) av_frame_free(&pendingFrame_);
}

void VideoRenderer::stopRendering()  {
    timer_->stop();
    if (pendingFrame_) av_frame_free(&pendingFrame_);
}

// 定时回调：音视频同步的核心决策点。
// 优先检查暂存帧，再从队列取帧；帧到得太早时暂存而非阻塞主线程。
void VideoRenderer::onTimer() {
    AVFrame* frame = pendingFrame_;
    pendingFrame_ = nullptr;

    if (!frame) {
        if (!frameQueue_ || !frameQueue_->tryPop(frame, 0)) return;
    }

    double pts = (frame->pts != AV_NOPTS_VALUE)
                 ? frame->pts * av_q2d(timeBase_) : 0.0;
    double audioClock = sync_ ? sync_->audioClock() : -1.0;
    double diff = (audioClock >= 0.0) ? pts - audioClock : 0.0;

    qCDebug(lcVideo) << "pts =" << pts
                     << "audioClock =" << audioClock
                     << "diff =" << diff;

    // 视频严重落后（正向 seek 后旧帧）：丢弃不渲染，否则淹没主线程导致死锁。
    if (audioClock >= 0.0 && diff < -0.4) {
        av_frame_free(&frame);
        return;
    }

    // 视频严重超前（反向 seek 后旧帧 PTS 远大于新 audioClock）：同样丢弃。
    // 阈值 1.5s：正常播放 diff 不超过 0.5s，超过 1.5s 必为 seek 残留的旧帧。
    if (audioClock >= 0.0 && diff > 1.5) {
        av_frame_free(&frame);
        return;
    }

    // 视频轻微超前：暂存等下次 timer 再检查，不阻塞主线程
    if (diff > 0.0) {
        pendingFrame_ = frame;
        return;
    }

    uint8_t* dst[1]  = { currentFrame_.bits() };
    int dstStride[1] = { currentFrame_.bytesPerLine() };
    {
        QMutexLocker lk(&frameMutex_);
        sws_scale(swsCtx_,
                  (const uint8_t* const*)frame->data, frame->linesize,
                  0, srcH_, dst, dstStride);
    }
    av_frame_free(&frame);
    update();
}

// Qt 绘制回调：用 QPainter 将 currentFrame_ 保持宽高比居中绘制到 widget，无帧时填黑。
void VideoRenderer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QMutexLocker lk(&frameMutex_);
    if (!currentFrame_.isNull()) {
        QRect r = rect();
        double aspect = (double)srcW_ / srcH_;
        int w = r.width(), h = (int)(w / aspect);
        if (h > r.height()) { h = r.height(); w = (int)(h * aspect); }
        int x = (r.width() - w) / 2, y = (r.height() - h) / 2;
        p.fillRect(r, Qt::black);
        p.drawImage(QRect(x, y, w, h), currentFrame_);
    } else {
        p.fillRect(rect(), Qt::black);
    }
}
