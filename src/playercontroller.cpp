#include "playercontroller.h"
#include "videorenderer.h"
#include "logger.h"
#include <QDebug>
#include <QTimer>
#include <QThread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

// 构造函数：创建 100ms 位置定时器，连接 DemuxThread::finished 信号。
PlayerController::PlayerController(VideoRenderer* renderer, QObject* parent)
    : QObject(parent), renderer_(renderer) {
    posTimer_ = new QTimer(this);
    posTimer_->setInterval(100);
    connect(posTimer_, &QTimer::timeout, this, &PlayerController::updatePosition);
    connect(&demux_, &DemuxThread::finished, this, &PlayerController::onDemuxFinished);
    connect(&demux_, &DemuxThread::networkStateChanged, this, &PlayerController::onDemuxNetworkStateChanged);
    connect(&demux_, &DemuxThread::statsUpdated, this, &PlayerController::statsUpdated);
    connect(renderer_, &VideoRenderer::fellBehindLive, this, &PlayerController::onRendererFellBehindLive);
}

// 析构函数：停止所有线程后销毁。
PlayerController::~PlayerController() { stop(); }

// 异步打开媒体文件/网络流：重置所有状态，在独立 worker 线程探测
// （avformat_open_input + avformat_find_stream_info），避免网络流 DNS/握手
// 耗时阻塞 UI 线程。探测完成后回到主线程通过 onProbeFinished 接管结果。
void PlayerController::open(const QString& path) {
    stop();
    videoPacketQ_.reset(); audioPacketQ_.reset(); videoFrameQ_.reset();

    ++probeGeneration_;
    int gen = probeGeneration_;
    auto result = std::make_shared<DemuxThread::ProbeResult>();

    QThread* worker = QThread::create([path, result]{
        *result = DemuxThread::probeOpen(path);
    });
    connect(worker, &QThread::finished, this, [this, worker, result, gen]{
        worker->deleteLater();
        onProbeFinished(result, gen);
    });
    worker->start();
}

// probeOpen() 完成回调（已在主线程，QThread::finished 跨线程信号自动排队）。
// gen 与 probeGeneration_ 不一致说明本次结果已被后续 open() 取代，仅释放资源后返回。
// 探测成功后 adopt() 接管 fmtCtx_，再初始化解码器/渲染器，最终发出 openResult。
void PlayerController::onProbeFinished(std::shared_ptr<DemuxThread::ProbeResult> r, int gen) {
    if (gen != probeGeneration_) {
        if (r->ok) avformat_close_input(&r->fmtCtx);
        return;
    }
    if (!r->ok) {
        emit openResult(false);
        return;
    }

    demux_.adopt(*r, &videoPacketQ_, &audioPacketQ_);

    AVFormatContext* fmt = demux_.formatContext();  // 拿到已打开的 AVFormatContext，后续从中取流参数
    int vi = demux_.videoStreamIdx();               // 视频流索引，-1 表示文件无视频流
    int ai = demux_.audioStreamIdx();               // 音频流索引，-1 表示文件无音频流

    if (vi >= 0) {
        AVCodecParameters* vp = fmt->streams[vi]->codecpar;    // 视频流的编解码参数（分辨率、codec id 等）
        AVRational vtb = fmt->streams[vi]->time_base;          // 视频流时间基，pts 单位换算用
        if (!videoDec_.init(vp, hwAccelEnabled_)) { stop(); emit openResult(false); return; } // 打开视频解码器，失败则整体 open 失败
        videoDec_.setInputQueue(&videoPacketQ_);                // 解码线程从这条队列取包
        videoDec_.setOutputQueue(&videoFrameQ_);                // 解码线程把解码帧推入这条队列
        renderer_->init(vp->width, vp->height, vtb, &sync_, &videoFrameQ_); // 渲染器绑定分辨率、时钟和帧队列
    }
    if (ai >= 0) {
        AVCodecParameters* ap = fmt->streams[ai]->codecpar;    // 音频流的编解码参数（采样率、声道数等）
        AVRational atb = fmt->streams[ai]->time_base;          // 音频流时间基，用于 pts → 秒换算
        if (!audioDec_.init(ap, atb, &sync_)) { stop(); emit openResult(false); return; } // 打开音频解码器并初始化 swr + QAudioOutput
        audioDec_.setInputQueue(&audioPacketQ_);                // 音频解码线程从这条队列取包
    }

    emit durationChanged(duration());  // 通知 UI 文件时长已确定（毫秒）
    qInfo() << "PlayerController::open ok duration=" << duration() << "ms";
    emit openResult(true);
}

