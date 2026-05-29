#pragma once
#include <QString>
#include <cstdint>

// 微秒 → "MM:SS" 或 "HH:MM:SS" 字符串
inline QString usToLabel(int64_t us)
{
    int totalSec = static_cast<int>(us / 1000000);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}
