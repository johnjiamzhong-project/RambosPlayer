#include "streamcontroller.h"
#include <QDebug>

StreamController::StreamController(QObject* parent) : QObject(parent) {}

// 析构：停止推流，所有线程安全退出
StreamController::~StreamController() { stop(); }

// 启动推流：依次初始化捕获/编码/封装线程，然后 start。
// 每步失败时记录详细日志，并通过 errorOccurred 信号通知 UI 层。
bool StreamController::start(const QString& source, const QString& url,
                              int width, int height, int fps, int bitrate) {
    qInfo() << "StreamController::start begin source =" << source << "url =" << url
            << "size =" << width << "x" << height << "fps =" << fps;
    stop();

    // 1) 初始化采集线程
    if (!capture_.init(source, width, height, fps)) {
        QString msg = QString("Capture device init failed: %1").arg(source);
        qWarning() << "StreamController:" << msg;
        emit errorOccurred(msg);
        return false;
    }
    capture_.setOutputQueue(&rawFrameQ_);
    qInfo() << "StreamController: capture init ok";

    // 2) 初始化编码线程
    if (!encode_.init(width, height, fps, bitrate)) {
        QString msg = QString("Encoder init failed.\n\n"
                              "Install FFmpeg with H.264 support:\n"
                              "  .\\vcpkg install ffmpeg[gpl,x264]\n\n"
                              "See logs for encoder probe details.");
        qWarning() << "StreamController:" << msg;
        emit errorOccurred(msg);
        return false;
    }
    encode_.setInputQueue(&rawFrameQ_);
    encode_.setOutputQueue(&encodedPacketQ_);
    qInfo() << "StreamController: encode init ok";

    // 3) 初始化封装推流线程（传入编码器 extradata / SPS+PPS）
    AVCodecContext* encCtx = encode_.codecContext();
    if (!mux_.init(url, width, height, AVRational{1, fps},
                   encCtx ? encCtx->extradata : nullptr,
                   encCtx ? encCtx->extradata_size : 0)) {
        QString msg = QString("Mux/connect failed: %1").arg(url);
        qWarning() << "StreamController:" << msg;
        emit errorOccurred(msg);
        return false;
    }
    mux_.setInputQueue(&encodedPacketQ_);
    qInfo() << "StreamController: mux init ok";

    // 4) 启动三个线程（逆流方向：先启动下游，避免上游推入队列时下游还未就绪）
    mux_.start();
    encode_.start();
    capture_.start();

    streaming_ = true;
    emit streamingStarted();
    qInfo() << "StreamController::start ok source =" << source << "url =" << url;
    return true;
}

// 停止推流：先停采集（不再生产），再停编码，最后停封装（消费完残留包）。
void StreamController::stop() {
    if (!streaming_) return;
    streaming_ = false;

    capture_.stop();  capture_.wait(3000);
    encode_.stop();    encode_.wait(3000);
    mux_.stop();        mux_.wait(3000);

    // reset() 同时清空队列并复位 aborted_ 标志，确保下次 start() 时队列可用
    rawFrameQ_.reset();
    encodedPacketQ_.reset();

    emit streamingStopped();
    qInfo() << "StreamController::stop done";
}
