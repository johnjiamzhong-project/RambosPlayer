// MpegTsServer：内置 HTTP MPEG-TS 流媒体服务器（低延迟模式）
// 消费 StreamPipeline 输出的已编码 AVPacket*，经 FFmpeg mpegts muxer 广播给浏览器。
// 晚连客户端收到 headerBytes_（含 PAT/PMT）+ segmentBytes_（最近一个关键帧起的数据）。
// 浏览器端使用 mpegts.js 以追帧模式播放，端到端延迟 ≤ 600ms。
#pragma once
#include <QThread>
#include <QList>
#include <QByteArray>
#include <QString>
#include <atomic>
#include "framequeue.h"

class QTcpSocket;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class MpegTsServer : public QThread {
    Q_OBJECT
public:
    explicit MpegTsServer(QObject* parent = nullptr);
    ~MpegTsServer() override;

    // 初始化 muxer 参数。队列由 StreamPipeline 持有，不转移所有权。
    // vCodecCtx/aCodecCtx 为 EncodeThread/AudioEncodeThread 的编码器上下文。
    bool init(quint16 port,
              FrameQueue<AVPacket*>* vq, FrameQueue<AVPacket*>* aq,
              AVCodecContext* vCodecCtx, AVCodecContext* aCodecCtx);
    void stop();
    void requestClientReconnect();

    QString playerUrl() const;  // http://LAN_IP:PORT/player.html
    quint16 port() const { return port_; }

signals:
    void clientConnected(int total);
    void clientDisconnected(int total);
    void errorOccurred(const QString& msg);
    void firewallHint(const QString& cmd);

protected:
    void run() override;

private:
    // HTTP
    void handleRequest(QTcpSocket* socket, const QByteArray& request);
    void servePlayer(QTcpSocket* socket);
    void serveMpegtsJs(QTcpSocket* socket);
    void startStreaming(QTcpSocket* socket);
    void removeClient(QTcpSocket* socket);
    void disconnectAllStreamClients(const QString& reason);
    void flushPendingClients(const QString& reason);

    // MPEG-TS muxer
    bool initMuxer();
    void cleanupMuxer();
    static int writeCallback(void* opaque, const uint8_t* buf, int size);
    void broadcastData(const QByteArray& data);
    void processPackets();

    // 防火墙
    void addFirewallRule();

    // 工具
    static QString getLanIp();
    static QString buildPlayerHtml();

    // FFmpeg
    AVFormatContext*  fmtCtx_    = nullptr;
    AVStream*         vStream_   = nullptr;
    AVStream*         aStream_   = nullptr;
    AVRational        videoTb_   = {1, 30};     // 编码器 time_base（与 EncodeThread 一致）
    AVRational        audioTb_   = {1, 44100};  // 编码器 time_base（与 AudioEncodeThread 一致）

    // late-join 缓冲：新客户端连接时发送 headerBytes_ + segmentBytes_
    QByteArray        headerBytes_;              // avformat_write_header 输出（PAT/PMT）
    bool              headerFrozen_ = false;     // 首个关键帧写入后冻结
    QByteArray        segmentBytes_;             // 最近一个关键帧起至今的 TS 字节
    bool              newSegmentStarting_ = false; // writeCallback 中重置 segmentBytes_

    // 首帧关键帧门控
    bool needsKeyframe_ = true;

    // PTS 续接（编码器从 0 计数，seek 后重置，此处保证输出单调递增）
    int64_t videoSegBase_  = AV_NOPTS_VALUE;
    int64_t audioSegBase_  = AV_NOPTS_VALUE;
    int64_t videoAccumPts_ = 0;
    int64_t audioAccumPts_ = 0;
    int64_t videoLastOut_  = 0;
    int64_t audioLastOut_  = 0;

    // 输入队列（外部持有，生命周期由 StreamPipeline 管理）
    FrameQueue<AVPacket*>* videoQueue_ = nullptr;
    FrameQueue<AVPacket*>* audioQueue_ = nullptr;

    // HTTP 客户端
    QList<QTcpSocket*> streamClients_;   // 正在接收 TS 流的客户端
    QList<QTcpSocket*> pendingClients_;  // 等待首个关键帧的客户端

    quint16           port_  = 8080;
    std::atomic<bool> reconnectClientsRequested_{false}; // 外部请求浏览器重连，由服务线程执行断开
    std::atomic<bool> abort_{false};
};
