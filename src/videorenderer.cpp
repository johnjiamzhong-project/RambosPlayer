#include "videorenderer.h"
#include "logger.h"
#include <QLoggingCategory>
#include <QPainter>
#include <QThread>
#include <QDateTime>

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

// 启动看门狗线程：每 300ms 检查一次 lastOnTimerEpochMs_ 距现在过了多久。
// 用独立线程而非 Qt 定时器，是因为如果改用 GUI 线程上的另一个 QTimer，
// 它会和 onTimer() 一样受同样的系统限流影响，测不出问题；后台 QThread
// 的调度不依赖 GUI 线程的消息泵，能更可靠地反映 onTimer() 是否真的停摆。
void VideoRenderer::startWatchdog() {
    if (watchdog_) return;
    lastOnTimerEpochMs_.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    watchdog_ = QThread::create([this] {
        bool stalled = false;
        qint64 stallStartMs = 0;
        while (!QThread::currentThread()->isInterruptionRequested()) {
            QThread::msleep(300);
            qint64 now  = QDateTime::currentMSecsSinceEpoch();
            qint64 last = lastOnTimerEpochMs_.load(std::memory_order_relaxed);
            qint64 gap  = now - last;
            if (!stalled && gap > 1000) {
                stalled = true;
                stallStartMs = last;
                qWarning() << "VideoRenderer::watchdog onTimer 已停止被调度，距上次调用已过去"
                           << gap << "ms（GUI 线程的 1ms 定时器可能被系统限流："
                           << "窗口最小化/被其他窗口覆盖/失去前台焦点）";
            } else if (stalled && gap <= 1000) {
                qInfo() << "VideoRenderer::watchdog onTimer 恢复调度，本次停摆约"
                        << (now - stallStartMs) << "ms";
                stalled = false;
            }
        }
    });
    watchdog_->start();
}