// 启动播放：若线程已在运行（暂停状态），只需解除 paused_ 标志；
// 若线程尚未启动（首次 play），调 start() 创建线程。
void PlayerController::play() {
    if (playing_ || !demux_.formatContext()) return;
    playing_ = true;
    qInfo() << "PlayerController::play";
    audioDec_.setPaused(false);   // 先解除暂停，再 start（start 对已运行线程是 no-op）
    demux_.start();
    videoDec_.start();
    audioDec_.start();
    renderer_->startRendering();
    posTimer_->start();
    emit playingChanged(true);
}

// 暂停：停止渲染定时器和位置定时器，同时挂起音频输出。
// 解码线程继续持有资源，恢复时无需重新初始化。
void PlayerController::pause() {
    if (!playing_) return;
    playing_ = false;
    qInfo() << "PlayerController::pause at" << sync_.audioClock() << "s";
    posTimer_->stop();
    renderer_->stopRendering();
    audioDec_.setPaused(true);
    emit playingChanged(false);
}

// 停止：停止渲染，等待所有线程退出，清空三条队列。
void PlayerController::stop() {
    if (demux_.formatContext())
        qInfo() << "PlayerController::stop";
    playing_ = false;
    posTimer_->stop();
    renderer_->stopRendering();
    stopAllThreads();
}

// Seek：立刻更新音频钟到目标位置，再通知各线程。
// 必须先更新钟：VideoRenderer 用 audioClock 判断是否丢帧，若钟仍停在旧位置，
// 旧帧 diff ≈ 0 不会被丢弃，videoFrameQ 堵满 → DemuxThread 无法执行 seek。
// 清除包队列以唤醒可能阻塞在 push 上的 DemuxThread，避免 handleSeek 延迟。
void PlayerController::seek(double seconds) {
    // 在 setAudioClock 覆盖之前捕获当前播放位置，传给 DemuxThread 用于录制器精确截断
    double fromSec = sync_.audioClock();
    qInfo() << "PlayerController::seek target =" << seconds << "audioClock was" << fromSec;
    sync_.setAudioClock(seconds);
    renderer_->flushPendingFrame();
    demux_.seek(seconds, fromSec);
    // 排空播放包队列并释放 AVPacket*，同时唤醒可能阻塞在 push 上的 DemuxThread
    AVPacket* tmp;
    while (videoPacketQ_.tryPop(tmp, 0)) av_packet_free(&tmp);
    while (audioPacketQ_.tryPop(tmp, 0)) av_packet_free(&tmp);
    videoDec_.flush();
    audioDec_.flush(seconds);
    qInfo() << "PlayerController::seek flushes done";

    // 暂停状态下 posTimer_ 已停止，需手动补发进度条位置；
    // 并延迟 200ms 触发单帧渲染，让 VideoDecodeThread 有足够时间解码出新帧。
    if (!playing_) {
        emit positionChanged((int64_t)(seconds * 1000.0));
        QTimer::singleShot(200, renderer_, &VideoRenderer::renderOneFrame);
    }
}

// 设置音量，转发给 AudioDecodeThread（线程安全原子量）。
void PlayerController::setVolume(float v) { volume_ = v; audioDec_.setVolume(v); }

// 硬件加速开关，仅在下次 open() 时生效，播放中切换不重启解码器。
void PlayerController::setHwAccelEnabled(bool on) { hwAccelEnabled_ = on; }

