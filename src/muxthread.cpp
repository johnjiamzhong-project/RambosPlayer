#include "muxthread.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/channel_layout.h>
}

MuxThread::~MuxThread() {
    stop(); wait();
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
    }
}

// 直通模式初始化：用 avcodec_parameters_copy 复制源流参数，无需重编码。
// 协议自动识别：srt:// → MPEGTS，其余 → FLV（兼容 RTMP 和本地文件）。
// 要求源视频为 H.264（FLV 容器限制），音频格式无限制（MPEGTS 更宽容）。
bool MuxThread::init(const QString& url,
                     AVCodecParameters* vpar, AVRational vTimeBase,
                     AVCodecParameters* apar, AVRational aTimeBase) {
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
        fmtCtx_ = nullptr;
    }
    vStream_ = aStream_ = nullptr;
    videoPtsBase_ = audioPtsBase_ = AV_NOPTS_VALUE;
    abort_.store(false, std::memory_order_relaxed);
    url_ = url;

    bool isSrt = url.startsWith("srt://");
    const char* fmt = isSrt ? "mpegts" : "flv";

    // FLV 仅支持 H.264 视频；非 H.264 源给出明确错误
    if (!isSrt && vpar && vpar->codec_id != AV_CODEC_ID_H264) {
        qWarning() << "MuxThread: FLV requires H.264, source codec_id ="
                   << vpar->codec_id << "(use SRT/MPEGTS for other codecs)";
        return false;
    }

    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, fmt, nullptr) < 0) {
        qWarning() << "MuxThread: avformat_alloc_output_context2 failed";
        return false;
    }

    // ---- 视频流（直接复制源 codecpar）----
    if (vpar) {
        vStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!vStream_) return false;
        avcodec_parameters_copy(vStream_->codecpar, vpar);
        vStream_->codecpar->codec_tag = 0;  // 让封装器自动填写正确 tag
        vStream_->time_base = vTimeBase;
        videoTimeBase_      = vTimeBase;
    }

    // ---- 音频流（直接复制源 codecpar）----
    if (apar) {
        aStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!aStream_) return false;
        avcodec_parameters_copy(aStream_->codecpar, apar);
        aStream_->codecpar->codec_tag = 0;
        aStream_->time_base = aTimeBase;
        audioTimeBase_      = aTimeBase;
    }

    // ---- 打开输出 IO ----
    if (!(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx_->pb, url.toUtf8(), AVIO_FLAG_WRITE) < 0) {
            qWarning() << "MuxThread: avio_open failed" << url;
            return false;
        }
    }

    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "MuxThread: avformat_write_header failed";
        return false;
    }

    connected_ = true;
    emit connected();
    qInfo() << "MuxThread::init (copy) ok url=" << url << "fmt=" << fmt
            << "vcodec=" << (vpar ? avcodec_get_name(vpar->codec_id) : "none")
            << "acodec=" << (apar ? avcodec_get_name(apar->codec_id) : "none");
    return true;
}

