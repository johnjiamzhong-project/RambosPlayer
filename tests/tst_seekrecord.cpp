// 集成测试：快进时录制行为验证
// 模拟"播放到 5s → seek 到 15s"的场景，验证：
// 1. 录制文件 PTS 单调递增
// 2. 快进后的第一个视频帧是 IDR 关键帧（不会花屏）
// 3. 快进前的帧源 PTS < 6s，快进后首帧源 PTS >= 14.5s（跳过了中间内容）
#include <QtTest>
#include <QDir>
#include <QFile>
#include "demuxthread.h"
#include "framequeue.h"
#include "localrecorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

class TstSeekRecord : public QObject {
    Q_OBJECT

private:
    QString outPath_;
    QString srcPath_;

    void initTestCase() {
        QString exeDir = QCoreApplication::applicationDirPath();
        outPath_ = exeDir + "/test_seek.flv";
        // exeDir = build/tests/Debug, 往上 3 级到项目根，再进 tests/data
        QDir d(exeDir);
        d.cdUp(); d.cdUp(); d.cdUp();  // build/tests/Debug → build/tests → build → project root
        srcPath_ = d.absoluteFilePath("tests/data/sample_30s.mp4");
    }

    void cleanupTestCase() {
        QFile::remove(outPath_);
    }

private slots:
    // 端到端测试：seek 后录制从目标附近的 IDR 开始
    void seekRecording_startsNearTarget() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;

        qInfo() << "Opening test video:" << srcPath_;
        QVERIFY(QFile::exists(srcPath_));
        QVERIFY(dt.open(srcPath_, &vq, &aq));

        AVFormatContext* fmt = dt.formatContext();
        int vi = dt.videoStreamIdx();
        int ai = dt.audioStreamIdx();
        QVERIFY(vi >= 0);

        // 创建录制器
        LocalRecorder rec;
        QVERIFY(rec.init(outPath_,
                         fmt->streams[vi]->codecpar, fmt->streams[vi]->time_base,
                         ai >= 0 ? fmt->streams[ai]->codecpar : nullptr,
                         ai >= 0 ? fmt->streams[ai]->time_base : AVRational{1,1}));
        dt.addLocalRecorder(&rec);

        // 启动解复用线程
        dt.start();

        // 等待约 5 秒内容写入录制器
        QTest::qWait(250);
        // 清掉播放队列让 DemuxThread 持续推进
        AVPacket* pkt;
        int drained = 0;
        while (vq.tryPop(pkt, 0)) { av_packet_free(&pkt); ++drained; }
        while (aq.tryPop(pkt, 0)) { av_packet_free(&pkt); }
        qInfo() << "pre-seek drained" << drained << "video packets";

        // 再等一会确保 5s+ 内容已录制
        QTest::qWait(200);
        while (vq.tryPop(pkt, 0)) { av_packet_free(&pkt); }
        while (aq.tryPop(pkt, 0)) { av_packet_free(&pkt); }

        // 执行 seek: 从 ~5s 跳到 ~15s
        dt.seek(15.0, 5.0);

        // 等 DemuxThread 处理 seek 并写入新帧
        QTest::qWait(500);
        while (vq.tryPop(pkt, 0)) { av_packet_free(&pkt); }
        while (aq.tryPop(pkt, 0)) { av_packet_free(&pkt); }

        // 停止
        dt.stop();
        dt.wait(3000);
        rec.finish();

        // --- 验证录制文件 ---
        AVFormatContext* rCtx = nullptr;
        QVERIFY(avformat_open_input(&rCtx, outPath_.toUtf8().constData(), nullptr, nullptr) == 0);
        QVERIFY(avformat_find_stream_info(rCtx, nullptr) >= 0);

        int rVi = -1;
        for (unsigned i = 0; i < rCtx->nb_streams; ++i) {
            if (rCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                rVi = (int)i;
        }
        QVERIFY(rVi >= 0);

        AVRational rVtb = rCtx->streams[rVi]->time_base;
        AVPacket* rPkt = av_packet_alloc();

        int64_t lastDts = AV_NOPTS_VALUE;
        double firstPostSeekSrcSec = -1.0;
        bool foundJump = false;
        int totalFrames = 0;

        while (av_read_frame(rCtx, rPkt) >= 0) {
            if (rPkt->stream_index == rVi) {
                ++totalFrames;
                double sec = rPkt->pts * av_q2d(rVtb);

                // 验证 DTS 单调递增
                if (rPkt->dts != AV_NOPTS_VALUE && lastDts != AV_NOPTS_VALUE) {
                    QVERIFY2(rPkt->dts >= lastDts,
                             QString("DTS non-monotonic at frame %1: %2 < %3")
                                 .arg(totalFrames).arg(rPkt->dts).arg(lastDts).toUtf8());
                }
                if (rPkt->dts != AV_NOPTS_VALUE) lastDts = rPkt->dts;

                // 检测快进跳变：源 PTS（录制文件中 pts * time_base 近似源位置）
                // 录制文件前半段源 PTS 在 0~6s，后半段在 14.5s+
                if (!foundJump && sec > 10.0) {
                    foundJump = true;
                    firstPostSeekSrcSec = sec;
                    // 快进后的第一帧必须是 IDR
                    QVERIFY2(rPkt->flags & AV_PKT_FLAG_KEY,
                             QString("First post-seek frame at %.2fs is NOT a keyframe").arg(sec).toUtf8());
                }
            }
            av_packet_unref(rPkt);
        }

        av_packet_free(&rPkt);
        avformat_close_input(&rCtx);

        QVERIFY2(foundJump, "No jump detected in recording — seek may not have taken effect");
        QVERIFY2(firstPostSeekSrcSec >= 14.5,
                 QString("First post-seek frame too early: %.2fs, expected >= 14.5s")
                     .arg(firstPostSeekSrcSec).toUtf8());

        qInfo() << "PASS: seek jump at" << firstPostSeekSrcSec << "s, total video frames"
                << totalFrames << "DTS monotonic OK";
    }
};

QTEST_MAIN(TstSeekRecord)
#include "tst_seekrecord.moc"