// 停止看门狗线程：请求中断后等待退出（最长 1s）并释放。
void VideoRenderer::stopWatchdog() {
    if (!watchdog_) return;
    watchdog_->requestInterruption();
    watchdog_->wait(1000);
    delete watchdog_;
    watchdog_ = nullptr;
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
    livePacingStartPts_ = -1.0;
    reconnectRequested_ = false;
    driftAnchorPts_ = -1.0;
    srcFormat_ = AV_PIX_FMT_YUV420P;  // 默认软解格式，硬解首次收帧时自动切换
    swsCtx_ = sws_getContext(width, height, srcFormat_,
                              width, height, AV_PIX_FMT_RGB32,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    currentFrame_ = QImage(width, height, QImage::Format_RGB32);
    currentFrame_.fill(Qt::black);
}

// 启动/停止 1ms 定时器，控制帧拉取循环的运行状态。
// stopRendering 同时释放暂存帧，防止残留帧跨文件泄漏。
void VideoRenderer::startRendering() {
    noFrameTimer_.restart();
    noFrameTimerStarted_ = true;
    noFrameLogged_ = false;
    dropCount_ = 0;
    livePacingStartPts_ = -1.0;
    reconnectRequested_ = false;
    driftAnchorPts_ = -1.0;
    startWatchdog();
    timer_->start();
}

// seek 时由 PlayerController 调用，释放暂存的旧帧，并排空帧队列中残留的旧帧。
// pause() 后 VideoDecodeThread 继续运行会把旧位置的帧填满 videoFrameQ_；
// 若不在此处排空，这些帧 seek 后进入 pendingFrame_ 会冻住画面 N 秒（N = 旧帧 PTS）。
void VideoRenderer::flushPendingFrame() {
    if (pendingFrame_) av_frame_free(&pendingFrame_);
    if (frameQueue_) {
        AVFrame* tmp;
        while (frameQueue_->tryPop(tmp, 0)) av_frame_free(&tmp);
    }
    noFrameTimer_.restart();
    noFrameTimerStarted_ = true;
    noFrameLogged_ = false;
    dropCount_ = 0;
    livePacingStartPts_ = -1.0;
    reconnectRequested_ = false;
    driftAnchorPts_ = -1.0;
}

void VideoRenderer::stopRendering()  {
    timer_->stop();
    stopWatchdog();
    if (pendingFrame_) av_frame_free(&pendingFrame_);
}

// seek while paused 后由 PlayerController 通过 QTimer::singleShot 延迟调用，
// 直接触发一次 onTimer() 把新位置的帧渲染出来，无需重启定时器。
void VideoRenderer::renderOneFrame() { onTimer(); }

// 定时回调：音视频同步的核心决策点。
// 优先检查暂存帧，再从队列取帧；帧到得太早时暂存而非阻塞主线程。
void VideoRenderer::onTimer() {
    // 心跳：不管本次有没有帧可渲染，只要 onTimer 被调度到就更新，供 watchdog 线程判断
    // GUI 线程的 1ms 定时器是否还在正常工作（而不是"队列恰好空了"这种正常情况）。
    lastOnTimerEpochMs_.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);

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

    // 纯视频直播节拍控制（无音频时钟时启用）：
    // SRS 按 GOP 批量投递帧，若不加速率控制，8 帧会在 <10ms 内瞬间渲完，
    // 然后冻屏 ~533ms 等下一 GOP，视觉上呈现"一帧闪过→冻住"的规律性卡顿。
    //
    // 采用"短程基准"：以上一帧渲染时刻为锚点，而非全局第一帧。
    // 每帧只等一个帧间隔（≈67ms），不会因编码器帧率与 PTS timebase 的微小
    // 偏差（≤1%）导致 expectedMs-actualMs 随时间累积，引发延迟漂移。
    if (audioClock < 0.0) {
        if (livePacingStartPts_ >= 0.0) {
            double expectedMs = (pts - livePacingStartPts_) * 1000.0;
            qint64 actualMs   = livePacingTimer_.elapsed();
            double holdMs = expectedMs - actualMs;

            // 落后追赶：actualMs 远大于 expectedMs 说明自上一帧渲染以来，
            // 这段挂钟时间里 onTimer 没有正常被调度（例如窗口被最小化导致
            // GUI 线程的 1ms 定时器被系统限流），期间上游网络/解码把帧积压在了
            // 服务端/系统缓冲区里。此时不能按 1x 速度把积压逐帧播完（那样的话
            // 窗口最小化多久，恢复后就要多花同样的时长才能追上直播），而是直接
            // 丢弃队列里所有积压的旧帧，跳到最新一帧立即渲染。
            double behindSec = (actualMs - expectedMs) / 1000.0;
            if (behindSec > kCatchUpBehindSec) {
                AVFrame* newer;
                int skipped = 0;
                while (frameQueue_ && frameQueue_->tryPop(newer, 0)) {
                    av_frame_free(&frame);
                    frame = newer;
                    ++skipped;
                }
                pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(timeBase_) : pts;
                if (skipped > 0) {
                    qInfo() << "VideoRenderer: catching up to live, behind=" << behindSec
                            << "s, skipped" << skipped << "buffered frame(s), new pts=" << pts;
                }
                // 落后太多：本地这几帧（frameQueue_ 只有 10 帧）根本追不完堆在
                // 服务端/系统缓冲区里看不见的积压，发一次重连请求让 PlayerController
                // 触发 DemuxThread 整条流重连，直接拿最新 GOP；reconnectRequested_
                // 防止追赶期间（可能持续多个 1ms tick）反复发同一个请求。
                if (behindSec > kForceReconnectBehindSec && !reconnectRequested_) {
                    reconnectRequested_ = true;
                    qWarning() << "VideoRenderer: behind live by" << behindSec << "s, exceeds"
                               << kForceReconnectBehindSec << "s threshold, requesting stream reconnect";
                    emit fellBehindLive(behindSec);
                }
            } else if (holdMs > 2.0) {
                pendingFrame_ = frame;
                return;
            } else {
                reconnectRequested_ = false; // 已追平，允许下次落后时再次触发重连请求

                // 长期漂移检测：每 kDriftCheckWindowSec 重新校准一次独立基准，
                // 用来发现"每帧只差一点点、但持续累积"的缓慢漂移——这种漂移
                // 永远不会让上面的 behindSec（相对上一帧）超过 kCatchUpBehindSec，
                // 但攒的时间足够长（比如窗口被最小化的整段时间）还是会变成
                // 看得见的延迟。
                if (driftAnchorPts_ < 0.0 || driftAnchorTimer_.elapsed() > kDriftCheckWindowSec * 1000.0) {
                    driftAnchorTimer_.start();
                    driftAnchorPts_ = pts;
                } else {
                    double expectedPts = driftAnchorPts_ + driftAnchorTimer_.elapsed() / 1000.0;
                    double longTermBehindSec = expectedPts - pts;
                    if (longTermBehindSec > kForceReconnectBehindSec && !reconnectRequested_) {
                        reconnectRequested_ = true;
                        qWarning() << "VideoRenderer: long-term drift, behind live by"
                                   << longTermBehindSec << "s over last"
                                   << (driftAnchorTimer_.elapsed() / 1000.0)
                                   << "s window, requesting stream reconnect";
                        emit fellBehindLive(longTermBehindSec);
                    }
                }
            }
        }
        // 即将渲染：更新锚点为本帧，下一帧以此为基准
        // （每帧都重新校准，不会像固定起点的长期锚点那样被编码帧率与 PTS
        // timebase 间的微小偏差长期累积出虚假的"落后"判定，参考 #047）
        livePacingTimer_.start();
        livePacingStartPts_ = pts;
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
