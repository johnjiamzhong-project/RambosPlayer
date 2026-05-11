#include <QtTest>
#include "demuxthread.h"
#include "videodecodethread.h"
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class TstHWAccel : public QObject {
    Q_OBJECT

private slots:
    // 验证硬解路径能正常解码视频帧（D3D11VA 或软解回退均接受）。
    // 测试通过条件：解码 ≥ 5 帧，且帧宽高 > 0。
    void hwaccel_decodesFrames() {
        FrameQueue<AVPacket*> vq(50);
        FrameQueue<AVPacket*> aq(200);   // sample.mp4 含音频流，必须传有效队列
        FrameQueue<AVFrame*>  fq(15);
        DemuxThread dt;
        VideoDecodeThread vd;

        QVERIFY(dt.open("tests/data/sample.mp4", &vq, &aq));

        AVFormatContext* fmt = dt.formatContext();
        int vi = dt.videoStreamIdx();
        QVERIFY(vi >= 0);

        AVCodecParameters* vp = fmt->streams[vi]->codecpar;
        // hwEnabled = true：尝试 D3D11VA，失败则静默回退软解
        QVERIFY(vd.init(vp, true));

        vd.setInputQueue(&vq);
        vd.setOutputQueue(&fq);

        dt.start();
        vd.start();

        // 等待至少 5 帧解码完成（给硬解设备创建 + 解码留足时间）
        QTRY_VERIFY_WITH_TIMEOUT(fq.size() >= 5, 10000);

        AVFrame* frame = nullptr;
        QVERIFY(fq.tryPop(frame, 1000));
        QVERIFY(frame != nullptr);
        QVERIFY(frame->width  > 0);
        QVERIFY(frame->height > 0);
        av_frame_free(&frame);

        // 停止并清理
        dt.stop(); dt.wait(3000);
        vd.stop(); vd.wait(3000);

        while (fq.tryPop(frame, 0)) av_frame_free(&frame);
        AVPacket* pkt;
        while (vq.tryPop(pkt, 0)) av_packet_free(&pkt);
        while (aq.tryPop(pkt, 0)) av_packet_free(&pkt);
    }
};

QTEST_MAIN(TstHWAccel)
#include "tst_hwaccel.moc"
