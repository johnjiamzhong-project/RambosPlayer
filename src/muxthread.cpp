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
    videoSegBase_ = audioSegBase_ = AV_NOPTS_VALUE;
    videoAccumPts_ = audioAccumPts_ = 0;
    videoLastOut_  = audioLastOut_  = 0;
    abort_.store(false, std::memory_order_relaxed);
    url_ = url;

    bool isSrt = url.startsWith("srt://");
    const char* fmt = isSrt ? "mpegts" : "flv";

    // SRT listener 模式：PC 监听端口等待客户端连接，必须显式加 mode=listener，
    // 否则 FFmpeg 默认 caller 模式（主动连接），找不到对端直接报 I/O error。
    // srt://:9000 → srt://:9000?mode=listener
    // srt://:9000 → srt://0.0.0.0:9000?mode=listener
    // 空主机部分（srt://:port）SRT 库报 Bad parameters，必须显式写 0.0.0.0
    if (isSrt && !url.contains("mode=")) {
        url_ = url;
        url_.replace("srt://:", "srt://0.0.0.0:");
        url_ += (url_.contains('?') ? "&" : "?");
        url_ += "mode=listener&latency=50";
    }

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

    isSrt_ = isSrt;
    qInfo() << "MuxThread::init ok url=" << url_ << "fmt=" << fmt
            << "vcodec=" << (vpar ? avcodec_get_name(vpar->codec_id) : "none")
            << "acodec=" << (apar ? avcodec_get_name(apar->codec_id) : "none");
    return true;
}

