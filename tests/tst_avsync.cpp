#include <QtTest>
#include "avsync.h"

class TstAVSync : public QObject {
    Q_OBJECT

private slots:
    void defaultClockIsNegative() {
        AVSync sync;
        QVERIFY(sync.audioClock() < 0.0);
    }

    void setAndGetClock() {
        AVSync sync;
        sync.setAudioClock(1.234);
        QVERIFY(qAbs(sync.audioClock() - 1.234) < 1e-9);
        sync.setAudioClock(3.75);
        QVERIFY(qAbs(sync.audioClock() - 3.75) < 1e-9);
    }

    void videoDelay_onTime() {
        AVSync sync;
        sync.setAudioClock(1.0);
        // 视频 PTS 与音频时钟相同，延迟应接近 0
        double delay = sync.videoDelay(1.0);
        QVERIFY(delay >= 0.0 && delay < 10.0);
    }

    void videoDelay_videoAhead() {
        AVSync sync;
        sync.setAudioClock(1.0);
        // 视频超前音频 200 ms，应等待约 200 ms
        double delay = sync.videoDelay(1.2);
        QVERIFY(delay >= 150.0 && delay <= 250.0);
    }

    void videoDelay_videoLate() {
        AVSync sync;
        sync.setAudioClock(2.0);
        // 视频落后音频 500 ms，超过丢帧阈值，应返回 0
        double delay = sync.videoDelay(1.5);
        QCOMPARE(delay, 0.0);
    }
};

QTEST_MAIN(TstAVSync)
#include "tst_avsync.moc"
