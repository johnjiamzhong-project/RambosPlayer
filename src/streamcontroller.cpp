#include "streamcontroller.h"
#include <QDebug>

StreamController::StreamController(QObject* parent) : QObject(parent) {}

StreamController::~StreamController() { stop(); }

// 启动推流管线：本地目标创建 LocalRecorder（DemuxThread 直接写），
// 网络目标创建 MuxThread + 队列对（DemuxThread tryPush 分叉）。
bool StreamController::start(const QList<StreamDestination>& destinations,
                              AVCodecParameters* vpar, AVRational vTimeBase,
                              AVCodecParameters* apar, AVRational aTimeBase) {
    stop();
    if (destinations.isEmpty()) {
        emit errorOccurred("至少需要选择一个推流目标");
        return false;
    }

    qInfo() << "StreamController::start targets=" << destinations.size()
            << "vcodec=" << (vpar ? avcodec_get_name(vpar->codec_id) : "none")
            << "acodec=" << (apar ? avcodec_get_name(apar->codec_id) : "none");

    for (const auto& dest : destinations) {
        if (dest.type == StreamDestination::LocalFile) {
            // 本地录制：LocalRecorder 直接写 FLV，不走 MuxThread 线程
            auto rec = std::make_unique<LocalRecorder>();
            if (!rec->init(dest.url, vpar, vTimeBase, apar, aTimeBase)) {
                emit errorOccurred(QString("本地录制初始化失败: %1").arg(dest.url));
                return false;
            }
            recorders_.push_back(std::move(rec));
            qInfo() << "StreamController: local recorder ready path=" << dest.url;
        } else if (dest.type == StreamDestination::HttpFlv) {
            // HTTP-FLV 内置服务器（-c copy 直通）
            auto srv = std::make_unique<HttpFlvServer>();
            connect(srv.get(), &HttpFlvServer::errorOccurred,
                    this, &StreamController::errorOccurred);
            connect(srv.get(), &HttpFlvServer::firewallHint, this, [](const QString& cmd) {
                qWarning() << "HttpFlvServer: firewall rule needed, run as admin:" << cmd;
            });
            if (!srv->init(dest.port, vpar, vTimeBase, apar, aTimeBase)) {
                QString reason = (vpar && vpar->codec_id != AV_CODEC_ID_H264)
                    ? QString("视频编码 %1 不支持 FLV，请使用 H.264 编码的视频")
                          .arg(avcodec_get_name(vpar->codec_id))
                    : QString("端口 %1 初始化失败").arg(dest.port);
                emit errorOccurred("HTTP-FLV 推流失败：" + reason);
                return false;
            }
            httpFlvServers_.push_back(std::move(srv));
            qInfo() << "StreamController: HTTP-FLV server ready port=" << dest.port;
        } else if (dest.type == StreamDestination::HttpMpegTs) {
            // HTTP-MPEG-TS：StreamPipeline（解码→重编码）+ MpegTsServer
            auto pipeline = std::make_unique<StreamPipeline>();
            if (!pipeline->init(vpar, apar, dest.fps, dest.gopSeconds, dest.bitrate)) {
                emit errorOccurred(QString("MPEG-TS 编码管线初始化失败（端口 %1）").arg(dest.port));
                return false;
            }
            auto srv = std::make_unique<MpegTsServer>();
            connect(srv.get(), &MpegTsServer::errorOccurred,
                    this, &StreamController::errorOccurred);
            connect(srv.get(), &MpegTsServer::firewallHint, this, [](const QString& cmd) {
                qWarning() << "MpegTsServer: firewall rule needed, run as admin:" << cmd;
            });
            if (!srv->init(dest.port,
                           pipeline->encodedVideoQueue(), pipeline->encodedAudioQueue(),
                           pipeline->videoCodecContext(), pipeline->audioCodecContext())) {
                emit errorOccurred(QString("MPEG-TS 服务器初始化失败（端口 %1）").arg(dest.port));
                return false;
            }
            streamPipelines_.push_back(std::move(pipeline));
            mpegTsServers_.push_back(std::move(srv));
            qInfo() << "StreamController: HTTP-MPEG-TS pipeline+server ready port=" << dest.port;
        } else {
            // 网络推流：MuxThread + 队列对
            // 容量控制预读深度：30 帧 ≈ 1.25s，seek 时最多清掉这么多内容。
            // 太小（旧值 10）tryPush 失败率高；太大（200）seek 时丢失内容多。
            auto vQ = std::make_unique<FrameQueue<AVPacket*>>(30);
            auto aQ = std::make_unique<FrameQueue<AVPacket*>>(60);
            auto mux = std::make_unique<MuxThread>();
            connect(mux.get(), &MuxThread::errorOccurred,
                    this, &StreamController::errorOccurred);

            if (!mux->init(dest.url, vpar, vTimeBase, apar, aTimeBase)) {
                emit errorOccurred(QString("推流初始化失败: %1").arg(dest.url));
                return false;
            }
            // 连接本身在 run() 后台线程执行（SRT listener 会阻塞等待客户端）
            // 连接失败通过 errorOccurred 信号异步上报

            mux->setVideoInputQueue(vQ.get());
            mux->setAudioInputQueue(aQ.get());

            videoMuxQueues_.push_back(std::move(vQ));
            audioMuxQueues_.push_back(std::move(aQ));
            muxThreads_.push_back(std::move(mux));
            qInfo() << "StreamController: mux ready url=" << dest.url;
        }
    }

    // 启动网络推流线程、HTTP-FLV 服务器、MPEG-TS 管线和服务器
    for (auto& mux : muxThreads_) mux->start();
    for (auto& srv : httpFlvServers_) srv->start();
    for (auto& srv : mpegTsServers_) srv->start();
    for (auto& p   : streamPipelines_) p->start();

    streaming_ = true;
    emit streamingStarted();
    qInfo() << "StreamController::start ok local=" << recorders_.size()
            << "remote=" << muxThreads_.size()
            << "httpFlv=" << httpFlvServers_.size()
            << "mpegTs=" << mpegTsServers_.size();
    return true;
}

