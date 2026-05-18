// LocalRecorder：本地 FLV 同步录制器（无独立线程，无队列）
// DemuxThread 在分叉循环中直接调用 writeVideoPacket/writeAudioPacket，
// 内部 clone → PTS 归零 → 截止检查 → av_interleaved_write_frame。
// 消除了 MuxThread 线程+队列带来的超前读问题。
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
    void stop();                               // 原子设 abort 标志
    void resetPtsBase();                       // seek 后重置归零基准
    void setStopDuration(double sec);          // 归零后 PTS 超过此值即停止写入
    bool writeVideoPacket(AVPacket* pkt);      // 返回 false 表示已停止（abort 或超时）
    bool writeAudioPacket(AVPacket* pkt);
    void finish();                             // 写 trailer + 关闭文件
    QString path() const { return path_; }

private:
    AVFormatContext* fmtCtx_       = nullptr;          // 输出 FLV 上下文
    AVStream*        vStream_      = nullptr;          // 视频流
    AVStream*        aStream_      = nullptr;          // 音频流
    AVRational       videoTimeBase_ = {1, 30};         // 视频编码器时间基（来自源文件）
    AVRational       audioTimeBase_ = {1, 44100};      // 音频编码器时间基
    int64_t          videoPtsBase_  = AV_NOPTS_VALUE;  // 视频 PTS 归零基准
    int64_t          audioPtsBase_  = AV_NOPTS_VALUE;  // 音频 PTS 归零基准
    std::atomic<bool>   abort_{false};                 // 停止标志
    std::atomic<double> stopDuration_{-1.0};           // 截止时长（秒），-1 不截断
    QString          path_;
};
