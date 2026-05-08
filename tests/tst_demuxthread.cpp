#include <QtTest>
#include "demuxthread.h"
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class TstDemuxThread : public QObject {
    Q_OBJECT

private slots:
    void open_validFile_returnsTrue() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(dt.open("tests/data/sample.mp4", &vq, &aq));
        QVERIFY(dt.duration() > 0);
    }

    void open_invalidFile_returnsFalse() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(!dt.open("nonexistent.mp4", &vq, &aq));
    }

    void run_populatesQueues() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(dt.open("tests/data/sample.mp4", &vq, &aq));
        dt.start();
        QTRY_VERIFY_WITH_TIMEOUT(vq.size() > 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(aq.size() > 0, 2000);
        dt.stop();
        dt.wait(2000);
        // 清理队列中残留包，避免内存泄漏
        AVPacket* pkt;
        while (vq.tryPop(pkt, 0)) av_packet_free(&pkt);
        while (aq.tryPop(pkt, 0)) av_packet_free(&pkt);
    }
};

QTEST_MAIN(TstDemuxThread)
#include "tst_demuxthread.moc"
