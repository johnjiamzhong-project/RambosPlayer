#include "httpflvserver.h"
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
}

HttpFlvServer::HttpFlvServer(QObject* parent) : QThread(parent) {}

HttpFlvServer::~HttpFlvServer() {
    stop();
    wait(5000);
    cleanupMuxer();
}

// 初始化编解码参数和 FLV muxer（不打开网络，不阻塞主线程）
bool HttpFlvServer::init(quint16 port,
                          AVCodecParameters* vpar, AVRational vtb,
                          AVCodecParameters* apar, AVRational atb) {
    port_    = port;
    videoTb_ = vtb;
    audioTb_ = atb;

    videoQueue_ = std::make_unique<FrameQueue<AVPacket*>>(30);
    audioQueue_ = std::make_unique<FrameQueue<AVPacket*>>(60);

    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, "flv", nullptr) < 0) {
        qWarning() << "HttpFlvServer: avformat_alloc_output_context2 failed";
        return false;
    }

    if (vpar) {
        vStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!vStream_) return false;
        avcodec_parameters_copy(vStream_->codecpar, vpar);
        vStream_->codecpar->codec_tag = 0;
        vStream_->time_base = vtb;
    }
    if (apar) {
        aStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!aStream_) return false;
        avcodec_parameters_copy(aStream_->codecpar, apar);
        aStream_->codecpar->codec_tag = 0;
        aStream_->time_base = atb;
        // extradata 是 AAC AudioSpecificConfig，缺失则浏览器无法解码音频
        qInfo() << "HttpFlvServer: audio stream"
                << "codec=" << avcodec_get_name(apar->codec_id)
                << "sampleRate=" << apar->sample_rate
                << "extradata_size=" << apar->extradata_size
                << "tb=" << atb.num << "/" << atb.den;
    } else {
        qWarning() << "HttpFlvServer: NO audio stream (apar=null), browser will have no audio";
    }

    qInfo() << "HttpFlvServer::init ok port=" << port_;
    return true;
}

void HttpFlvServer::stop() {
    abort_ = true;
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
    quit();
}

// 初始化 FLV muxer 的自定义 avio（run() 中调用）
bool HttpFlvServer::initMuxer() {
    const int BUF = 65536;
    uint8_t* buf = static_cast<uint8_t*>(av_malloc(BUF));
    if (!buf) return false;

    AVIOContext* avio = avio_alloc_context(buf, BUF, 1, this, nullptr, writeCallback, nullptr);
    if (!avio) { av_free(buf); return false; }

    fmtCtx_->pb    = avio;
    fmtCtx_->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "HttpFlvServer: avformat_write_header failed";
        return false;
    }
    // avformat_write_header 输出的数据（FLV 文件头 + metadata tag）
    // 已经通过 writeCallback → broadcastData → flvHeader_ 收集完毕
    qInfo() << "HttpFlvServer: muxer ready, FLV header" << flvHeader_.size() << "bytes";
    return true;
}

void HttpFlvServer::cleanupMuxer() {
    if (fmtCtx_) {
        if (fmtCtx_->pb) {
            avio_context_free(&fmtCtx_->pb);
        }
        avformat_free_context(fmtCtx_);
        fmtCtx_ = nullptr;
    }
    vStream_ = aStream_ = nullptr;
}

// FFmpeg 写数据回调：追加 FLV 头缓冲 + 广播给所有客户端
int HttpFlvServer::writeCallback(void* opaque, const uint8_t* buf, int size) {
    auto* self = static_cast<HttpFlvServer*>(opaque);
    QByteArray data(reinterpret_cast<const char*>(buf), size);
    if (!self->headerFrozen_) self->flvHeader_.append(data);
    self->broadcastData(data);
    return size;
}


