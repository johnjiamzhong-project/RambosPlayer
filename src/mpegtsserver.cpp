#include "mpegtsserver.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QProcess>
#include <QFile>
#include <QCoreApplication>
#include <QNetworkInterface>
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

MpegTsServer::MpegTsServer(QObject* parent) : QThread(parent) {}

MpegTsServer::~MpegTsServer() {
    stop();
    wait(5000);
    cleanupMuxer();
}

// 初始化 MPEG-TS muxer 格式上下文（不打开网络）。
// 流参数从编码器上下文复制，保证 SPS/PPS extradata 已就绪。
bool MpegTsServer::init(quint16 port,
                         FrameQueue<AVPacket*>* vq, FrameQueue<AVPacket*>* aq,
                         AVCodecContext* vCodecCtx, AVCodecContext* aCodecCtx) {
    port_       = port;
    videoQueue_ = vq;
    audioQueue_ = aq;

    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, "mpegts", nullptr) < 0) {
        qWarning() << "MpegTsServer: avformat_alloc_output_context2 failed";
        return false;
    }

    if (vCodecCtx) {
        vStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!vStream_) return false;
        avcodec_parameters_from_context(vStream_->codecpar, vCodecCtx);
        vStream_->codecpar->codec_tag = 0;
        vStream_->time_base = vCodecCtx->time_base;
        videoTb_ = vCodecCtx->time_base;
    }
    if (aCodecCtx) {
        aStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!aStream_) return false;
        avcodec_parameters_from_context(aStream_->codecpar, aCodecCtx);
        aStream_->codecpar->codec_tag = 0;
        aStream_->time_base = aCodecCtx->time_base;
        audioTb_ = aCodecCtx->time_base;
        qInfo() << "MpegTsServer: audio stream codec="
                << avcodec_get_name(aCodecCtx->codec_id)
                << "sr=" << aCodecCtx->sample_rate
                << "extradata_size=" << aCodecCtx->extradata_size;
    } else {
        qWarning() << "MpegTsServer: NO audio stream (aCodecCtx=null)";
    }

    qInfo() << "MpegTsServer::init ok port=" << port_;
    return true;
}

void MpegTsServer::stop() {
    abort_ = true;
    quit();
}

// 在 run() 中初始化 avio（自定义写回调），再调 avformat_write_header 写出 PAT/PMT
bool MpegTsServer::initMuxer() {
    const int BUF = 65536;
    uint8_t* buf = static_cast<uint8_t*>(av_malloc(BUF));
    if (!buf) return false;

    AVIOContext* avio = avio_alloc_context(buf, BUF, 1, this, nullptr, writeCallback, nullptr);
    if (!avio) { av_free(buf); return false; }

    fmtCtx_->pb    = avio;
    fmtCtx_->flags |= AVFMT_FLAG_CUSTOM_IO;

    // PAT/PMT 每 0.5s 重发一次（与 GOP 对齐），保证 segmentBytes_ 开头含 PAT/PMT
    av_opt_set(fmtCtx_->priv_data, "mpegts_flags", "pat_period=0.5", 0);

    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "MpegTsServer: avformat_write_header failed";
        return false;
    }
    // MPEG-TS muxer 将 PAT/PMT 留在 avio 内部 64 KB 缓冲区中，不会自动调用 writeCallback。
    // 必须手动 flush 才能让 PAT/PMT 字节流经 writeCallback 进入 headerBytes_。
    avio_flush(fmtCtx_->pb);
    qInfo() << "MpegTsServer: muxer ready, header=" << headerBytes_.size() << "bytes";
    return true;
}

void MpegTsServer::cleanupMuxer() {
    if (fmtCtx_) {
        if (fmtCtx_->pb) {
            avio_context_free(&fmtCtx_->pb);
        }
        avformat_free_context(fmtCtx_);
        fmtCtx_ = nullptr;
    }
    vStream_ = aStream_ = nullptr;
}

