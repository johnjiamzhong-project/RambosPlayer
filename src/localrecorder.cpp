#include "localrecorder.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
}

LocalRecorder::~LocalRecorder() {
    stop();
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
    }
}

// 初始化本地 FLV 输出：复制源流参数，创建同名流，写文件头。
bool LocalRecorder::init(const QString& path,
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
    stopDuration_.store(-1.0, std::memory_order_relaxed);
    path_ = path;

    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, "flv", nullptr) < 0) {
        qWarning() << "LocalRecorder: avformat_alloc_output_context2 failed";
        return false;
    }

    if (vpar) {
        vStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!vStream_) return false;
        avcodec_parameters_copy(vStream_->codecpar, vpar);
        vStream_->codecpar->codec_tag = 0;
        vStream_->time_base = vTimeBase;
        videoTimeBase_ = vTimeBase;
    }

    if (apar) {
        aStream_ = avformat_new_stream(fmtCtx_, nullptr);
        if (!aStream_) return false;
        avcodec_parameters_copy(aStream_->codecpar, apar);
        aStream_->codecpar->codec_tag = 0;
        aStream_->time_base = aTimeBase;
        audioTimeBase_ = aTimeBase;
    }

    if (!(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx_->pb, path.toUtf8(), AVIO_FLAG_WRITE) < 0) {
            qWarning() << "LocalRecorder: avio_open failed" << path;
            return false;
        }
    }

    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "LocalRecorder: avformat_write_header failed";
        return false;
    }

    qInfo() << "LocalRecorder::init ok path=" << path
            << "vcodec=" << (vpar ? avcodec_get_name(vpar->codec_id) : "none")
            << "acodec=" << (apar ? avcodec_get_name(apar->codec_id) : "none");
    return true;
}

void LocalRecorder::stop() {
    abort_.store(true, std::memory_order_relaxed);
}

// seek 后重置 PTS 归零基准，新位置的包从 0 重新计时。
void LocalRecorder::resetPtsBase() {
    videoPtsBase_ = AV_NOPTS_VALUE;
    audioPtsBase_ = AV_NOPTS_VALUE;
}

void LocalRecorder::setStopDuration(double sec) {
    stopDuration_.store(sec, std::memory_order_relaxed);
}

// 写视频包：clone → PTS 归零（以首帧 dts 为基准）→ 截止检查 → 写交错帧。
// 在 DemuxThread 的 restreamMtx_ 保护下调用，确保与 stop/finish 互斥。
bool LocalRecorder::writeVideoPacket(AVPacket* pkt) {
    if (abort_.load(std::memory_order_relaxed)) return false;
    if (!vStream_) return false;

    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return false;

    if (videoPtsBase_ == AV_NOPTS_VALUE) {
        videoPtsBase_ = (copy->dts != AV_NOPTS_VALUE) ? copy->dts : copy->pts;
        qInfo() << "LocalRecorder: video PTS base =" << videoPtsBase_;
    }
    if (copy->pts != AV_NOPTS_VALUE) copy->pts -= videoPtsBase_;
    if (copy->dts != AV_NOPTS_VALUE) copy->dts -= videoPtsBase_;

    double d = stopDuration_.load(std::memory_order_relaxed);
    if (d >= 0 && copy->pts != AV_NOPTS_VALUE &&
        copy->pts * av_q2d(videoTimeBase_) > d + 0.1) {
        qInfo() << "LocalRecorder: video stop at"
                << copy->pts * av_q2d(videoTimeBase_) << "s (stopDuration=" << d << ")";
        av_packet_free(&copy);
        abort_.store(true, std::memory_order_relaxed);
        return false;
    }

    copy->stream_index = vStream_->index;
    av_packet_rescale_ts(copy, videoTimeBase_, vStream_->time_base);

    if (av_interleaved_write_frame(fmtCtx_, copy) < 0)
        qWarning() << "LocalRecorder: write video frame error";
    av_packet_free(&copy);
    return true;
}

// 写音频包：流程同视频。
bool LocalRecorder::writeAudioPacket(AVPacket* pkt) {
    if (abort_.load(std::memory_order_relaxed)) return false;
    if (!aStream_) return false;

    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return false;

    if (audioPtsBase_ == AV_NOPTS_VALUE) {
        audioPtsBase_ = copy->pts;
        qInfo() << "LocalRecorder: audio PTS base =" << audioPtsBase_;
    }
    if (copy->pts != AV_NOPTS_VALUE) copy->pts -= audioPtsBase_;
    if (copy->dts != AV_NOPTS_VALUE) copy->dts -= audioPtsBase_;

    double d = stopDuration_.load(std::memory_order_relaxed);
    if (d >= 0 && copy->pts != AV_NOPTS_VALUE &&
        copy->pts * av_q2d(audioTimeBase_) > d + 0.1) {
        qInfo() << "LocalRecorder: audio stop at"
                << copy->pts * av_q2d(audioTimeBase_) << "s (stopDuration=" << d << ")";
        av_packet_free(&copy);
        abort_.store(true, std::memory_order_relaxed);
        return false;
    }

    copy->stream_index = aStream_->index;
    av_packet_rescale_ts(copy, audioTimeBase_, aStream_->time_base);

    if (av_interleaved_write_frame(fmtCtx_, copy) < 0)
        qWarning() << "LocalRecorder: write audio frame error";
    av_packet_free(&copy);
    return true;
}

// 写文件尾并关闭 IO，之后可安全析构。
void LocalRecorder::finish() {
    if (!fmtCtx_) return;
    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    qInfo() << "LocalRecorder::finish done path=" << path_;
}
