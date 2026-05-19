// LocalRecorder：本地 FLV 同步录制器（无独立线程，无队列）
// DemuxThread 在分叉循环中直接调用 writeVideoPacket/writeAudioPacket，
// 内部 clone → PTS 归零+偏移 → 截止检查 → av_interleaved_write_frame。
// seek 时通过累积偏移保持 PTS 连续，消除文件内时间轴跳跃。
// seek 后进入抑制期：丢弃 PTS < targetSec 的帧，在首个 ≥ targetSec 的关键帧处退出，
// 音频等视频关键帧定位后再对齐，彻底消除 GOP 重叠内容。
#pragma once
#include <atomic>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class LocalRecorder {
public:
    ~LocalRecorder();

    bool init(const QString& path, AVCodecParameters* vpar, AVRational vtb,
              AVCodecParameters* apar, AVRational atb);
    void stop();
    // seek 后重置段基准并进入抑制期，等首个 ≥ targetSec 的视频关键帧后再写入
    void resetPtsBase(double fromSourceSec = -1.0, double targetSec = -1.0);
    void setStopDuration(double sec);
    bool writeVideoPacket(AVPacket* pkt);
    bool writeAudioPacket(AVPacket* pkt);
    void finish();
    QString path() const { return path_; }

private:
    // ---- 输出文件 ----
    AVFormatContext* fmtCtx_       = nullptr;
    AVStream*        vStream_      = nullptr;
    AVStream*        aStream_      = nullptr;
    AVRational       videoTimeBase_ = {1, 30};
    AVRational       audioTimeBase_ = {1, 44100};

    // ---- PTS 累积偏移 ----
    int64_t  videoSegBase_  = AV_NOPTS_VALUE;
    int64_t  audioSegBase_  = AV_NOPTS_VALUE;
    int64_t  videoAccumPts_ = 0;
    int64_t  audioAccumPts_ = 0;
    int64_t  videoLastOut_  = 0;
    int64_t  audioLastOut_  = 0;
    int64_t  videoLastWrittenDts_ = AV_NOPTS_VALUE;
    int64_t  audioLastWrittenDts_ = AV_NOPTS_VALUE;

    // ---- seek 抑制期 ----
    // suppressVideoUntilSec_: >= 0 表示抑制期激活，丢弃视频直到 PTS >= 此值且为关键帧
    // suppressAudioUntilSec_: >= 0 表示丢弃音频直到 PTS >= 此值（由视频关键帧落点设置）
    //                          1e18 = 等待视频关键帧定位中（无限抑制）
    double   suppressVideoUntilSec_ = -1.0;
    double   suppressAudioUntilSec_ = -1.0;

    // ---- 控制 ----
    std::atomic<bool>   abort_{false};
    std::atomic<double> stopDuration_{-1.0};
    int64_t  videoFrameCount_ = 0;
    int64_t  audioFrameCount_ = 0;
    QString  path_;

    bool writeVideoCopy(AVPacket* pkt);
};