// FFmpeg 写数据回调：写入前收集到 headerBytes_/segmentBytes_，并广播给所有客户端
int MpegTsServer::writeCallback(void* opaque, const uint8_t* buf, int size) {
    auto* self = static_cast<MpegTsServer*>(opaque);
    QByteArray data(reinterpret_cast<const char*>(buf), size);

    if (!self->headerFrozen_) {
        self->headerBytes_.append(data);
    } else {
        if (self->newSegmentStarting_) {
            self->newSegmentStarting_ = false;
            self->segmentBytes_.clear();
        }
        self->segmentBytes_.append(data);
    }
    self->broadcastData(data);
    return size;
}

// 广播 TS 数据到所有正在接收的 HTTP 客户端
void MpegTsServer::broadcastData(const QByteArray& data) {
    QMutableListIterator<QTcpSocket*> it(streamClients_);
    while (it.hasNext()) {
        QTcpSocket* s = it.next();
        if (s->state() == QAbstractSocket::ConnectedState) {
            s->write(data);
        } else {
            it.remove();
        }
    }
}

// 主线程：HTTP 服务 + 定时轮询包队列
void MpegTsServer::run() {
    if (!initMuxer()) {
        emit errorOccurred("MPEG-TS muxer 初始化失败");
        return;
    }

    QTcpServer server;
    if (!server.listen(QHostAddress::AnyIPv4, port_)) {
        qWarning() << "MpegTsServer: listen failed port=" << port_ << server.errorString();
        emit errorOccurred(QString("HTTP 服务启动失败（端口 %1）: %2").arg(port_).arg(server.errorString()));
        return;
    }
    qInfo() << "MpegTsServer: listening port=" << port_ << "url=" << playerUrl();

    connect(&server, &QTcpServer::newConnection, [&]() {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, [this, socket]() {
                QByteArray req = socket->readAll();
                handleRequest(socket, req);
            });
            connect(socket, &QTcpSocket::disconnected, [this, socket]() {
                removeClient(socket);
                socket->deleteLater();
                emit clientDisconnected(streamClients_.size());
            });
        }
    });

    QTimer pktTimer;
    connect(&pktTimer, &QTimer::timeout, [this]() { processPackets(); });
    pktTimer.start(5);

    exec();

    if (fmtCtx_) av_write_trailer(fmtCtx_);
    for (QTcpSocket* s : streamClients_) s->disconnectFromHost();
    streamClients_.clear();
    qInfo() << "MpegTsServer: stopped";
}

// 按 HTTP 路径分发请求
void MpegTsServer::handleRequest(QTcpSocket* socket, const QByteArray& request) {
    QString firstLine = QString::fromUtf8(request).section('\n', 0, 0).trimmed();
    QString path = firstLine.section(' ', 1, 1);
    qInfo() << "MpegTsServer: HTTP request" << path << "from" << socket->peerAddress().toString();

    if (path == "/" || path == "/player.html" || path.isEmpty()) {
        servePlayer(socket);
    } else if (path == "/mpegts.min.js") {
        serveMpegtsJs(socket);
    } else if (path == "/stream.ts") {
        startStreaming(socket);
    } else {
        qWarning() << "MpegTsServer: 404 path=" << path;
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        socket->disconnectFromHost();
    }
}

// 返回内嵌播放页面（使用 mpegts.js + 追帧模式）
void MpegTsServer::servePlayer(QTcpSocket* socket) {
    QString html = buildPlayerHtml();
    QByteArray body = html.toUtf8();
    QByteArray resp = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                      "\r\n" + body;
    socket->write(resp);
    socket->disconnectFromHost();
}

