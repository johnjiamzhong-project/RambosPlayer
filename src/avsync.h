#pragma once

#include <atomic>

// 音视频同步时钟。AudioDecodeThread 持续调用 setAudioClock 更新主时钟，
// VideoRenderer 通过 videoDelay 获取当前帧应等待的毫秒数；
// 视频落后超过 400 ms 时返回 0，调用方应直接丢帧。
class AVSync {
public:
    AVSync();

    void setAudioClock(double pts);
    double audioClock() const;
    double videoDelay(double videoPts) const;

private:
    static constexpr double kDropThresholdSec = 0.4;

    std::atomic<double> m_clock;
};
