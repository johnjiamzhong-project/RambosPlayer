// StreamController：推流总控制器
// 持有并管理 CaptureThread → EncodeThread → MuxThread 的生命周期。
// 对外暴露 start(source, url) / stop()；内部通过两条 FrameQueue 串联三个线程。
// 先输出到本地 FLV 验证编码链路，RTMP 推流需外部 RTMP 服务运行。
#pragma once
#include <QObject>
#include "framequeue.h"
#include "capturethread.h"
#include "encodethread.h"
#include "muxthread.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class StreamController : public QObject {
    Q_OBJECT
public:
    explicit StreamController(QObject* parent = nullptr);
    ~StreamController() override;

    // source: "desktop" 或摄像头设备名
    // url: RTMP 地址或本地 .flv 路径
    // width/height/fps/bitrate: 编码参数
    bool start(const QString& source, const QString& url,
               int width = 1920, int height = 1080, int fps = 30, int bitrate = 2000000);
    void stop();

    bool isStreaming() const { return streaming_; }

signals:
    void streamingStarted();           // 推流所有线程全部启动
    void streamingStopped();           // 推流所有线程全部停止
    void errorOccurred(const QString& msg);

private:
    // 队列必须在成员之前声明，保证析构时队列活得比线程久（C++ 逆序析构）
    FrameQueue<AVFrame*>  rawFrameQ_{30};       // 原始帧队列
    FrameQueue<AVPacket*> encodedPacketQ_{60};  // 编码包队列
    CaptureThread  capture_;
    EncodeThread   encode_;
    MuxThread      mux_;
    bool           streaming_ = false;
};