// 透传起始时间到所有 MuxThread
void StreamController::setStreamStartSeconds(double sec) {
    for (auto& mux : muxThreads_) mux->setStreamStartSeconds(sec);
}

void StreamController::setWaitingForStart(bool v) {
    for (auto& mux : muxThreads_) mux->setWaitingForStart(v);
}

// 透传截止时长：MuxThread 用 stopDuration，LocalRecorder 也设置
void StreamController::setStreamStopDuration(double durationSec) {
    qInfo() << "StreamController: setStreamStopDuration" << durationSec << "s";
    for (auto& mux : muxThreads_) mux->setStreamStopDuration(durationSec);
    for (auto& rec : recorders_) rec->setStopDuration(durationSec);
}

// 停止推流：先让 DemuxThread 侧解除绑定（由调用方先调用 clearRestreamPacketQueues），
// 再停止所有 MuxThread → 写 trailer 关闭文件 → 清空状态。
void StreamController::stop() {
    if (!streaming_) return;
    streaming_ = false;

    // 停止网络推流线程
    for (auto& mux : muxThreads_) { mux->stop(); mux->wait(3000); }
    muxThreads_.clear();
    videoMuxQueues_.clear();
    audioMuxQueues_.clear();

    // 停止 HTTP-FLV 服务器
    for (auto& srv : httpFlvServers_) { srv->stop(); srv->wait(3000); }
    httpFlvServers_.clear();

    // 停止 MPEG-TS 管线（先 abort 编码队列，再停服务器）
    for (auto& p   : streamPipelines_) p->stop();
    for (auto& srv : mpegTsServers_)   { srv->stop(); srv->wait(3000); }
    streamPipelines_.clear();
    mpegTsServers_.clear();

    // 停止写入并完成本地录制文件
    for (auto& rec : recorders_) { rec->stop(); rec->finish(); }
    recorders_.clear();

    emit streamingStopped();
    qInfo() << "StreamController::stop done";
}
