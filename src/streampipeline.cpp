#include "streampipeline.h"
#include <QDebug>
#include <algorithm>

StreamPipeline::StreamPipeline(QObject* parent) : QObject(parent) {}

StreamPipeline::~StreamPipeline() { stop(); }

// 初始化四条线程并连接内部队列。
// 视频解码走软解（不需要 D3D11VA，帧立即传给编码器），
// GOP = fps × gopSeconds（最小 1 帧）。
bool StreamPipeline::init(AVCodecParameters* vpar, AVCodecParameters* apar,
                           AVRational vTimeBase, AVRational aTimeBase,
                           int fps, double gopSeconds, int bitrate) {
    if (!vpar) {
        qWarning() << "StreamPipeline::init: vpar is null";
        return false;
    }

    // 视频解码线程（软解，无滤镜，nullptr 包触发 flush）
    if (!streamVideoDec_.init(vpar)) {
        qWarning() << "StreamPipeline: video decoder init failed";
        return false;
    }
    streamVideoDec_.setInputQueue(&streamVideoInQ_);
    streamVideoDec_.setOutputQueue(&rawVideoQ_);
    streamVideoDec_.setInputTimeBase(vTimeBase);

    // 视频编码线程（GOP 由 gopSeconds 控制，余量取 max(1,...)）
    int gopSize = qMax(1, (int)(fps * gopSeconds));
    videoEnc_.setGopSize(gopSize);
    if (!videoEnc_.init(vpar->width, vpar->height, fps, bitrate)) {
        qWarning() << "StreamPipeline: video encoder init failed";
        return false;
    }
    videoEnc_.setInputQueue(&rawVideoQ_);
    videoEnc_.addOutputQueue(&encodedVideoQ_);

    // 音频解码线程（apar 可为 null，表示无音频流）
    if (apar) {
        if (!streamAudioDec_.init(apar)) {
            qWarning() << "StreamPipeline: audio decoder init failed";
            return false;
        }
        streamAudioDec_.setInputQueue(&streamAudioInQ_);
        streamAudioDec_.setOutputQueue(&rawAudioQ_);
        streamAudioDec_.setInputTimeBase(aTimeBase);

        // 音频编码线程
        int sr = streamAudioDec_.sampleRate();
        int ch = streamAudioDec_.channels();
        if (!audioEnc_.init(sr, ch)) {
            qWarning() << "StreamPipeline: audio encoder init failed";
            return false;
        }
        audioEnc_.setInputQueue(&rawAudioQ_);
        audioEnc_.addOutputQueue(&encodedAudioQ_);
    }

    qInfo() << "StreamPipeline::init ok fps=" << fps
            << "gopSize=" << gopSize << "bitrate=" << bitrate
            << "video=" << vpar->width << "x" << vpar->height
            << "hasAudio=" << (apar != nullptr);
    return true;
}

// 设置低延迟推流 seek 目标。视频解码器会预滚目标前包但不输出旧帧给编码器。
void StreamPipeline::setSeekTargetSeconds(double seconds) {
    streamVideoDec_.setMinOutputSeconds(seconds);
}

// 启动四条线程
void StreamPipeline::start() {
    streamVideoDec_.start();
    videoEnc_.start();
    streamAudioDec_.start();
    audioEnc_.start();
    qInfo() << "StreamPipeline: all threads started";
}

// 停止四条线程：先 abort 入口队列（解除 DemuxThread 可能的 tryPush 阻塞），
// 再逐一调 stop()（内部会 abort 各自的输入/输出队列），最后等待退出。
void StreamPipeline::stop() {
    // abort 入口队列（DemuxThread 注册的分叉队列）
    streamVideoInQ_.abort();
    streamAudioInQ_.abort();

    // 各线程 stop() 负责 abort 自己的输入/输出队列
    streamVideoDec_.stop();
    streamAudioDec_.stop();
    videoEnc_.stop();
    audioEnc_.stop();

    streamVideoDec_.wait(3000);
    streamAudioDec_.wait(3000);
    videoEnc_.wait(3000);
    audioEnc_.wait(3000);

    qInfo() << "StreamPipeline: all threads stopped";
}
