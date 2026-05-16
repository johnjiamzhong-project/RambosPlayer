#include "videorenderer.h"
#include "logger.h"
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
    srcFormat_ = AV_PIX_FMT_YUV420P;  // 默认软解格式，硬解首次收帧时自动切换
    swsCtx_ = sws_getContext(width, height, srcFormat_,
                              width, height, AV_PIX_FMT_RGB32,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    currentFrame_ = QImage(width, height, QImage::Format_RGB32);
}

// 启动/停止 1ms 定时器，控制帧拉取循环的运行状态。
// stopRendering 同时释放暂存帧，防止残留帧跨文件泄漏。
void VideoRenderer::startRendering() {
    noFrameTimer_.restart();
    noFrameTimerStarted_ = true;
    noFrameLogged_ = false;
    dropCount_ = 0;
    timer_->start();
}

// seek 时由 PlayerController 调用，释放暂存的旧帧，避免旧 PTS 卡住帧队列消费。
// 同时重置无帧计时器，使 seek 后能立即开始计量新的空窗期。
void VideoRenderer::flushPendingFrame() {
    if (pendingFrame_) av_frame_free(&pendingFrame_);
    noFrameTimer_.restart();
    noFrameTimerStarted_ = true;
    noFrameLogged_ = false;
    dropCount_ = 0;
}

void VideoRenderer::stopRendering()  {
    timer_->stop();
    if (pendingFrame_) av_frame_free(&pendingFrame_);
}

// seek while paused 后由 PlayerController 通过 QTimer::singleShot 延迟调用，
// 直接触发一次 onTimer() 把新位置的帧渲染出来，无需重启定时器。
void VideoRenderer::renderOneFrame() { onTimer(); }

// 定时回调：音视频同步的核心决策点。
// 优先检查暂存帧，再从队列取帧；帧到得太早时暂存而非阻塞主线程。
void VideoRenderer::onTimer() {
    AVFrame* frame = pendingFrame_;
    pendingFrame_ = nullptr;

    if (!frame) {
        if (!frameQueue_ || !frameQueue_->tryPop(frame, 0)) {
            // 队列持续为空：每 500ms 打印一次警告，帮助定位 seek 后的视频停顿
            if (noFrameTimerStarted_ && noFrameTimer_.elapsed() > 500 && !noFrameLogged_) {
                qInfo() << "VideoRenderer: no frame for" << noFrameTimer_.elapsed()
                        << "ms (queue empty, dropCount=" << dropCount_ << ")";
                noFrameLogged_ = true;
            }
            return;
        }
    }

    double pts = (frame->pts != AV_NOPTS_VALUE)
                 ? frame->pts * av_q2d(timeBase_) : 0.0;
    double audioClock = sync_ ? sync_->audioClock() : -1.0;
    double diff = (audioClock >= 0.0) ? pts - audioClock : 0.0;

    qCDebug(lcVideo) << "pts =" << pts
                     << "audioClock =" << audioClock
                     << "diff =" << diff;

    // 视频落后超过 400ms：丢弃（正向 seek 残留帧或解码太慢的帧）
    if (audioClock >= 0.0 && diff < -0.4) {
        ++dropCount_;
        // 第 1 帧、第 30 帧、之后每 60 帧打印一次，避免日志爆炸
        if (dropCount_ == 1 || dropCount_ == 30 || dropCount_ % 60 == 0)
            qInfo() << "VideoRenderer: drop late frame #" << dropCount_
                    << "pts=" << pts << "clock=" << audioClock << "diff=" << diff;
        av_frame_free(&frame);
        return;
    }

    // 视频超前超过 5 秒：丢弃（向后 seek 后残留的旧位置帧，
    // audioClock 被 seek 设回低位，旧帧 PTS 远大于新时钟，会被当成"超前"卡死 pendingFrame_）
    if (audioClock >= 0.0 && diff > 5.0) {
        qInfo() << "VideoRenderer: drop stale-ahead frame pts=" << pts
                << "clock=" << audioClock << "diff=" << diff;
        av_frame_free(&frame);
        return;
    }

    // 视频超前：暂存等下次 timer 再检查。
    // 若已有暂存帧，保留更接近时钟的一帧；另一帧释放。
    if (audioClock >= 0.0 && diff > 0.0) {
        if (pendingFrame_) {
            double pPts = (pendingFrame_->pts != AV_NOPTS_VALUE)
                          ? pendingFrame_->pts * av_q2d(timeBase_) : 0.0;
            double pDiff = pPts - audioClock;
            if (diff < pDiff) {
                qCDebug(lcVideo) << "VideoRenderer: swap pending frame pts" << pts << "for closer pts" << pPts;
                av_frame_free(&pendingFrame_);
                pendingFrame_ = frame;
            } else {
                av_frame_free(&frame);
            }
        } else {
            qCDebug(lcVideo) << "VideoRenderer: buffer pending frame pts" << pts << "diff" << diff;
            pendingFrame_ = frame;
        }
        return;
    }

    // 硬解路径下帧格式可能从 YUV420P 变为 NV12，检测到变化时重建 sws 上下文
    if (frame->format != srcFormat_ && frame->format != AV_PIX_FMT_NONE) {
        srcFormat_ = (AVPixelFormat)frame->format;
        if (swsCtx_) sws_freeContext(swsCtx_);
        swsCtx_ = sws_getContext(srcW_, srcH_, srcFormat_,
                                  srcW_, srcH_, AV_PIX_FMT_RGB32,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    // seek 后首帧 / 长时间无帧后恢复：打印诊断信息
    bool isFirstAfterGap = noFrameLogged_ ||
                           (lastRenderedPts_ >= 0.0 && qAbs(pts - lastRenderedPts_) > 1.0) ||
                           lastRenderedPts_ < 0.0;
    if (isFirstAfterGap) {
        qInfo() << "VideoRenderer: render frame pts=" << pts
                << "clock=" << audioClock << "diff=" << diff
                << "(after" << noFrameTimer_.elapsed() << "ms, drops=" << dropCount_ << ")";
    }

    // 重置计数器，为下次空窗/seek 做准备
    dropCount_ = 0;
    noFrameLogged_ = false;
    noFrameTimer_.restart();
    lastRenderedPts_ = pts;

    uint8_t* dst[1]  = { currentFrame_.bits() };
    int dstStride[1] = { currentFrame_.bytesPerLine() };
    {
        QMutexLocker lk(&frameMutex_);
        if (swsCtx_) {
            sws_scale(swsCtx_,
                      (const uint8_t* const*)frame->data, frame->linesize,
                      0, srcH_, dst, dstStride);
        }
    }
    qCDebug(lcVideo) << "VideoRenderer: render frame pts" << pts << "format" << frame->format;
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
