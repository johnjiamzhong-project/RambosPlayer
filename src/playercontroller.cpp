#include "playercontroller.h"
#include "videorenderer.h"
#include <QTimer>

extern "C" {
#include <libavformat/avformat.h>
}

// 构造函数：创建 100ms 位置定时器，连接 DemuxThread::finished 信号。
PlayerController::PlayerController(VideoRenderer* renderer, QObject* parent)
    : QObject(parent), renderer_(renderer) {
    posTimer_ = new QTimer(this);
    posTimer_->setInterval(100);
    connect(posTimer_, &QTimer::timeout, this, &PlayerController::updatePosition);
    connect(&demux_, &DemuxThread::finished, this, &PlayerController::onDemuxFinished);
}

// 析构函数：停止所有线程后销毁。
PlayerController::~PlayerController() { stop(); }

// 打开媒体文件：重置所有状态，依次初始化解复用、解码线程和渲染器。
// 从 formatContext() 读取实际流参数（宽高、时间基、codec），传给各组件。
bool PlayerController::open(const QString& path) {
    stop();
    videoPacketQ_.reset(); audioPacketQ_.reset(); videoFrameQ_.reset();

    if (!demux_.open(path, &videoPacketQ_, &audioPacketQ_))
        return false;

    AVFormatContext* fmt = demux_.formatContext();  // 拿到已打开的 AVFormatContext，后续从中取流参数
    int vi = demux_.videoStreamIdx();               // 视频流索引，-1 表示文件无视频流
    int ai = demux_.audioStreamIdx();               // 音频流索引，-1 表示文件无音频流

    if (vi >= 0) {
        AVCodecParameters* vp = fmt->streams[vi]->codecpar;    // 视频流的编解码参数（分辨率、codec id 等）
        AVRational vtb = fmt->streams[vi]->time_base;          // 视频流时间基，pts 单位换算用
        if (!videoDec_.init(vp)) return false;                  // 打开视频解码器，失败则整体 open 失败
        videoDec_.setInputQueue(&videoPacketQ_);                // 解码线程从这条队列取包
        videoDec_.setOutputQueue(&videoFrameQ_);                // 解码线程把解码帧推入这条队列
        renderer_->init(vp->width, vp->height, vtb, &sync_, &videoFrameQ_); // 渲染器绑定分辨率、时钟和帧队列
    }
    if (ai >= 0) {
        AVCodecParameters* ap = fmt->streams[ai]->codecpar;    // 音频流的编解码参数（采样率、声道数等）
        AVRational atb = fmt->streams[ai]->time_base;          // 音频流时间基，用于 pts → 秒换算
        if (!audioDec_.init(ap, atb, &sync_)) return false;    // 打开音频解码器并初始化 swr + QAudioOutput
        audioDec_.setInputQueue(&audioPacketQ_);                // 音频解码线程从这条队列取包
    }

    emit durationChanged(duration());  // 通知 UI 文件时长已确定（毫秒）
    return true;
}

// 启动播放：依次启动解复用、解码、渲染定时器和位置定时器。
// playing_ 防重入，避免重复调用导致线程重复 start。
void PlayerController::play() {
    if (playing_ || !demux_.formatContext()) return;
    playing_ = true;
    demux_.start();
    videoDec_.start();
    audioDec_.start();
    renderer_->startRendering();
    posTimer_->start();
}

// 暂停：停止渲染定时器和位置定时器，解码线程继续运行以维持队列缓冲。
// 简化实现：生产级需在各线程中加暂停标志，这里通过 stop/seek/play 实现继续。
void PlayerController::pause() {
    playing_ = false;
    posTimer_->stop();
    renderer_->stopRendering();
}

// 停止：停止渲染，等待所有线程退出，清空三条队列。
void PlayerController::stop() {
    playing_ = false;
    posTimer_->stop();
    renderer_->stopRendering();
    stopAllThreads();
}

// Seek：通知解复用线程定位，然后 flush 两个解码线程的内部缓冲，防止旧数据污染。
void PlayerController::seek(double seconds) {
    demux_.seek(seconds);
    videoDec_.flush();
    audioDec_.flush();
}

// 设置音量，转发给 AudioDecodeThread（线程安全原子量）。
void PlayerController::setVolume(float v) { audioDec_.setVolume(v); }

// 返回文件时长（毫秒）。demux_.duration() 单位是微秒。
int64_t PlayerController::duration() const {
    return demux_.duration() / 1000;
}

// 停止并等待所有线程退出（最多 3s），然后清空队列。
// 顺序：先停解复用（生产方），再停解码（消费方），避免队列满导致线程卡死。
void PlayerController::stopAllThreads() {
    demux_.stop();    demux_.wait(3000);
    videoDec_.stop(); videoDec_.wait(3000);
    audioDec_.stop(); audioDec_.wait(3000);
    videoPacketQ_.clear(); audioPacketQ_.clear(); videoFrameQ_.clear();
}

// 解复用线程结束回调：延迟 500ms 发出 playbackFinished，
// 给解码线程留时间耗尽队列中残留的包，避免音视频截断。
void PlayerController::onDemuxFinished() {
    QTimer::singleShot(500, this, [this]{ emit playbackFinished(); });
}

// 位置更新回调：从 AVSync 读取当前音频时钟，转换为毫秒后发出 positionChanged。
// audioClock() 初始为负值（未开始播放），负值时不发信号。
void PlayerController::updatePosition() {
    double ac = sync_.audioClock();
    if (ac >= 0.0)
        emit positionChanged((int64_t)(ac * 1000.0));
}