// 返回文件时长（毫秒）。demux_.duration() 单位是微秒。
int64_t PlayerController::duration() const {
    return demux_.duration() / 1000;
}

// 停止并等待所有线程退出（最多 3s），然后清空队列。
// 顺序：先停解复用（生产方），再停解码（消费方），避免队列满导致线程卡死。
// demux_.stop() 内部已调用 clearRestreamQueues()，断开所有推流分叉。
void PlayerController::stopAllThreads() {
    demux_.stop();    demux_.wait(3000);
    videoDec_.stop(); videoDec_.wait(3000);
    audioDec_.stop(); audioDec_.wait(3000);
    videoPacketQ_.clear(); audioPacketQ_.clear(); videoFrameQ_.clear();
}

// 从 AVFormatContext 读取视频帧率，无法获取时返回 30
int PlayerController::videoFps() const {
    AVFormatContext* fmt = demux_.formatContext();
    int vi = demux_.videoStreamIdx();
    if (!fmt || vi < 0) return 30;
    AVRational fr = fmt->streams[vi]->avg_frame_rate;
    if (fr.den == 0) return 30;
    int fps = (int)(av_q2d(fr) + 0.5);
    return fps > 0 ? fps : 30;
}

// 从 AVFormatContext 读取音频采样率，无法获取时返回 44100
int PlayerController::audioSampleRate() const {
    AVFormatContext* fmt = demux_.formatContext();
    int ai = demux_.audioStreamIdx();
    if (!fmt || ai < 0) return 44100;
    int sr = fmt->streams[ai]->codecpar->sample_rate;
    return sr > 0 ? sr : 44100;
}

// 返回源视频流的 AVCodecParameters 指针（仅在 open() 成功后有效，不得释放）
AVCodecParameters* PlayerController::videoCodecPar() const {
    AVFormatContext* fmt = demux_.formatContext();
    int vi = demux_.videoStreamIdx();
    if (!fmt || vi < 0) return nullptr;
    return fmt->streams[vi]->codecpar;
}

// 返回源音频流的 AVCodecParameters 指针
AVCodecParameters* PlayerController::audioCodecPar() const {
    AVFormatContext* fmt = demux_.formatContext();
    int ai = demux_.audioStreamIdx();
    if (!fmt || ai < 0) return nullptr;
    return fmt->streams[ai]->codecpar;
}

// 返回源视频流的时间基（供 MuxThread PTS rescale 使用）
AVRational PlayerController::videoStreamTimeBase() const {
    AVFormatContext* fmt = demux_.formatContext();
    int vi = demux_.videoStreamIdx();
    if (!fmt || vi < 0) return {1, 30};
    return fmt->streams[vi]->time_base;
}

// 返回源音频流的时间基
AVRational PlayerController::audioStreamTimeBase() const {
    AVFormatContext* fmt = demux_.formatContext();
    int ai = demux_.audioStreamIdx();
    if (!fmt || ai < 0) return {1, 44100};
    return fmt->streams[ai]->time_base;
}

// 从 AVFormatContext 读取音频声道数，无法获取时返回 2
int PlayerController::audioChannels() const {
    AVFormatContext* fmt = demux_.formatContext();
    int ai = demux_.audioStreamIdx();
    if (!fmt || ai < 0) return 2;
    int ch = fmt->streams[ai]->codecpar->ch_layout.nb_channels;
    return ch > 0 ? ch : 2;
}

// 解复用线程结束回调：延迟 500ms 发出 playbackFinished，
// 给解码线程留时间耗尽队列中残留的包，避免音视频截断。
void PlayerController::onDemuxFinished() {
    QTimer::singleShot(500, this, [this]{ emit playbackFinished(); });
}