void MuxThread::stop() {
    abort_ = true;
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

// 主循环：轮询视频/音频两个队列，PTS 归零后 av_interleaved_write_frame 交错写。
void MuxThread::run() {
    if (!fmtCtx_) return;

    AVPacket* pkt  = nullptr;
    int64_t frames = 0;
    int64_t videoDiscarded = 0, audioDiscarded = 0;
    int64_t videoRateLimitCount = 0, audioRateLimitCount = 0;

    while (!abort_) {
        // 优先取视频包；若无则取音频包；均无则短暂等待
        bool gotVideo = videoQueue_ && videoQueue_->tryPop(pkt, 0);
        if (!gotVideo) {
            bool gotAudio = audioQueue_ && audioQueue_->tryPop(pkt, 10);
            if (!gotAudio) continue;

            // seek sentinel：重置音频 PTS 基准，同时解除启动等待，重启实时时钟
            if (!pkt) {
                audioPtsBase_ = AV_NOPTS_VALUE;
                waitingForStart_.store(false);
                streamClock_.restart(); clockStarted_ = true;
                qInfo() << "MuxThread: audio sentinel received, start writing";
                continue;
            }

            // 等待 sentinel 期间丢弃所有包（防止 DemuxThread 超前包混入开头）
            if (waitingForStart_.load()) { av_packet_free(&pkt); continue; }

            // 丢弃早于推流起始时间的音频包，对齐播放器当前画面
            {
                double s = audioStartSec_.load();
                if (s >= 0 && pkt->pts != AV_NOPTS_VALUE) {
                    double pktSec = pkt->pts * av_q2d(audioTimeBase_);
                    if (pktSec < s) {
                        ++audioDiscarded;
                        av_packet_free(&pkt); continue;
                    }
                    qInfo() << "MuxThread: audio filter done, discarded=" << audioDiscarded
                            << "startSec=" << s << "firstPassPTS=" << pktSec;
                    audioStartSec_.store(-1.0);
                }
            }

            // 音频包 PTS 归零
            if (audioPtsBase_ == AV_NOPTS_VALUE) {
                audioPtsBase_ = pkt->pts;
                qInfo() << "MuxThread: audio PTS base =" << audioPtsBase_;
            }
            pkt->pts -= audioPtsBase_;
            pkt->dts -= audioPtsBase_;
            // 实时限速：归零后 pts 对应录制时间轴秒数，超前实时 500ms 以上则等待
            if (clockStarted_ && pkt->pts != AV_NOPTS_VALUE) {
                double outputSec = pkt->pts * av_q2d(audioTimeBase_);
                double elapsedSec = streamClock_.elapsed() / 1000.0;
                if (outputSec > elapsedSec + 0.5) {
                    int64_t sleepMs = (int64_t)((outputSec - elapsedSec - 0.5) * 1000);
                    if (sleepMs > 5) {
                        if (++audioRateLimitCount == 1)
                            qInfo() << "MuxThread: audio rate-limit first sleep" << sleepMs << "ms output=" << outputSec << "elapsed=" << elapsedSec;
                        QThread::msleep(qMin(sleepMs, (int64_t)2000));
                    }
                }
            }
            // 截断：归零后 pts * time_base = 录制已过秒数，超过截止时长则退出
            {
                double d = stopDuration_.load();
                if (d >= 0 && pkt->pts * av_q2d(audioTimeBase_) > d + 0.1) {
                    qInfo() << "MuxThread: audio stop at" << pkt->pts * av_q2d(audioTimeBase_)
                            << "s (stopDuration=" << d << ")";
                    av_packet_free(&pkt); abort_ = true; break;
                }
            }
            pkt->stream_index = aStream_->index;
            av_packet_rescale_ts(pkt, audioTimeBase_, aStream_->time_base);
        } else {
            // seek sentinel：重置视频 PTS 基准，同时解除启动等待，重启实时时钟
            if (!pkt) {
                videoPtsBase_ = AV_NOPTS_VALUE;
                waitingForStart_.store(false);
                streamClock_.restart(); clockStarted_ = true;
                qInfo() << "MuxThread: video sentinel received, start writing";
                continue;
            }

            // 等待 sentinel 期间丢弃所有包
            if (waitingForStart_.load()) { av_packet_free(&pkt); continue; }

            // 丢弃早于推流起始时间的视频包，对齐播放器当前画面
            {
                double s = videoStartSec_.load();
                if (s >= 0 && pkt->pts != AV_NOPTS_VALUE) {
                    double pktSec = pkt->pts * av_q2d(videoTimeBase_);
                    if (pktSec < s) {
                        ++videoDiscarded;
                        av_packet_free(&pkt); continue;
                    }
                    qInfo() << "MuxThread: video filter done, discarded=" << videoDiscarded
                            << "startSec=" << s << "firstPassPTS=" << pktSec;
                    videoStartSec_.store(-1.0);
                }
            }

            // 视频包 PTS 归零：用 dts 作基准，防止含 B 帧时 dts < pts 导致归零后出现负值
            if (videoPtsBase_ == AV_NOPTS_VALUE) {
                videoPtsBase_ = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                qInfo() << "MuxThread: video PTS base =" << videoPtsBase_;
            }
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= videoPtsBase_;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= videoPtsBase_;
            // 实时限速：确保视频写入速度不超过实时 + 500ms
            if (clockStarted_ && pkt->pts != AV_NOPTS_VALUE) {
                double outputSec = pkt->pts * av_q2d(videoTimeBase_);
                double elapsedSec = streamClock_.elapsed() / 1000.0;
                if (outputSec > elapsedSec + 0.5) {
                    int64_t sleepMs = (int64_t)((outputSec - elapsedSec - 0.5) * 1000);
                    if (sleepMs > 5) {
                        if (++videoRateLimitCount == 1)
                            qInfo() << "MuxThread: video rate-limit first sleep" << sleepMs << "ms output=" << outputSec << "elapsed=" << elapsedSec;
                        QThread::msleep(qMin(sleepMs, (int64_t)2000));
                    }
                }
            }
            // 截断：超过截止时长则退出，避免 DemuxThread 超前导致多录
            {
                double d = stopDuration_.load();
                if (d >= 0 && pkt->pts != AV_NOPTS_VALUE &&
                    pkt->pts * av_q2d(videoTimeBase_) > d + 0.1) {
                    qInfo() << "MuxThread: video stop at" << pkt->pts * av_q2d(videoTimeBase_)
                            << "s (stopDuration=" << d << ")";
                    av_packet_free(&pkt); abort_ = true; break;
                }
            }
            pkt->stream_index = vStream_->index;
            av_packet_rescale_ts(pkt, videoTimeBase_, vStream_->time_base);
        }

        if (av_interleaved_write_frame(fmtCtx_, pkt) < 0)
            qWarning() << "MuxThread: write frame error";

        av_packet_free(&pkt);
        frames++;
    }

    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    connected_ = false;
    qInfo() << "MuxThread::run finished frames=" << frames;
    emit finished();
}
