#include "localrecorder.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
}

// ---- 生命周期 ----

LocalRecorder::~LocalRecorder() {
    stop();
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
    }
}

// ---- 初始化 ----

bool LocalRecorder::init(const QString& path,
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
    videoLastOut_ = audioLastOut_ = 0;
    videoLastWrittenDts_ = audioLastWrittenDts_ = AV_NOPTS_VALUE;
    videoFrameCount_ = audioFrameCount_ = 0;
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

// ---- 控制 ----

void LocalRecorder::stop() {
    abort_.store(true, std::memory_order_relaxed);
}

// seek 后重置段基准并进入抑制期：
// 新段 PTS 从 videoLastOut_ 续接，但先丢弃 PTS < targetSec 的视频帧，
// 等到首个 PTS >= targetSec 的关键帧后才开始写入；音频等视频关键帧落点确定后对齐。
void LocalRecorder::resetPtsBase(double fromSourceSec, double targetSec) {
    videoAccumPts_ = videoLastOut_;
    audioAccumPts_ = audioLastOut_;
    videoSegBase_ = AV_NOPTS_VALUE;
    audioSegBase_ = AV_NOPTS_VALUE;
    videoLastWrittenDts_ = AV_NOPTS_VALUE;
    audioLastWrittenDts_ = AV_NOPTS_VALUE;
    suppressVideoUntilSec_ = (targetSec >= 0.0) ? targetSec : -1.0;
    suppressAudioUntilSec_ = (targetSec >= 0.0) ? 1e18 : -1.0; // 等视频关键帧
    qInfo() << "LocalRecorder: reset PTS base fromSec=" << fromSourceSec
            << "suppressUntil=" << suppressVideoUntilSec_
            << "vFrames=" << videoFrameCount_ << "aFrames=" << audioFrameCount_
            << "vAccum=" << videoAccumPts_ << "aAccum=" << audioAccumPts_;
}

void LocalRecorder::setStopDuration(double sec) {
    stopDuration_.store(sec, std::memory_order_relaxed);
    qInfo() << "LocalRecorder: setStopDuration" << sec << "s"
            << "vFrames=" << videoFrameCount_ << "aFrames=" << audioFrameCount_
            << "vLastOut=" << videoLastOut_;
}

// ---- -c copy 路径 ----

bool LocalRecorder::writeVideoCopy(AVPacket* pkt) {
    // 抑制期：丢弃 PTS < targetSec 的帧，等首个 >= targetSec 的关键帧才退出
    if (suppressVideoUntilSec_ >= 0.0) {
        double srcSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(videoTimeBase_) : -1.0;
        bool isKey    = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (srcSec < suppressVideoUntilSec_ || !isKey) return true; // 丢弃，继续等
        // 找到关键帧：退出视频抑制，设置音频对齐点
        qInfo() << "LocalRecorder: seek keyframe found PTS=" << srcSec
                << "(target=" << suppressVideoUntilSec_ << ") vFrames=" << videoFrameCount_;
        suppressAudioUntilSec_ = srcSec;
        suppressVideoUntilSec_ = -1.0;
    }

    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return false;

    int64_t srcPts = copy->pts, srcDts = copy->dts;

    if (videoSegBase_ == AV_NOPTS_VALUE) {
        videoSegBase_ = (copy->dts != AV_NOPTS_VALUE) ? copy->dts : copy->pts;
        qInfo() << "LocalRecorder: video seg base =" << videoSegBase_
                << "accum =" << videoAccumPts_
                << "srcPts =" << srcPts << "srcDts =" << srcDts;
    }
    if (copy->pts != AV_NOPTS_VALUE) copy->pts = copy->pts - videoSegBase_ + videoAccumPts_;
    if (copy->dts != AV_NOPTS_VALUE) copy->dts = copy->dts - videoSegBase_ + videoAccumPts_;
    if (copy->pts != AV_NOPTS_VALUE) videoLastOut_ = copy->pts;

    if (videoFrameCount_ < 5)
        qInfo() << "LocalRecorder: video frame#" << videoFrameCount_
                << "srcPts =" << srcPts << "srcDts =" << srcDts
                << "outPts =" << copy->pts
                << "outSec =" << (copy->pts != AV_NOPTS_VALUE ? copy->pts * av_q2d(videoTimeBase_) : -1);

    ++videoFrameCount_;

    double d = stopDuration_.load(std::memory_order_relaxed);
    if (d >= 0 && copy->pts != AV_NOPTS_VALUE &&
        copy->pts * av_q2d(videoTimeBase_) > d + 0.1) {
        qInfo() << "LocalRecorder: video stop at"
                << copy->pts * av_q2d(videoTimeBase_) << "s (stopDuration=" << d
                << ") frame#" << videoFrameCount_;
        av_packet_free(&copy);
        abort_.store(true, std::memory_order_relaxed);
        return false;
    }

    if (copy->dts != AV_NOPTS_VALUE && videoLastWrittenDts_ != AV_NOPTS_VALUE &&
        copy->dts < videoLastWrittenDts_) {
        av_packet_free(&copy);
        return true;
    }

    copy->stream_index = vStream_->index;
    av_packet_rescale_ts(copy, videoTimeBase_, vStream_->time_base);

    if (av_interleaved_write_frame(fmtCtx_, copy) < 0)
        qWarning() << "LocalRecorder: write video frame error frame#" << videoFrameCount_;
    else if (copy->dts != AV_NOPTS_VALUE)
        videoLastWrittenDts_ = copy->dts;
    av_packet_free(&copy);

    if (videoFrameCount_ % 100 == 0)
        qInfo() << "LocalRecorder: video progress frame#" << videoFrameCount_
                << "accum=" << videoAccumPts_ << "lastOut=" << videoLastOut_;
    return true;
}

// ---- 对外接口 ----

bool LocalRecorder::writeVideoPacket(AVPacket* pkt) {
    if (abort_.load(std::memory_order_relaxed)) {
        qInfo() << "LocalRecorder: video write blocked, abort=true frame#" << videoFrameCount_;
        return false;
    }
    if (!vStream_) return false;
    return writeVideoCopy(pkt);
}

bool LocalRecorder::writeAudioPacket(AVPacket* pkt) {
    if (abort_.load(std::memory_order_relaxed)) {
        qInfo() << "LocalRecorder: audio write blocked, abort=true frame#" << audioFrameCount_;
        return false;
    }
    if (!aStream_) return false;

    // 抑制期：视频未找到关键帧（1e18）或音频 PTS 尚未追上视频关键帧落点时丢弃
    if (suppressAudioUntilSec_ >= 0.0) {
        double srcSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(audioTimeBase_) : -1.0;
        if (srcSec < suppressAudioUntilSec_) return true;
        qInfo() << "LocalRecorder: audio aligned PTS=" << srcSec
                << "(audioTarget=" << suppressAudioUntilSec_ << ") aFrames=" << audioFrameCount_;
        suppressAudioUntilSec_ = -1.0;
    }

    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return false;

    int64_t srcPts = copy->pts;

    if (audioSegBase_ == AV_NOPTS_VALUE) {
        audioSegBase_ = copy->pts;
        qInfo() << "LocalRecorder: audio seg base =" << audioSegBase_
                << "accum =" << audioAccumPts_ << "srcPts =" << srcPts;
    }
    if (copy->pts != AV_NOPTS_VALUE) copy->pts = copy->pts - audioSegBase_ + audioAccumPts_;
    if (copy->dts != AV_NOPTS_VALUE) copy->dts = copy->dts - audioSegBase_ + audioAccumPts_;
    if (copy->pts != AV_NOPTS_VALUE) audioLastOut_ = copy->pts;

    if (audioFrameCount_ < 5)
        qInfo() << "LocalRecorder: audio frame#" << audioFrameCount_
                << "srcPts =" << srcPts
                << "outPts =" << copy->pts
                << "outSec =" << (copy->pts != AV_NOPTS_VALUE ? copy->pts * av_q2d(audioTimeBase_) : -1);

    ++audioFrameCount_;

    double d = stopDuration_.load(std::memory_order_relaxed);
    if (d >= 0 && copy->pts != AV_NOPTS_VALUE &&
        copy->pts * av_q2d(audioTimeBase_) > d + 0.1) {
        qInfo() << "LocalRecorder: audio stop at"
                << copy->pts * av_q2d(audioTimeBase_) << "s (stopDuration=" << d
                << ") frame#" << audioFrameCount_;
        av_packet_free(&copy);
        abort_.store(true, std::memory_order_relaxed);
        return false;
    }

    if (copy->dts != AV_NOPTS_VALUE && audioLastWrittenDts_ != AV_NOPTS_VALUE &&
        copy->dts < audioLastWrittenDts_) {
        av_packet_free(&copy);
        return true;
    }

    copy->stream_index = aStream_->index;
    av_packet_rescale_ts(copy, audioTimeBase_, aStream_->time_base);

    if (av_interleaved_write_frame(fmtCtx_, copy) < 0)
        qWarning() << "LocalRecorder: write audio frame error frame#" << audioFrameCount_;
    else if (copy->dts != AV_NOPTS_VALUE)
        audioLastWrittenDts_ = copy->dts;
    av_packet_free(&copy);

    if (audioFrameCount_ % 200 == 0)
        qInfo() << "LocalRecorder: audio progress frame#" << audioFrameCount_
                << "accum=" << audioAccumPts_ << "lastOut=" << audioLastOut_;
    return true;
}

void LocalRecorder::finish() {
    if (!fmtCtx_) return;
    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    qInfo() << "LocalRecorder::finish done path=" << path_;
}