// 返回 mpegts.min.js：优先 exe 同目录（用户可替换版本），回退到 Qt 内嵌资源
void MpegTsServer::serveMpegtsJs(QTcpSocket* socket) {
    QString fsPath = QCoreApplication::applicationDirPath() + "/mpegts.min.js";
    QFile f(QFile::exists(fsPath) ? fsPath : ":/mpegts.min.js");
    if (!f.open(QIODevice::ReadOnly)) {
        QByteArray body = "/* mpegts.min.js not found */";
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/javascript\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
        return;
    }
    QByteArray js = f.readAll();
    socket->write("HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/javascript\r\n"
                  "Cache-Control: public, max-age=3600\r\n"
                  "Content-Length: " + QByteArray::number(js.size()) + "\r\n\r\n");
    socket->write(js);
    socket->disconnectFromHost();
}

// 将客户端加入推流列表，先发 headerBytes_ + segmentBytes_（late-join），后续实时广播
void MpegTsServer::startStreaming(QTcpSocket* socket) {
    socket->write("HTTP/1.1 200 OK\r\n"
                  "Content-Type: video/mp2t\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Connection: keep-alive\r\n"
                  "\r\n");
    if (headerFrozen_) {
        // 晚连：发 PAT/PMT header + 最近关键帧起的 segment（含后续 PAT/PMT）
        if (!headerBytes_.isEmpty()) socket->write(headerBytes_);
        if (!segmentBytes_.isEmpty()) socket->write(segmentBytes_);
        streamClients_.append(socket);
        qInfo() << "MpegTsServer: client connected (late-join)"
                << "total=" << streamClients_.size()
                << "header=" << headerBytes_.size() << "bytes"
                << "segment=" << segmentBytes_.size() << "bytes";
    } else {
        // 早连：等待首个关键帧后补发
        pendingClients_.append(socket);
        qInfo() << "MpegTsServer: client pending (no keyframe yet)"
                << "pending=" << pendingClients_.size();
    }
    emit clientConnected(streamClients_.size() + pendingClients_.size());
}

void MpegTsServer::removeClient(QTcpSocket* socket) {
    streamClients_.removeOne(socket);
    pendingClients_.removeOne(socket);
}

