// EncodeThread：H.264 视频编码线程
// 消费 FrameQueue<AVFrame*> 输入队列，编码后 av_packet_clone fan-out 推入所有输出队列。
// init() 优先尝试 h264_nvenc 硬编，失败回退 libx264 → openh264 → 通用 H.264。
#pragma once
#include <QThread>
#include <atomic>
#include <vector>
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

    // init() 前调用：覆盖默认 GOP（默认=fps，即 1 秒一个关键帧）
    void setGopSize(int g) { gopSize_ = g; }

    void setInputQueue(FrameQueue<AVFrame*>* q) { inputQueue_ = q; }
    void addOutputQueue(FrameQueue<AVPacket*>* q) { outputQueues_.push_back(q); }
    void clearOutputQueues() { outputQueues_.clear(); }

signals:
    void finished();

protected:
    void run() override;

private:
    bool reopenCodec(); // seek 后重建编码器（libx264 不支持 avcodec_flush_buffers 复位）

    AVCodecContext*         codecCtx_    = nullptr; // 编码器上下文
    FrameQueue<AVFrame*>*                inputQueue_   = nullptr; // 输入：原始帧队列
    std::vector<FrameQueue<AVPacket*>*>  outputQueues_;           // 输出：fan-out 目标队列列表
    SwsContext*             swsCtx_      = nullptr; // BGR0→YUV420P 像素格式转换，按需初始化
    AVFrame*                swsFrame_    = nullptr; // 转换目标帧（复用，避免每帧分配）
    std::atomic<bool>       abort_{false};          // 停止标志
    bool                    hwEnc_       = false;   // 是否成功启用硬件编码
    int64_t                 ptsIdx_      = 0;       // 帧序号 PTS，init() 时重置
    int                     gopSize_     = -1;      // -1 = 使用 fps（默认 1s），>0 = 指定帧数
    bool                    firstFrameAfterSentinel_ = false; // sentinel 后等待第一帧日志
    int                     width_       = 0;       // 编码分辨率，init() 保存供 reopenCodec() 使用
    int                     height_      = 0;
    int                     fps_         = 0;       // 帧率
    int                     bitrate_     = 0;       // 码率
};
