// VideoDecodeThread: 视频解码线程
// 消费 FrameQueue<AVPacket*> 输入队列，解码为 AVFrame* 后推入输出队列。
// init() 打开解码器，run() 循环 avcodec_send_packet / avcodec_receive_frame。
// flush() 用于 seek 后清空解码器缓冲。
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecodeThread : public QThread {
    Q_OBJECT
public:
    ~VideoDecodeThread() override;

    // hwEnabled: 尝试 D3D11VA 硬解，失败则静默回退软解
    bool init(AVCodecParameters* params, bool hwEnabled = false);
    void stop();
    void flush();

    void setInputQueue(FrameQueue<AVPacket*>* q)  { inputQueue_  = q; }
    void setOutputQueue(FrameQueue<AVFrame*>* q)  { outputQueue_ = q; }

    int width()  const;
    int height() const;
    AVRational timeBase() const;

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext* codecCtx_ = nullptr;            // 解码器上下文
    FrameQueue<AVPacket*>* inputQueue_ = nullptr;   // 输入：视频包队列
    FrameQueue<AVFrame*>* outputQueue_ = nullptr;   // 输出：解码帧队列
    std::atomic<bool> abort_{false};                // 停止标志
    std::atomic<bool> flush_{false};                // Seek 后清空解码器缓冲
    AVRational timeBase_{1, 1};                     // 时间基，用于外部查询
    bool hwAccel_ = false;                          // 是否成功启用了硬件加速
};
