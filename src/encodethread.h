// EncodeThread：编码线程
// 消费 FrameQueue<AVFrame*> 输入队列，编码为 H.264 AVPacket* 后推入输出队列。
// init() 优先尝试 h264_nvenc 硬编，失败回退 libx264 软编。
#pragma once
#include <QThread>
#include <atomic>
#include <QString>
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

class EncodeThread : public QThread {
    Q_OBJECT
public:
    ~EncodeThread() override;

    bool init(int width, int height, int fps, int bitrate = 2000000);
    void stop();
    void flush();
    AVCodecContext* codecContext() const { return codecCtx_; }

    void setInputQueue(FrameQueue<AVFrame*>*  q) { inputQueue_  = q; }
    void setOutputQueue(FrameQueue<AVPacket*>* q) { outputQueue_ = q; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext*         codecCtx_    = nullptr; // 编码器上下文
    FrameQueue<AVFrame*>*   inputQueue_  = nullptr; // 输入：原始帧队列
    FrameQueue<AVPacket*>*  outputQueue_ = nullptr; // 输出：编码包队列
    SwsContext*             swsCtx_      = nullptr; // BGR0→YUV420P 像素格式转换，按需初始化
    AVFrame*                swsFrame_    = nullptr; // 转换目标帧（复用，避免每帧分配）
    std::atomic<bool>       abort_{false};          // 停止标志
    bool                    hwEnc_       = false;   // 是否成功启用硬件编码
};
