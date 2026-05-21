// StreamController：推流总控制器
// 网络推流目标（RTMP/SRT）：创建 MuxThread + FrameQueue 对，DemuxThread tryPush 分叉。
// 本地录制目标（LocalFile）：创建 LocalRecorder，DemuxThread 直接同步写 FLV。
// HTTP-FLV 目标：创建 HttpFlvServer，内置 HTTP 服务，浏览器直接访问。
#pragma once
#include <QObject>
#include <memory>
#include <vector>
#include <QList>
#include "framequeue.h"
#include "muxthread.h"
#include "localrecorder.h"
#include "httpflvserver.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

struct StreamDestination {
    enum Type { Rtmp, Srt, LocalFile, HttpFlv };
    Type    type;
    QString url;   // RTMP: 完整 URL；SRT: "srt://:port"；LocalFile: 文件路径
    quint16 port = 8080;  // HttpFlv 专用
};

class StreamController : public QObject {
    Q_OBJECT
public:
    explicit StreamController(QObject* parent = nullptr);
    ~StreamController() override;

    // 启动推流：本地目标用 LocalRecorder，网络目标用 MuxThread
    bool start(const QList<StreamDestination>& destinations,
               AVCodecParameters* vpar, AVRational vTimeBase,
               AVCodecParameters* apar, AVRational aTimeBase);
    void stop();

    bool isStreaming() const { return streaming_; }

    // 网络推流队列（供 PlayerController 注册到 DemuxThread）
    const std::vector<std::unique_ptr<FrameQueue<AVPacket*>>>& videoMuxQueues() const { return videoMuxQueues_; }
    const std::vector<std::unique_ptr<FrameQueue<AVPacket*>>>& audioMuxQueues() const { return audioMuxQueues_; }

    // 本地录制器（供 PlayerController 注册到 DemuxThread）
    const std::vector<std::unique_ptr<LocalRecorder>>& recorders() const { return recorders_; }
    // 网络推流线程（供 PlayerController 注册到 DemuxThread，用于 seek 抑制通知）
    const std::vector<std::unique_ptr<MuxThread>>& muxThreads() const { return muxThreads_; }
    // HTTP-FLV 服务器（供 PlayerController 注册队列到 DemuxThread）
    const std::vector<std::unique_ptr<HttpFlvServer>>& httpFlvServers() const { return httpFlvServers_; }

    // MuxThread 侧控制（仅对网络推流生效）
    void setStreamStartSeconds(double sec);
    void setStreamStopDuration(double durationSec);
    void setWaitingForStart(bool v);

signals:
    void streamingStarted();
    void streamingStopped();
    void errorOccurred(const QString& msg);

private:
    std::vector<std::unique_ptr<MuxThread>>             muxThreads_;       // 网络推流线程
    std::vector<std::unique_ptr<FrameQueue<AVPacket*>>> videoMuxQueues_;   // 视频包队列（网络）
    std::vector<std::unique_ptr<FrameQueue<AVPacket*>>> audioMuxQueues_;   // 音频包队列（网络）
    std::vector<std::unique_ptr<LocalRecorder>>         recorders_;        // 本地录制器
    std::vector<std::unique_ptr<HttpFlvServer>>         httpFlvServers_;   // HTTP-FLV 服务器
    bool streaming_ = false;
};
