// MuxThread：封装推流线程（音视频双流版）
// 消费视频/音频两条 FrameQueue<AVPacket*>，以 FLV 或 MPEGTS 格式写出。
// 协议自动识别：rtmp:// → FLV/RTMP，srt:// → MPEGTS/SRT，本地路径 → FLV 文件。
// PTS 归零：记录首包 PTS 为基准，后续包减去偏移，避免接收端跳过前段内容。
#pragma once
#include <QThread>
#include <QElapsedTimer>
#include <atomic>
#include <QString>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class MuxThread : public QThread {
    Q_OBJECT
public:
    ~MuxThread() override;

    // 直通模式初始化：直接复制源流编解码参数，无需重编码
    bool init(const QString& url,
              AVCodecParameters* vpar, AVRational vTimeBase,
              AVCodecParameters* apar, AVRational aTimeBase);
    void stop();

    void setVideoInputQueue(FrameQueue<AVPacket*>* q) { videoQueue_ = q; }
    void setAudioInputQueue(FrameQueue<AVPacket*>* q) { audioQueue_ = q; }

    // 设置推流起始时间（秒）：run() 会丢弃早于该时间的包，对齐播放器当前画面。
    // 必须在 DemuxThread 开始推包之前调用（注册 restream 队列之前）。
    void setStreamStartSeconds(double sec) { videoStartSec_.store(sec); audioStartSec_.store(sec); }

    // 设置推流截止时长（秒，从流开始计）：run() 写包时超过该时长即截断退出。
    // 在停止推流前调用，防止 DemuxThread 超前读包导致录制比实际多几秒。
    void setStreamStopDuration(double durationSec) { stopDuration_.store(durationSec); }

    // 控制"等待 sentinel"模式：true 时丢弃所有包直到收到 nullptr sentinel。
    // 用于推流启动/恢复时防止 DemuxThread 超前读包混入流开头。
    void setWaitingForStart(bool v) { waitingForStart_.store(v); }
    // seek 后激活抑制期：丢弃视频直到 PTS >= targetSec 且为关键帧；音频等视频落点后对齐
    void setSuppressUntilKeyframe(double targetSec) {
        suppressVideoUntilSec_.store(targetSec);
        suppressAudioUntilSec_.store(1e18); // 等待视频关键帧定位
    }

    bool isConnected() const { return connected_; }

signals:
    void connected();
    void errorOccurred(const QString& msg);
    void finished();

protected:
    void run() override;

private:
    AVFormatContext*       fmtCtx_         = nullptr;    // 输出格式上下文
    AVStream*              vStream_        = nullptr;    // 视频流
    AVStream*              aStream_        = nullptr;    // 音频流
    FrameQueue<AVPacket*>* videoQueue_     = nullptr;    // 输入：H.264 包队列
    FrameQueue<AVPacket*>* audioQueue_     = nullptr;    // 输入：AAC 包队列
    AVRational             videoTimeBase_  = {1, 30};   // 视频编码器时间基
    AVRational             audioTimeBase_  = {1, 44100};// 音频编码器时间基
    // 每段的起始源 DTS（sentinel 到来时重置），配合 accumPts 实现跨 seek 连续输出
    // PTS 与 DTS 使用同一 segBase，确保 pts >= dts（FFmpeg 硬校验，否则报 EINVAL）
    int64_t                videoSegBase_   = AV_NOPTS_VALUE;
    int64_t                audioSegBase_   = AV_NOPTS_VALUE;
    // 上一段最后写出的输出 PTS，seek 后新段从此续接
    int64_t                videoAccumPts_  = 0;
    int64_t                audioAccumPts_  = 0;
    int64_t                videoLastOut_   = 0;
    int64_t                audioLastOut_   = 0;
    std::atomic<double>    videoStartSec_{-1.0};        // 推流起始时间过滤（秒），-1 表示不过滤
    std::atomic<double>    audioStartSec_{-1.0};
    std::atomic<double>    stopDuration_{-1.0};         // 推流截止时长（秒，从流首包计），-1 表示不截断
    // seek 抑制期（源 PTS 秒）：视频 >= 0 表示激活；音频 1e18 表示等待视频关键帧落点
    std::atomic<double>    suppressVideoUntilSec_{-1.0};
    std::atomic<double>    suppressAudioUntilSec_{-1.0};
    std::atomic<bool>      waitingForStart_{true};      // 等待首个 sentinel，期间丢弃所有包
    QElapsedTimer          streamClock_;                // sentinel 后启动，用于实时限速
    bool                   clockStarted_ = false;       // streamClock_ 是否已启动
    std::atomic<bool>      abort_{false};
    std::atomic<bool>      connected_{false};
    QString                url_;
};