// 轮询包队列，写入 MPEG-TS muxer（muxer 通过 writeCallback 广播）
void MpegTsServer::processPackets() {
    if (abort_) { quit(); return; }

    for (int i = 0; i < 8; ++i) {
        AVPacket* pkt = nullptr;
        bool gotVideo = videoQueue_ && videoQueue_->tryPop(pkt, 0);
        if (gotVideo) {
            if (!pkt) {
                // sentinel（seek）：重置 PTS 续接基点，进入关键帧等待
                int64_t oneUnit = qMax((int64_t)1,
                    av_rescale_q(1, {1, 1000}, videoTb_));
                videoAccumPts_ = videoLastOut_ + oneUnit;
                videoSegBase_  = AV_NOPTS_VALUE;
                needsKeyframe_ = true;
                qInfo() << "MpegTsServer: video sentinel, accumPts=" << videoAccumPts_;
                // seek 后强制断开所有客户端，让浏览器重连拿新位置的 segmentBytes_
                // 避免旧缓冲与新帧混在 MSE buffer 里造成 5 秒以上的时延堆积
                if (!streamClients_.isEmpty()) {
                    for (QTcpSocket* s : streamClients_) s->disconnectFromHost();
                    streamClients_.clear();
                    emit clientDisconnected(0);
                }
            } else {
                if (needsKeyframe_ && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_free(&pkt); continue;
                }
                if (needsKeyframe_) {
                    needsKeyframe_ = false;
                    qInfo() << "MpegTsServer: first keyframe after sentinel";
                }
                if (videoSegBase_ == AV_NOPTS_VALUE) {
                    videoSegBase_ = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                }
                if (pkt->pts != AV_NOPTS_VALUE)
                    pkt->pts = pkt->pts - videoSegBase_ + videoAccumPts_;
                if (pkt->dts != AV_NOPTS_VALUE)
                    pkt->dts = pkt->dts - videoSegBase_ + videoAccumPts_;
                if (pkt->pts != AV_NOPTS_VALUE) videoLastOut_ = pkt->pts;

                if (vStream_) {
                    // 关键帧：通知 writeCallback 重置 segmentBytes_；冻结后补发 pending 客户端
                    if (pkt->flags & AV_PKT_FLAG_KEY) {
                        if (headerFrozen_) {
                            newSegmentStarting_ = true;
                        } else {
                            // 首个关键帧：冻结 headerBytes_，开始收集 segmentBytes_
                            headerFrozen_ = true;
                            newSegmentStarting_ = true;
                        }
                    }
                    pkt->stream_index = vStream_->index;
                    av_packet_rescale_ts(pkt, videoTb_, vStream_->time_base);
                    int ret = av_write_frame(fmtCtx_, pkt);
                    if (ret >= 0) {
                        avio_flush(fmtCtx_->pb);
                        if (pkt->flags & AV_PKT_FLAG_KEY)
                            qInfo() << "MpegTsServer: keyframe written, segmentBytes_="
                                    << segmentBytes_.size() << "headerBytes_=" << headerBytes_.size();
                    }
                    if (ret < 0) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "MpegTsServer: video write error:" << errbuf;
                    }
                    // 关键帧冻结后补发 pending 客户端
                    if (headerFrozen_ && !pendingClients_.isEmpty() && !newSegmentStarting_) {
                        for (auto* s : pendingClients_) {
                            if (!headerBytes_.isEmpty()) s->write(headerBytes_);
                            if (!segmentBytes_.isEmpty()) s->write(segmentBytes_);
                            streamClients_.append(s);
                        }
                        qInfo() << "MpegTsServer: flushed" << pendingClients_.size() << "pending clients";
                        pendingClients_.clear();
                    }
                }
                av_packet_free(&pkt);
            }
        }

        AVPacket* apkt = nullptr;
        bool gotAudio = audioQueue_ && audioQueue_->tryPop(apkt, 0);
        if (gotAudio) {
            if (!apkt) {
                // sentinel（seek）：重置音频 PTS 续接基点
                int64_t oneUnit = qMax((int64_t)1,
                    av_rescale_q(1, {1, 1000}, audioTb_));
                audioAccumPts_ = audioLastOut_ + oneUnit;
                audioSegBase_  = AV_NOPTS_VALUE;
                qInfo() << "MpegTsServer: audio sentinel, accumPts=" << audioAccumPts_;
            } else {
                // 关键帧门控：首个视频关键帧到来前丢弃所有音频
                if (needsKeyframe_) {
                    av_packet_free(&apkt); continue;
                }
                if (audioSegBase_ == AV_NOPTS_VALUE) {
                    audioSegBase_ = apkt->pts;
                }
                if (apkt->pts != AV_NOPTS_VALUE)
                    apkt->pts = apkt->pts - audioSegBase_ + audioAccumPts_;
                if (apkt->dts != AV_NOPTS_VALUE)
                    apkt->dts = apkt->dts - audioSegBase_ + audioAccumPts_;
                if (apkt->dts == AV_NOPTS_VALUE) apkt->dts = apkt->pts;
                if (apkt->pts != AV_NOPTS_VALUE) audioLastOut_ = apkt->pts;

                if (aStream_) {
                    apkt->stream_index = aStream_->index;
                    av_packet_rescale_ts(apkt, audioTb_, aStream_->time_base);
                    int ret = av_write_frame(fmtCtx_, apkt);
                    if (ret >= 0) avio_flush(fmtCtx_->pb);
                    if (ret < 0) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "MpegTsServer: audio write error:" << errbuf;
                    }
                }
                av_packet_free(&apkt);
            }
        }

        if (!gotVideo && !gotAudio) break;
    }
}

