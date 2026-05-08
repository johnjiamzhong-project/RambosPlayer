#include "avsync.h"

AVSync::AVSync()
    : m_clock(-1.0)
{}

void AVSync::setAudioClock(double pts)
{
    m_clock.store(pts, std::memory_order_relaxed);
}

double AVSync::audioClock() const
{
    return m_clock.load(std::memory_order_relaxed);
}

double AVSync::videoDelay(double videoPts) const
{
    double audio = audioClock();
    if (audio < 0.0)
        return 0.0;

    double diff = videoPts - audio;   // 正值：视频超前；负值：视频落后

    if (diff < -kDropThresholdSec)
        return 0.0;                   // 落后过多，丢帧

    if (diff <= 0.0)
        return 0.0;                   // 轻微落后或同步，立即渲染

    // 视频超前：返回等待毫秒数，上限 1 秒
    return diff * 1000.0 < 1000.0 ? diff * 1000.0 : 1000.0;
}