void MuxThread::stop() {
    abort_ = true;
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

// 主循环：双队列暂存 + 原始 DTS 对比选包，确保音视频按各自帧率消耗。
// 旧方案（consecutiveVideo < 2）每 2 帧视频才消耗 1 帧音频，比例 2:1；
// 而 48 kHz AAC 帧率是 24 fps 视频的 ~2 倍，导致音频队列持续积压最终阻塞 DemuxThread。
// 新方案：比较两路的原始 DTS，始终选时间戳更小的那路写入，自然达到正确的 1:2 音视频比。
void MuxThread::run() {
    if (!fmtCtx_) return;

    // ---- 打开输出 IO（在后台线程执行，SRT listener 会阻塞等待客户端连接）----
    // 中断回调先于 avio_open2 注册，确保 stop() 能中断阻塞中的 SRT accept。
    fmtCtx_->interrupt_callback.callback = [](void* ctx) -> int {
        return static_cast<MuxThread*>(ctx)->abort_.load(std::memory_order_relaxed) ? 1 : 0;
    };
    fmtCtx_->interrupt_callback.opaque = this;

    if (!(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* ioOpts = nullptr;
        if (!isSrt_) av_dict_set(&ioOpts, "tcp_nodelay", "1", 0);
        qInfo() << "MuxThread: opening" << url_ << (isSrt_ ? "(waiting for client...)" : "");
        int ret = avio_open2(&fmtCtx_->pb, url_.toUtf8(), AVIO_FLAG_WRITE, nullptr, &ioOpts);
        av_dict_free(&ioOpts);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            qWarning() << "MuxThread: avio_open2 failed" << url_ << errbuf;
            emit errorOccurred(QString("推流连接失败: %1").arg(errbuf));
            return;
        }
    }

    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "MuxThread: avformat_write_header failed";
        emit errorOccurred("写入流头失败");
        return;
    }

    connected_ = true;
    emit connected();
    qInfo() << "MuxThread: connected url=" << url_;

    AVPacket* pkt  = nullptr;
    int64_t frames = 0, videoFrames = 0, audioFrames = 0;
    int64_t videoDiscarded = 0, audioDiscarded = 0;

    // 双队列暂存：每次从各自队列最多预取一包，比较 DTS 后决定处理哪路
    bool hasPV = false; AVPacket* pV = nullptr; // 暂存的视频包
    bool hasPA = false; AVPacket* pA = nullptr; // 暂存的音频包

    while (!abort_) {
        // 填充暂存槽（非阻塞）
        if (!hasPV && videoQueue_) hasPV = videoQueue_->tryPop(pV, 0);
        if (!hasPA && audioQueue_) hasPA = audioQueue_->tryPop(pA, 0);

        // 两路均空：短暂等待音频（音频帧率高，通常先到）
        if (!hasPV && !hasPA) {
            if (audioQueue_) hasPA = audioQueue_->tryPop(pA, 5);
            if (!hasPV && !hasPA) continue;
        }

        // 选包：sentinel(nullptr) 立即处理；普通包按原始 DTS 升序选（DTS 相等优先视频）
        bool pickVideo;
        if      (hasPV && !pV)  pickVideo = true;   // 视频 sentinel 立即处理
        else if (hasPA && !pA)  pickVideo = false;  // 音频 sentinel 立即处理
        else if (!hasPV)        pickVideo = false;
        else if (!hasPA)        pickVideo = true;
        else {
            double vt = (pV->dts != AV_NOPTS_VALUE ? pV->dts : pV->pts) * av_q2d(videoTimeBase_);
            double at = (pA->dts != AV_NOPTS_VALUE ? pA->dts : pA->pts) * av_q2d(audioTimeBase_);
            pickVideo = (vt <= at);
        }

        if (!pickVideo) {
            hasPA = false; pkt = pA; pA = nullptr;
            bool gotAudio = true; (void)gotAudio;

            // seek sentinel：保存上段末尾 PTS 作为累积偏移，新段续接而非归零
            if (!pkt) {
                audioAccumPts_ = audioLastOut_;
                audioSegBase_  = AV_NOPTS_VALUE;
                waitingForStart_.store(false);
                streamClock_.restart(); clockStarted_ = true;
                qInfo() << "MuxThread: audio sentinel received, audioAccum=" << audioAccumPts_;
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

            // seek 抑制期：等待视频关键帧落点（1e18），或等音频 PTS 追上关键帧
            {
                double sa = suppressAudioUntilSec_.load();
                if (sa >= 0.0) {
                    double srcSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(audioTimeBase_) : -1.0;
                    if (srcSec < sa) { av_packet_free(&pkt); continue; }
                    qInfo() << "MuxThread: audio aligned after seek PTS=" << srcSec;
                    suppressAudioUntilSec_.store(-1.0);
                }
            }

            // 音频包 PTS 连续输出：新段用 segBase 对齐，加上累积偏移续接上一段
            if (audioSegBase_ == AV_NOPTS_VALUE) {
                audioSegBase_ = pkt->pts;
                qInfo() << "MuxThread: audio seg base=" << audioSegBase_ << "accum=" << audioAccumPts_;
            }
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = pkt->pts - audioSegBase_ + audioAccumPts_;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts = pkt->dts - audioSegBase_ + audioAccumPts_;
            if (pkt->pts != AV_NOPTS_VALUE) {
                if (audioFrames > 0 && pkt->pts <= audioLastOut_)
                    qWarning() << "MuxThread: audio PTS non-monotonic"
                               << audioLastOut_ << "->" << pkt->pts
                               << "seg=" << audioSegBase_ << "accum=" << audioAccumPts_;
                audioLastOut_ = pkt->pts;
            }
            // 音频不做限速：DTS 选包模式下视频睡眠期会批量消耗音频，若再叠加
            // 音频限速睡眠会严重阻塞视频写入。音频帧率由 DemuxThread 天然控制，
            // 批量到达的音频先缓冲在接收端，不影响最终播放质量。
            // 截断：归零后 pts * time_base = 录制已过秒数，超过截止时长则退出
            {
                double d = stopDuration_.load();
                if (d >= 0 && pkt->pts * av_q2d(audioTimeBase_) > d + 0.1) {
                    qInfo() << "MuxThread: audio stop at" << pkt->pts * av_q2d(audioTimeBase_)
                            << "s (stopDuration=" << d << ")";
                    av_packet_free(&pkt); abort_ = true; break;
                }
            }
            // AAC 包在 MP4 中 DTS 常为 AV_NOPTS_VALUE，FLV 写入时需要有效 DTS
            if (pkt->dts == AV_NOPTS_VALUE) {
                pkt->dts = pkt->pts;
                if (audioFrames == 0)
                    qInfo() << "MuxThread: audio DTS missing, using PTS=" << pkt->pts * av_q2d(audioTimeBase_) << "s";
            }
            audioFrames++;
            pkt->stream_index = aStream_->index;
            av_packet_rescale_ts(pkt, audioTimeBase_, aStream_->time_base);
        } else {
            hasPV = false; pkt = pV; pV = nullptr;
            // seek sentinel：保存上段末尾 PTS 作为累积偏移，新段续接而非归零
            if (!pkt) {
                videoAccumPts_ = videoLastOut_;
                videoSegBase_  = AV_NOPTS_VALUE;
                waitingForStart_.store(false);
                streamClock_.restart(); clockStarted_ = true;
                qInfo() << "MuxThread: video sentinel received, videoAccum=" << videoAccumPts_;
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

            // seek 抑制期：丢弃 PTS < targetSec 的帧，等首个 >= targetSec 的关键帧退出。
            // 使用 AV_PKT_FLAG_KEY 而非 isIDRPacket：Open GOP 电影后续所有 I 帧均为
            // non-IDR（NAL type=1），isIDRPacket 会导致抑制期永远无法退出（frames=0）。
            {
                double sv = suppressVideoUntilSec_.load();
                if (sv >= 0.0) {
                    double srcSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(videoTimeBase_) : -1.0;
                    bool isKey    = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                    if (srcSec < sv || !isKey) { av_packet_free(&pkt); continue; }
                    qInfo() << "MuxThread: seek keyframe found PTS=" << srcSec << "target=" << sv;
                    suppressVideoUntilSec_.store(-1.0);
                    suppressAudioUntilSec_.store(srcSec); // 音频对齐到此关键帧
                }
            }

            // 视频包 PTS/DTS 连续输出：首包 DTS 作 segBase，使 DTS 从 0 起步；
            // PTS 同步减去相同偏移确保 pts >= dts（B 帧 DTS 为负时 PTS 也会被拉起，
            // 但 FFmpeg av_write_frame 硬校验要求 pts >= dts，必须保持一致）
            if (videoSegBase_ == AV_NOPTS_VALUE) {
                videoSegBase_ = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                qInfo() << "MuxThread: video seg base=" << videoSegBase_ << "accum=" << videoAccumPts_;
            }
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = pkt->pts - videoSegBase_ + videoAccumPts_;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts = pkt->dts - videoSegBase_ + videoAccumPts_;
            // DTS 必须单调递增（FLV/RTMP 要求）；PTS 对 B 帧内容天然非单调，不检查
            if (pkt->dts != AV_NOPTS_VALUE) {
                if (videoFrames > 0 && pkt->dts <= videoLastOut_)
                    qWarning() << "MuxThread: video DTS non-monotonic"
                               << videoLastOut_ << "->" << pkt->dts
                               << "seg=" << videoSegBase_ << "accum=" << videoAccumPts_;
                videoLastOut_ = pkt->dts;
            }
            // 不做限速：预读量由 mux 队列容量控制（30 帧 ≈ 1.25s），
            // seek 时最多清掉 30 帧，比旧的 200 帧队列少得多。
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
            videoFrames++;
            pkt->stream_index = vStream_->index;
            av_packet_rescale_ts(pkt, videoTimeBase_, vStream_->time_base);
        }

        // FLV/RTMP 音视频独立传输，不需要全局 DTS 排序，用 av_write_frame 直接写
        // av_interleaved_write_frame 的内部缓冲区会在音频迟到时报 EINVAL
        int writeRet = av_write_frame(fmtCtx_, pkt);
        av_packet_free(&pkt);
        if (writeRet < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(writeRet, errbuf, sizeof(errbuf));
            qWarning() << "MuxThread: write frame error:" << errbuf;
            abort_ = true;
            break;
        }
        frames++;
    }

    if (pV) av_packet_free(&pV);
    if (pA) av_packet_free(&pA);

    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    connected_ = false;
    qInfo() << "MuxThread::run finished frames=" << frames
            << "video=" << videoFrames << "audio=" << audioFrames;
    emit finished();
}