// 自动添加 Windows 防火墙入站规则
void MpegTsServer::addFirewallRule() {
    QString ruleName = QString("RambosPlayer-HTTPMPEGTS-%1").arg(port_);
    QProcess p;
    p.start("netsh", {"advfirewall", "firewall", "add", "rule",
                      QString("name=%1").arg(ruleName),
                      "protocol=TCP", "dir=in", "action=allow",
                      QString("localport=%1").arg(port_)});
    p.waitForFinished(3000);
    if (p.exitCode() != 0) {
        QString cmd = QString("netsh advfirewall firewall add rule name=\"%1\" "
                              "protocol=TCP dir=in action=allow localport=%2")
                      .arg(ruleName).arg(port_);
        qWarning() << "MpegTsServer: firewall rule add failed";
        emit firewallHint(cmd);
    } else {
        qInfo() << "MpegTsServer: firewall rule added for port" << port_;
    }
}

QString MpegTsServer::getLanIp() {
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        for (const auto& entry : iface.addressEntries()) {
            QString ip = entry.ip().toString();
            if (ip.startsWith("192.168.") || ip.startsWith("10.") || ip.startsWith("172."))
                return ip;
        }
    }
    return "127.0.0.1";
}

QString MpegTsServer::playerUrl() const {
    return QString("http://%1:%2/player.html").arg(getLanIp()).arg(port_);
}

// 内嵌播放页：mpegts.js + liveBufferLatencyChasing 追帧模式，自动重连
QString MpegTsServer::buildPlayerHtml() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>RambosPlayer 低延迟直播</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#000;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;position:relative}
video{width:100%;max-height:100vh}
#msg{color:#aaa;font:13px sans-serif;padding:6px;text-align:center}
#unmute-btn{position:fixed;top:16px;right:16px;background:rgba(0,0,0,0.75);color:#fff;
  font:15px sans-serif;padding:10px 18px;border-radius:6px;cursor:pointer;
  border:1px solid rgba(255,255,255,0.3);user-select:none;display:none;z-index:999}
#unmute-btn:hover{background:rgba(40,40,40,0.9)}
</style>
</head>
<body>
<video id="v" autoplay muted playsinline controls></video>
<div id="unmute-btn" onclick="doUnmute()">&#128266; 点击开声音</div>
<div id="msg">加载中...</div>
<script src="/mpegts.min.js"></script>
<script>
var v=document.getElementById('v'),msg=document.getElementById('msg');
var unmuteBtn=document.getElementById('unmute-btn');
var player=null,retryTimer=null,prevTime=-1,stallCount=0;
function setMsg(t){msg.textContent=t;}
function destroy(){if(player){try{player.unload();player.detachMediaElement();player.destroy();}catch(e){}player=null;}}
function doUnmute(){v.muted=false;v.volume=1;unmuteBtn.style.display='none';}
function connect(){
  destroy();clearTimeout(retryTimer);
  if(!window.mpegts||!mpegts.isSupported()){setMsg('浏览器不支持 MSE，请用 Chrome/Edge');return;}
  player=mpegts.createPlayer({
    type:'mpegts',url:'/stream.ts',isLive:true,
    enableStashBuffer:false,
    liveBufferLatencyChasing:true,
    liveBufferLatencyMaxLatency:1.5,
    liveBufferLatencyMinRemain:0.3,
    liveBufferLatencyChaseOffset:0.1,
    autoCleanupSourceBuffer:true,
    autoCleanupMaxBackwardDuration:2,
    autoCleanupMinBackwardDuration:1
  });
  player.attachMediaElement(v);
  player.on(mpegts.Events.ERROR,function(et,ed){
    setMsg('断开（'+et+'），2秒后重连...');retryTimer=setTimeout(connect,2000);
  });
  player.load();
  player.play().then(function(){
    if(v.muted) unmuteBtn.style.display='block';
  }).catch(function(){});
  setMsg('低延迟直播中（MPEG-TS）');
}
setInterval(function(){
  if(v.paused||v.ended)return;
  if(v.currentTime===prevTime){if(++stallCount>=2){setMsg('画面卡住，重连...');connect();stallCount=0;}}
  else{stallCount=0;}
  prevTime=v.currentTime;
},3000);
connect();
</script>
</body>
</html>)HTML";
}
