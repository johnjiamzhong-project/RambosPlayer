// StreamPipeline：低延迟推流编解码管线
// 管理解码→编码四线程及六条 FrameQueue，将原始 AVPacket* 转换为已编码 AVPacket*。
// DemuxThread 将原始包推入 videoInputQueue/audioInputQueue；
// 编码后的包从 encodedVideoQueue/encodedAudioQueue 流出供 MpegTsServer 消费。
#pragma once
#include <QObject>
#include "framequeue.h"
#include "streamdecoder.h"
#include "encodethread.h"
#include "audioencodethread.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

class StreamPipeline : public QObject {
    Q_OBJECT
public:
    explicit StreamPipeline(QObject* parent = nullptr);
    ~StreamPipeline() override;

    // 初始化四条线程：vpar/apar 为源文件编解码参数，fps 用于计算 GOP
    bool init(AVCodecParameters* vpar, AVCodecParameters* apar,
              AVRational vTimeBase, AVRational aTimeBase,
              int fps, double gopSeconds = 0.5, int bitrate = 2000000);
    void start();
    void stop();
    void setSeekTargetSeconds(double seconds);

    // 供 DemuxThread 注册的原始包输入队列（-c copy 分叉点）
    FrameQueue<AVPacket*>* videoInputQueue() { return &streamVideoInQ_; }
    FrameQueue<AVPacket*>* audioInputQueue() { return &streamAudioInQ_; }

    // 供 MpegTsServer 消费的已编码包队列
    FrameQueue<AVPacket*>* encodedVideoQueue() { return &encodedVideoQ_; }
    FrameQueue<AVPacket*>* encodedAudioQueue() { return &encodedAudioQ_; }

    // 供 MpegTsServer 建流时读取编码参数
    AVCodecContext* videoCodecContext() const { return videoEnc_.codecContext(); }
    AVCodecContext* audioCodecContext() const { return audioEnc_.codecContext(); }

private:
    // 队列声明必须在线程前，保证析构时队列比线程活得更长（C++ 成员析构为声明逆序）
    FrameQueue<AVPacket*> streamVideoInQ_{30};  // DemuxThread → 视频解码
    FrameQueue<AVPacket*> streamAudioInQ_{60};  // DemuxThread → 音频解码
    FrameQueue<AVFrame*>  rawVideoQ_{5};        // 视频解码 → 视频编码（小容量控延迟）
    FrameQueue<AVFrame*>  rawAudioQ_{20};       // 音频解码 → 音频编码
    FrameQueue<AVPacket*> encodedVideoQ_{15};   // 视频编码 → MpegTsServer
    FrameQueue<AVPacket*> encodedAudioQ_{30};   // 音频编码 → MpegTsServer

    StreamDecoder  streamVideoDec_;  // 推流专用视频解码（软解，无滤镜）
    StreamDecoder  streamAudioDec_;  // 推流专用音频解码
    EncodeThread   videoEnc_;        // H.264 重编码（GPU h264_nvenc 优先）
    AudioEncodeThread audioEnc_;     // AAC 重编码
};
