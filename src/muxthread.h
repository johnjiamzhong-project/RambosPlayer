// MuxThread：封装推流线程
// 消费 FrameQueue<AVPacket*> 输入队列，以 FLV 格式写出到 RTMP URL 或本地文件。
// init(url) 打开输出容器；run() 循环取包 → av_interleaved_write_frame。
#pragma once
#include <QThread>
#include <atomic>
#include <QString>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
}

class MuxThread : public QThread {
    Q_OBJECT
public:
    ~MuxThread() override;

    // url: RTMP 地址或本地 .flv 文件路径
    bool init(const QString& url, int width, int height, AVRational timeBase,
              const uint8_t* extradata = nullptr, int extradataSize = 0);
    void stop();
    void setInputQueue(FrameQueue<AVPacket*>* q) { inputQueue_ = q; }

    bool isConnected() const { return connected_; }

signals:
    void connected();              // 目标连接成功（RTMP 握手完成时发出）
    void errorOccurred(const QString& msg);
    void finished();

protected:
    void run() override;

private:
    AVFormatContext*        fmtCtx_        = nullptr;   // 输出格式上下文 (FLV)
    AVStream*               vStream_       = nullptr;   // 视频流指针
    FrameQueue<AVPacket*>*  inputQueue_    = nullptr;   // 输入：编码包队列
    AVRational              codecTimeBase_ = {1, 30};   // 编码器时间基，用于写包前 rescale
    std::atomic<bool>       abort_{false};              // 停止标志
    std::atomic<bool>       connected_{false};          // 连接状态
    QString                 url_;                       // 推流目标地址
    bool                    isRtmp_        = false;     // 是否为 RTMP 推流
};
