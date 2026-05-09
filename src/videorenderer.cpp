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
void VideoRenderer::onTimer() {
    AVFrame* frame = nullptr;
    if (!frameQueue_ || !frameQueue_->tryPop(frame, 0)) return;  // 非阻塞取帧，队列空则直接返回，避免阻塞 GUI 线程

    double pts = (frame->pts != AV_NOPTS_VALUE)
                 ? frame->pts * av_q2d(timeBase_) : 0.0;  // pts × 时间基 → 秒；无效 pts 按 0 处理
    double delay = sync_ ? sync_->videoDelay(pts) : 0.0;  // 查询视频帧相对音频时钟的延迟（秒），负值表示视频落后

    if (delay > 2.0) {          // 视频大幅领先（音频未追上，常见于 seek 后）
        frameQueue_->push(frame);   // 放回队列，等音频时钟追上后再渲染，不丢帧
        return;
    }
    if (delay > 0.0) {
        QThread::msleep((unsigned long)delay);  // 视频轻微超前，等待音频追上；delay ≤ 0 则跳过，立即渲染追赶
    }

    uint8_t* dst[1]  = { currentFrame_.bits() };           // 拿到 QImage 像素缓冲区的裸指针，供 sws_scale 直接写入
    int dstStride[1] = { currentFrame_.bytesPerLine() };   // 每行字节数（stride），RGB32 = width × 4
    {
        QMutexLocker lk(&frameMutex_);              // 加锁防止 paintEvent() 在写入过程中读取撕裂的半帧
        sws_scale(swsCtx_,
                  (const uint8_t* const*)frame->data, frame->linesize,
                  0, srcH_, dst, dstStride);        // YUV420P → RGB32，结果原地写入 currentFrame_ 缓冲区
    }
    av_frame_free(&frame);  // 释放解码帧（AVFrame 及其数据缓冲区）
    update();               // 通知 Qt 调度 paintEvent()，将 currentFrame_ 绘制到屏幕
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