// 位置更新回调：从 AVSync 读取当前音频时钟，转换为毫秒后发出 positionChanged。
// audioClock() 初始为负值（未开始播放），负值时不发信号。
// 检测时钟突跳（两次 100ms 轮询间跳变 > 5 秒），记录警告帮助诊断 PTS 异常。
void PlayerController::updatePosition() {
    double ac = sync_.audioClock();
    if (ac < 0.0) return;

    if (lastPositionSec_ >= 0.0) {
        double jump = ac - lastPositionSec_;
        // 正常播放 100ms 推进约 0.1s，正向跳 > 5s 或时间倒流 > 2s 视为异常
        if (jump > 5.0 || jump < -2.0)
            qInfo() << "PlayerController: audioClock jump from" << lastPositionSec_
                    << "to" << ac << "delta" << jump;
    }
    lastPositionSec_ = ac;
    emit positionChanged((int64_t)(ac * 1000.0));
}

// 直接强制重连（不再先试 renderer_->tryLocalCatchUp() 判断"本地够不够"——实测
// 发现那个判断不可靠：它用"上次渲染时刻+经过的挂钟时间"反推期望 pts，但这个量
// 在不同时长的测试里（最小化 80 秒/2 分钟、遮挡 3 分钟）都稳定落在 -0.7~-0.8 秒，
// 跟实际等待时长毫无关系，说明它测的其实是这套流水线本身解码队列固有的缓冲深度，
// 不是真实落后量，几乎永远会判定"够了"从而拦掉本该执行的重连，导致恢复后留下
// 1-3 秒一直追不回来的残留延迟。重连是目前唯一有真实日志验证过、能干净消除残留
// 延迟的手段（见 docs/BUGFIX-LOG.md，代价是固定几秒的重新握手黑屏），所以恢复
// 时统一直接重连，不再赌"本地这几帧应该够了"。
// 先 abort() 三条队列唤醒可能阻塞在 push() 上的 DemuxThread/VideoDecodeThread
// （否则它们卡死在 push() 里，永远到不了 run() 循环顶部处理重连请求），再调用
// demux_.requestReconnect()。队列恢复可用（reset）和解码器/渲染器状态清理放在
// onDemuxNetworkStateChanged() 收到 Connected 时做——重连是异步的，这里没法
// 立刻知道它什么时候成功。
void PlayerController::forceLiveResync() {
    qInfo() << "PlayerController: forcing stream reconnect";
    videoPacketQ_.abort();
    audioPacketQ_.abort();
    videoFrameQ_.abort();
    demux_.requestReconnect();
}

// VideoRenderer 检测到落后直播源超过强制重连阈值（kForceReconnectBehindSec）时触发。
void PlayerController::onRendererFellBehindLive(double behindSec) {
    qInfo() << "PlayerController: video fell behind live by" << behindSec << "s";
    forceLiveResync();
}

// 转发网络连接状态给 UI；Connected 时（无论是正常打开还是强制重连后）顺带清理：
// 排空并 reset() 三条队列（强制重连场景下可能还处于 onRendererFellBehindLive()
// 留下的 aborted 状态；reset() 前必须先用 tryPop+free 排空，不能直接 reset()，
// 因为队列内部清空不会释放残留的 AVPacket*/AVFrame*，参考 DemuxThread::handleSeek()
// 的同样做法）、flush 解码器、清渲染器暂存帧。正常打开时这些队列/解码器/渲染器
// 本就是空/未初始化状态，以下操作都是安全的空操作。
void PlayerController::onDemuxNetworkStateChanged(int state) {
    emit networkStateChanged(state);
    if (static_cast<DemuxThread::NetworkState>(state) != DemuxThread::NetworkState::Connected) return;

    AVPacket* p;
    while (videoPacketQ_.tryPop(p, 0)) av_packet_free(&p);
    while (audioPacketQ_.tryPop(p, 0)) av_packet_free(&p);
    AVFrame* f;
    while (videoFrameQ_.tryPop(f, 0)) av_frame_free(&f);
    videoPacketQ_.reset();
    audioPacketQ_.reset();
    videoFrameQ_.reset();

    videoDec_.flush();
    renderer_->flushPendingFrame();
}
