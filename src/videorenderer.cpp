#include "videorenderer.h"
#include <QPainter>
#include <QThread>

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

// 析构函数：停止定时器，释放 sws 上下文。
VideoRenderer::~VideoRenderer() {
    stopRendering();
    if (swsCtx_) sws_freeContext(swsCtx_);
}

// 初始化渲染参数：记录视频宽高和时间基，绑定 AVSync 与帧队列，
// 创建 sws 上下文（YUV420P → RGB32）并分配 QImage 缓冲区。
void VideoRenderer::init(int width, int height, AVRational timeBase,
                          AVSync* sync, FrameQueue<AVFrame*>* frameQueue) {
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
void VideoRenderer::startRendering() { timer_->start(); }
void VideoRenderer::stopRendering()  { timer_->stop(); }

// 定时回调：音视频同步的核心决策点。
//
// 为什么用 tryPop(timeout=0) 而不是阻塞等待？
//   onTimer 运行在 GUI 主线程，阻塞会卡死整个 UI（拖动进度条、响应鼠标等全部失效）。
//   用非阻塞 tryPop：队列空时直接返回，等下一个 1ms tick 再试，主线程保持响应。
//
// 为什么 delay > 2.0 时把帧放回而不是丢弃？
//   视频帧 pts 远超音频时钟，说明音频还没追上（seek 后或缓冲积压）。
//   此时丢帧会造成画面永久缺失；放回队列让帧在下一 tick 重新参与判断，
//   等音频时钟追上后自然进入正常渲染路径。
//
// 为什么 delay > 0 时用 msleep 而不是直接跳过？
//   视频帧轻微超前（< 2s）属于正常情况（解码比播放快）。
//   msleep 让视频等音频，避免画面跑太快；delay < 0（视频落后）则跳过 sleep 立即渲染，
//   相当于用"不等待"来追赶音频，实现软性丢帧补偿。
void VideoRenderer::onTimer() {
    AVFrame* frame = nullptr;
    if (!frameQueue_ || !frameQueue_->tryPop(frame, 0)) return;

    double pts = (frame->pts != AV_NOPTS_VALUE)
                 ? frame->pts * av_q2d(timeBase_) : 0.0;
    double delay = sync_ ? sync_->videoDelay(pts) : 0.0;

    if (delay > 2.0) {
        frameQueue_->push(frame);
        return;
    }
    if (delay > 0.0) {
        QThread::msleep((unsigned long)delay);
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
