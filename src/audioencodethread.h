// AudioEncodeThread：AAC 音频编码线程（Task 5 实现）
// 消费 restreamAudioQ 中的原始音频帧，经 SwrContext 格式转换和 AVAudioFifo 缓冲后
// 送 AAC 编码器，编码包 clone 后 fan-out 推入所有 MuxThread 的音频队列。
#pragma once
#include <QThread>
#include <atomic>
#include <vector>
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
}

class AudioEncodeThread : public QThread {
    Q_OBJECT
public:
    ~AudioEncodeThread() override;

    bool init(int sampleRate, int channels, int bitrate = 128000);
    void stop();
    void addOutputQueue(FrameQueue<AVPacket*>* q);  // fan-out：添加输出目标
    void clearOutputQueues();
    void setInputQueue(FrameQueue<AVFrame*>* q) { inputQueue_ = q; }
    AVCodecContext* codecContext() const { return codecCtx_; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext*                     codecCtx_     = nullptr; // AAC 编码器上下文
    SwrContext*                         swrCtx_       = nullptr; // 格式转换：任意格式 → fltp
    AVAudioFifo*                        fifo_         = nullptr; // 采样缓冲，凑够 1024 喂编码器
    AVFrame*                            swrFrame_     = nullptr; // swr 输出帧（复用）
    FrameQueue<AVFrame*>*               inputQueue_   = nullptr; // 输入：原始音频帧队列
    std::vector<FrameQueue<AVPacket*>*> outputQueues_;           // 输出：fan-out 目标队列列表
    std::atomic<bool>                   abort_{false};
    int64_t                             nextPts_      = 0;       // 编码帧序号，用于生成 PTS
    int                                 sampleRate_   = 44100;   // 编码采样率
    int                                 channels_     = 2;       // 编码声道数
    int                                 bitrate_      = 128000;  // 编码码率，重建编码器时复用

    bool reopenCodec();                                         // seek 后重建 AAC 编码器（~2048 samples lookahead 无法 flush）
};