// 广播 FLV 数据到所有正在接收的 HTTP 客户端
void HttpFlvServer::broadcastData(const QByteArray& data) {
    broadcastByteCount_ += data.size();
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

// 主线程：HTTP 服务 + 数据包处理定时器
void HttpFlvServer::run() {
    if (!initMuxer()) {
        emit errorOccurred("FLV muxer 初始化失败");
        return;
    }

    addFirewallRule();

    QTcpServer server;
    if (!server.listen(QHostAddress::AnyIPv4, port_)) {
        qWarning() << "HttpFlvServer: listen failed port=" << port_ << server.errorString();
        emit errorOccurred(QString("HTTP 服务启动失败（端口 %1）: %2").arg(port_).arg(server.errorString()));
        return;
    }
    qInfo() << "HttpFlvServer: listening port=" << port_ << "url=" << playerUrl();

    // 新连接：读完 HTTP 请求头后分发处理
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

    // 定时轮询 FrameQueue，处理音视频包
    QTimer pktTimer;
    connect(&pktTimer, &QTimer::timeout, [this]() { processPackets(); });
    pktTimer.start(5);

    exec();

    // 退出前写 trailer、关闭所有客户端
    if (fmtCtx_) av_write_trailer(fmtCtx_);
    for (QTcpSocket* s : streamClients_) s->disconnectFromHost();
    streamClients_.clear();
    qInfo() << "HttpFlvServer: stopped";
}

// 处理 HTTP 请求，按路径分发
void HttpFlvServer::handleRequest(QTcpSocket* socket, const QByteArray& request) {
    QString firstLine = QString::fromUtf8(request).section('\n', 0, 0).trimmed();
    QString path = firstLine.section(' ', 1, 1);

    if (path == "/" || path == "/player.html" || path.isEmpty()) {
        servePlayer(socket);
    } else if (path == "/flv.min.js") {
        serveFlvJs(socket);
    } else if (path == "/stream.flv") {
        startStreaming(socket);
    } else {
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        socket->disconnectFromHost();
    }
}

// 返回内嵌播放页面
void HttpFlvServer::servePlayer(QTcpSocket* socket) {
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

// 返回 flv.min.js（从 exe 同目录读取）
void HttpFlvServer::serveFlvJs(QTcpSocket* socket) {
    QString path = findFlvJs();
    if (path.isEmpty()) {
        QByteArray body = "/* flv.min.js not found. Place it next to the executable. */";
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/javascript\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
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

// 将客户端加入推流列表，先发 FLV 文件头，后续实时广播
void HttpFlvServer::startStreaming(QTcpSocket* socket) {
    socket->write("HTTP/1.1 200 OK\r\n"
                  "Content-Type: video/x-flv\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Connection: keep-alive\r\n"
                  "\r\n");
    if (!flvHeader_.isEmpty()) socket->write(flvHeader_);
    streamClients_.append(socket);
    emit clientConnected(streamClients_.size());
    qInfo() << "HttpFlvServer: client connected"
            << "total=" << streamClients_.size()
            << "header=" << flvHeader_.size() << "bytes"
            << "audioFrames=" << audioFrameCount_
            << "videoFrames=" << videoFrameCount_;
}

void HttpFlvServer::removeClient(QTcpSocket* socket) {
    streamClients_.removeOne(socket);
}

// 轮询队列，将 AVPacket 写入 FLV muxer（muxer 通过 writeCallback 广播）
void HttpFlvServer::processPackets() {
    if (abort_) { quit(); return; }

    // 简单轮询：先取视频后取音频，各最多消耗一批
    for (int i = 0; i < 8; ++i) {
        AVPacket* pkt = nullptr;
        bool gotVideo = videoQueue_ && videoQueue_->tryPop(pkt, 0);
        if (gotVideo) {
            if (!pkt) {
                // sentinel：重置 PTS 并进入关键帧等待
                int64_t oneMsV = av_rescale_q(1, {1, 1000}, videoTb_);
                if (oneMsV < 1) oneMsV = 1;
                videoAccumPts_ = videoLastOut_ + oneMsV;
                videoSegBase_  = AV_NOPTS_VALUE;
                needsKeyframe_ = true;
                firstKeyframeSrcSec_ = -1.0;
                qInfo() << "HttpFlvServer: video sentinel seek-align"
                        << "accumPts=" << videoAccumPts_ << "(+" << oneMsV << "tb)";
            } else {
                // 关键帧门控：丢弃非关键帧直到首个关键帧，保证 FLV 流从合法位置开始
                if (needsKeyframe_ && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_free(&pkt);
                    continue;  // 本轮 gotVideo=true，继续尝试取下一帧
                }
                if (needsKeyframe_) {
                    needsKeyframe_ = false;
                    // 记录源 PTS（重映射前），供首帧音频到来时计算 A/V 时间差
                    firstKeyframeSrcSec_ = (pkt->pts != AV_NOPTS_VALUE)
                        ? pkt->pts * av_q2d(videoTb_) : -1.0;
                    qInfo() << "HttpFlvServer: first keyframe after sentinel, PTS="
                            << firstKeyframeSrcSec_;
                }
                if (videoSegBase_ == AV_NOPTS_VALUE) {
                    videoSegBase_ = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                    bool isKey = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                    double srcSec = (pkt->pts != AV_NOPTS_VALUE)
                        ? pkt->pts * av_q2d(videoTb_) : -1.0;
                    qInfo() << "HttpFlvServer: video seg base=" << videoSegBase_
                            << "accum=" << videoAccumPts_
                            << "firstPTS=" << srcSec << "isKey=" << isKey;
                }
                if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = pkt->pts - videoSegBase_ + videoAccumPts_;
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts = pkt->dts - videoSegBase_ + videoAccumPts_;
                if (pkt->pts != AV_NOPTS_VALUE) videoLastOut_ = pkt->pts;

                if (vStream_) {
                    int rawSize = pkt->size;
                    pkt->stream_index = vStream_->index;
                    av_packet_rescale_ts(pkt, videoTb_, vStream_->time_base);
                    int ret = av_write_frame(fmtCtx_, pkt);
                    if (ret < 0) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "HttpFlvServer: video write error:" << errbuf
                                   << "DTS=" << pkt->dts << "isKey=" << (bool)(pkt->flags & AV_PKT_FLAG_KEY);
                    } else {
                        ++videoFrameCount_;
                        videoByteCount_ += rawSize;
                        // 无音频流时在此冻结；有音频流时等第一帧音频写入后再冻结，
                        // 确保 flvHeader_ 包含 AVC 序列头 + I 帧 + AAC 序列头，两路时间戳对齐
                        if (!headerFrozen_ && !aStream_ && (pkt->flags & AV_PKT_FLAG_KEY)) {
                            headerFrozen_ = true;
                            qInfo() << "HttpFlvServer: FLV header frozen (no audio) at first keyframe, size=" << flvHeader_.size();
                        }
                    }
                }
                av_packet_free(&pkt);
            }
        }

        AVPacket* apkt = nullptr;
        bool gotAudio = audioQueue_ && audioQueue_->tryPop(apkt, 0);
        if (gotAudio) {
            if (!apkt) {
                // sentinel：同 video，重置 PTS
                int64_t oneMsA = av_rescale_q(1, {1, 1000}, audioTb_);
                if (oneMsA < 1) oneMsA = 1;
                audioAccumPts_ = audioLastOut_ + oneMsA;
                audioSegBase_  = AV_NOPTS_VALUE;
                qInfo() << "HttpFlvServer: audio sentinel seek-align"
                        << "accumPts=" << audioAccumPts_ << "(+" << oneMsA << "tb)";
            } else {
                // 音频与视频统一门控：在收到第一个视频关键帧之前丢弃所有音频帧。
                // 原因：若音频先于关键帧写入 flvHeader_，会积压多秒音频数据，
                // 导致浏览器看到 audio PTS >> video PTS，音频落后画面数秒。
                // 冻结时序：关键帧写入 → flvHeader_ 含 AVC 序列头；
                //           首帧音频写入 → flvHeader_ 含 AAC 序列头；两者 PTS 对齐后冻结。
                if (needsKeyframe_) {
                    av_packet_free(&apkt);
                    continue;
                }
                if (audioSegBase_ == AV_NOPTS_VALUE) {
                    audioSegBase_ = apkt->pts;
                    // A/V 时间戳对齐：seek 后关键帧落点（videoSegBase_）可能比音频目标早若干帧。
                    // 两路均重映射到 accumPts，源时差会保留为输出时差（声音超前/落后）。
                    // 补偿方式：把音频 accumPts 向后移动 (audioSrc - keyframeSrc) 的量，
                    // 使同一源时刻的音频和视频映射到相同的输出 PTS。
                    if (firstKeyframeSrcSec_ >= 0.0 && apkt->pts != AV_NOPTS_VALUE) {
                        double audioSrcSec = apkt->pts * av_q2d(audioTb_);
                        double gapSec = audioSrcSec - firstKeyframeSrcSec_;
                        if (gapSec > 0.005) {  // >5ms 才补偿，避免浮点噪声
                            int64_t gapTicks = av_rescale_q(
                                (int64_t)(gapSec * AV_TIME_BASE),
                                AV_TIME_BASE_Q, audioTb_);
                            audioAccumPts_ += gapTicks;
                            qInfo() << "HttpFlvServer: A/V align"
                                    << "keyframe=" << firstKeyframeSrcSec_ << "s"
                                    << "audio=" << audioSrcSec << "s"
                                    << "gap=" << gapSec << "s"
                                    << "audioAccumPts adj+" << gapTicks;
                        }
                        firstKeyframeSrcSec_ = -1.0;
                    }
                    qInfo() << "HttpFlvServer: audio seg base=" << audioSegBase_
                            << "accum=" << audioAccumPts_;
                }
                if (apkt->pts != AV_NOPTS_VALUE) apkt->pts = apkt->pts - audioSegBase_ + audioAccumPts_;
                if (apkt->dts != AV_NOPTS_VALUE) apkt->dts = apkt->dts - audioSegBase_ + audioAccumPts_;
                if (apkt->dts == AV_NOPTS_VALUE) apkt->dts = apkt->pts;
                if (apkt->pts != AV_NOPTS_VALUE) audioLastOut_ = apkt->pts;

                if (aStream_) {
                    int rawSize = apkt->size;
                    apkt->stream_index = aStream_->index;
                    av_packet_rescale_ts(apkt, audioTb_, aStream_->time_base);
                    int ret = av_write_frame(fmtCtx_, apkt);
                    if (ret < 0) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "HttpFlvServer: audio write error:" << errbuf
                                   << "DTS=" << apkt->dts;
                    } else {
                        ++audioFrameCount_;
                        audioByteCount_ += rawSize;
                        if (audioFrameCount_ == 1) {
                            // 首帧音频写入后冻结：此时 flvHeader_ 已含 AVC 序列头 + I 帧 + AAC 序列头，
                            // 视频和音频 PTS 均从 ~1ms 起步，无时间戳偏差
                            if (!headerFrozen_) {
                                headerFrozen_ = true;
                                qInfo() << "HttpFlvServer: FLV header frozen after first audio+video, size="
                                        << flvHeader_.size()
                                        << "videoFrames=" << videoFrameCount_
                                        << "audioPTS=" << apkt->pts;
                            }
                            qInfo() << "HttpFlvServer: FIRST audio frame written"
                                    << "size=" << rawSize
                                    << "outPTS=" << apkt->pts
                                    << "clients=" << streamClients_.size();
                        } else if (audioFrameCount_ % 500 == 0) {
                            qInfo() << "HttpFlvServer: audio stats"
                                    << "frames=" << audioFrameCount_
                                    << "bytes=" << audioByteCount_
                                    << "video_frames=" << videoFrameCount_
                                    << "video_bytes=" << videoByteCount_
                                    << "broadcast_total=" << broadcastByteCount_
                                    << "clients=" << streamClients_.size();
                        }
                    }
                }
                av_packet_free(&apkt);
            }
        }

        if (!gotVideo && !gotAudio) break;
    }
}

// 自动添加 Windows 防火墙入站规则
void HttpFlvServer::addFirewallRule() {
    QString ruleName = QString("RambosPlayer-HTTPFLV-%1").arg(port_);
    QString cmd = QString("netsh advfirewall firewall add rule name=\"%1\" "
                          "protocol=TCP dir=in action=allow localport=%2")
                  .arg(ruleName).arg(port_);

    QProcess p;
    p.start("netsh", {"advfirewall", "firewall", "add", "rule",
                      QString("name=%1").arg(ruleName),
                      "protocol=TCP", "dir=in", "action=allow",
                      QString("localport=%1").arg(port_)});
    p.waitForFinished(3000);

    if (p.exitCode() != 0) {
        qWarning() << "HttpFlvServer: firewall rule add failed (need admin?)";
        emit firewallHint(cmd);
    } else {
        qInfo() << "HttpFlvServer: firewall rule added for port" << port_;
    }
}

QString HttpFlvServer::getLanIp() {
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

// 查找 flv.min.js：优先从 exe 同目录加载
QString HttpFlvServer::findFlvJs() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString path = appDir + "/flv.min.js";
    if (QFile::exists(path)) return path;
    return {};
}

QString HttpFlvServer::playerUrl() const {
    return QString("http://%1:%2/player.html").arg(getLanIp()).arg(port_);
}

// 生成内嵌播放页（引用 /flv.min.js，含自动重连逻辑）
// video 元素保留 muted 满足浏览器 autoplay 策略，页面启动后立即显示"开声音"按钮，
// 用户首次点击即取消静音，无需手动操作进度条。
QString HttpFlvServer::buildPlayerHtml() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>RambosPlayer 直播</title>
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
<script src="/flv.min.js"></script>
<script>
var v=document.getElementById('v'),msg=document.getElementById('msg');
var unmuteBtn=document.getElementById('unmute-btn');
var player=null,retryTimer=null,prevTime=-1,stallCount=0;
function setMsg(t){msg.textContent=t;}
function destroy(){if(player){try{player.destroy();}catch(e){}player=null;}}
function doUnmute(){v.muted=false;v.volume=1;unmuteBtn.style.display='none';}
function connect(){
  destroy();clearTimeout(retryTimer);
  if(!window.flvjs||!flvjs.isSupported()){setMsg('浏览器不支持FLV，请用Chrome/Edge');return;}
  player=flvjs.createPlayer({type:'flv',url:'/stream.flv',isLive:true,
    enableStashBuffer:false,stashInitialSize:128});
  player.attachMediaElement(v);
  player.on(flvjs.Events.ERROR,function(){setMsg('断开，2秒后重连...');retryTimer=setTimeout(connect,2000);});
  player.load();
  player.play().then(function(){
    // autoplay 成功后显示取消静音按钮（浏览器要求 muted 才能 autoplay）
    if(v.muted) unmuteBtn.style.display='block';
  }).catch(function(){});
  setMsg('播放中');
}
// 卡顿检测：3秒内 currentTime 不变则重连
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
