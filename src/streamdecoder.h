// StreamDecoder：推流专用解码线程（视频/音频通用）
// 消费 FrameQueue<AVPacket*> 输入队列，解码为 AVFrame* 后推入输出队列。
// 与播放用 VideoDecodeThread/AudioDecodeThread 相比，无 QAudioOutput、无 AVSync、
// 无滤镜图，设计单一：正确处理 nullptr sentinel（avcodec_flush_buffers 而非发 EOF）。
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class StreamDecoder : public QThread {
    Q_OBJECT
public:
    ~StreamDecoder() override;

    // 初始化解码器（视频或音频均可）
    bool init(AVCodecParameters* par);
    void stop();

    void setInputQueue(FrameQueue<AVPacket*>* q)  { inputQueue_  = q; }
    void setOutputQueue(FrameQueue<AVFrame*>* q)  { outputQueue_ = q; }

    // 音频解码后的实际参数（视频流调用结果未定义）
    int sampleRate() const { return codecCtx_ ? codecCtx_->sample_rate : 44100; }
    int channels()   const { return codecCtx_ ? codecCtx_->ch_layout.nb_channels : 2; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext*       codecCtx_    = nullptr; // 解码器上下文
    FrameQueue<AVPacket*>* inputQueue_ = nullptr; // 输入：包队列（外部持有）
    FrameQueue<AVFrame*>* outputQueue_ = nullptr; // 输出：帧队列（外部持有）
    std::atomic<bool>     abort_{false};           // 停止标志
};
