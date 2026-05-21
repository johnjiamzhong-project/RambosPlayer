// HttpFlvServer：内置 HTTP-FLV 流媒体服务器，无需 SRS 等外部中继。
// 同局域网设备用浏览器打开 http://IP:PORT/player.html 即可实时观看。
// 管线：FrameQueue<AVPacket*> → FFmpeg FLV muxer（avio_alloc_context）→ HTTP chunked 广播
// 防火墙：start() 时自动尝试添加 Windows 入站规则，失败时通过信号提示用户手动添加。
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

class HttpFlvServer : public QThread {
    Q_OBJECT
public:
    explicit HttpFlvServer(QObject* parent = nullptr);
    ~HttpFlvServer() override;

    // 初始化编解码参数，start() 前调用
    bool init(quint16 port,
              AVCodecParameters* vpar, AVRational vtb,
              AVCodecParameters* apar, AVRational atb);
    void stop();

    FrameQueue<AVPacket*>* videoQueue() { return videoQueue_.get(); }
    FrameQueue<AVPacket*>* audioQueue() { return audioQueue_.get(); }

    QString playerUrl() const;  // http://LAN_IP:PORT/player.html

signals:
    void clientConnected(int total);
    void clientDisconnected(int total);
    void errorOccurred(const QString& msg);
    void firewallHint(const QString& cmd);  // 需要手动添加防火墙规则时发出

protected:
    void run() override;

private:
    // HTTP
    void handleRequest(QTcpSocket* socket, const QByteArray& request);
    void servePlayer(QTcpSocket* socket);
    void serveFlvJs(QTcpSocket* socket);
    void startStreaming(QTcpSocket* socket);
    void removeClient(QTcpSocket* socket);

    // FLV muxer
    bool initMuxer();
    void cleanupMuxer();
    static int writeCallback(void* opaque, const uint8_t* buf, int size);
    void broadcastData(const QByteArray& data);
    void processPackets();

    // 防火墙
    void addFirewallRule();

    // 工具
    static QString getLanIp();
    static QString findFlvJs();
    static QString buildPlayerHtml();

    // FFmpeg
    AVFormatContext*  fmtCtx_    = nullptr;
    AVStream*         vStream_   = nullptr;
    AVStream*         aStream_   = nullptr;
    AVRational        videoTb_   = {1, 90000};
    AVRational        audioTb_   = {1, 44100};

    // FLV 头缓冲：新客户端连接时先发这段数据，保证从合法起点开始解码
    QByteArray        flvHeader_;
    bool              headerFrozen_ = false; // 第一个关键帧后冻结，不再追加

    // PTS 续接（与 MuxThread 逻辑一致）
    int64_t videoSegBase_  = AV_NOPTS_VALUE;
    int64_t audioSegBase_  = AV_NOPTS_VALUE;
    int64_t videoAccumPts_ = 0;
    int64_t audioAccumPts_ = 0;
    int64_t videoLastOut_  = 0;
    int64_t audioLastOut_  = 0;

    // 首帧关键帧门控：丢弃非关键帧视频及之前的所有音频
    // sentinel 会重置为 true，确保重连/新启后从关键帧开始输出
    bool needsKeyframe_ = true;

    // 首帧视频关键帧的源 PTS（秒），用于第一帧音频到来时对齐 A/V 时间戳
    // seek 后关键帧可能比音频目标早若干帧，不对齐会导致声音超前或落后
    double firstKeyframeSrcSec_ = -1.0;

    // 流量统计（诊断音频是否实际发出）
    int64_t audioFrameCount_  = 0;  // 已写入 FLV 的音频帧总数
    int64_t audioByteCount_   = 0;  // 音频帧原始字节累计（不含 FLV tag 开销）
    int64_t videoFrameCount_  = 0;  // 已写入 FLV 的视频帧总数
    int64_t videoByteCount_   = 0;  // 视频帧原始字节累计
    int64_t broadcastByteCount_ = 0; // 通过 broadcastData 发出的总字节数

    // 队列
    std::unique_ptr<FrameQueue<AVPacket*>> videoQueue_;
    std::unique_ptr<FrameQueue<AVPacket*>> audioQueue_;

    // HTTP 客户端
    QList<QTcpSocket*> streamClients_;   // 正在接收 FLV 流的客户端

    quint16           port_  = 8080;
    std::atomic<bool> abort_{false};
};
