// CaptureThread：采集线程
// 从桌面 (gdigrab) 或摄像头 (dshow) 读取视频帧，解码后输出 AVFrame*。
// init(source) 打开采集设备并创建解码器；run() 循环 av_read_frame → 解码 → push 到输出队列。
#pragma once
#include <QThread>
#include <atomic>
#include <QString>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class CaptureThread : public QThread {
    Q_OBJECT
public:
    ~CaptureThread() override;

    // source: "desktop" 或摄像头设备名
    bool init(const QString& source, int width = 1920, int height = 1080,
              int fps = 30);
    void stop();
    void setOutputQueue(FrameQueue<AVFrame*>* q) { outputQueue_ = q; }

signals:
    void captureStarted();
    void captureError(const QString& msg);
    void frameCaptured();  // 每采集一帧发出，供调试统计

protected:
    void run() override;

private:
    AVFormatContext*       fmtCtx_      = nullptr; // 采集输入上下文
    AVCodecContext*        codecCtx_    = nullptr; // 解码器上下文（gdigrab 用 rawvideo）
    FrameQueue<AVFrame*>*  outputQueue_ = nullptr; // 输出：解码后的帧队列
    int                    videoIdx_    = -1;       // fmtCtx_ 中视频流索引
    std::atomic<bool>      abort_{false};           // 停止标志
    QString                source_;                 // 采集源名称
    int                    width_  = 1920;          // 采集分辨率宽
    int                    height_ = 1080;          // 采集分辨率高
    int                    fps_    = 30;            // 采集帧率
};
