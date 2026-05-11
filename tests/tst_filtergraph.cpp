#include <QtTest>
#include <QDebug>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include "filtergraph.h"

class TstFilterGraph : public QObject {
    Q_OBJECT

private:
    AVFrame* makeFrame(int w, int h) {
        AVFrame* f = av_frame_alloc();
        f->width  = w;
        f->height = h;
        f->format = AV_PIX_FMT_YUV420P;
        av_frame_get_buffer(f, 0);
        return f;
    }

private slots:
    // 空描述 → 直通模式，process 返回 ref（非 clone）
    void passthrough() {
        FilterGraph fg;
        QVERIFY(fg.init(320, 240, AV_PIX_FMT_YUV420P, {1, 25}, QString{}));
        QVERIFY(fg.isEmpty());

        AVFrame* in  = makeFrame(320, 240);
        AVFrame* out = av_frame_alloc();
        QCOMPARE(fg.process(in, out), 0);
        QCOMPARE(out->width,  320);
        QCOMPARE(out->height, 240);

        av_frame_free(&out);
        av_frame_free(&in);
    }

    // 有效滤镜 hflip → 输出帧尺寸不变
    void hflipFilter() {
        FilterGraph fg;
        QVERIFY(fg.init(320, 240, AV_PIX_FMT_YUV420P, {1, 25},
                        QStringLiteral("hflip")));
        QVERIFY(!fg.isEmpty());

        AVFrame* in  = makeFrame(320, 240);
        in->pts = 42;
        AVFrame* out = av_frame_alloc();
        QCOMPARE(fg.process(in, out), 0);
        QCOMPARE(out->width,  320);
        QCOMPARE(out->height, 240);

        av_frame_free(&out);
        av_frame_free(&in);
    }

    // 无效滤镜描述 → init 返回 false
    void invalidFilter() {
        FilterGraph fg;
        QVERIFY(!fg.init(320, 240, AV_PIX_FMT_YUV420P, {1, 25},
                         QStringLiteral("nonexistent_filter_xyz")));
        QVERIFY(fg.isEmpty());
    }

    // rebuild：从无效滤镜恢复到有效滤镜
    void rebuildAfterFailure() {
        FilterGraph fg;
        QVERIFY(!fg.init(320, 240, AV_PIX_FMT_YUV420P, {1, 25},
                         QStringLiteral("garbage")));

        QVERIFY(fg.rebuild(QStringLiteral("hflip")));
        QVERIFY(!fg.isEmpty());

        AVFrame* in  = makeFrame(320, 240);
        AVFrame* out = av_frame_alloc();
        QCOMPARE(fg.process(in, out), 0);
        QVERIFY(out != nullptr);

        av_frame_free(&out);
        av_frame_free(&in);
    }

    // rebuild 到空描述 → 回到直通模式
    void rebuildToPassthrough() {
        FilterGraph fg;
        QVERIFY(fg.init(320, 240, AV_PIX_FMT_YUV420P, {1, 25},
                        QStringLiteral("hflip")));
        QVERIFY(!fg.isEmpty());

        QVERIFY(fg.rebuild(QString{}));
        QVERIFY(fg.isEmpty());

        AVFrame* in  = makeFrame(320, 240);
        AVFrame* out = av_frame_alloc();
        QCOMPARE(fg.process(in, out), 0);
        QVERIFY(out != nullptr);

        av_frame_free(&out);
        av_frame_free(&in);
    }

    // 探测：列出可用的视频滤镜（一次性）
    void probeAvailableFilters() {
        const char* names[] = {
            "eq", "hue", "colorbalance", "colorlevels", "curves",
            "hflip", "vflip", "transpose", "crop", "scale",
            "overlay", "movie", "drawtext", "null", nullptr
        };
        qDebug() << "--- Available video filters ---";
        for (int i = 0; names[i]; ++i) {
            const AVFilter* f = avfilter_get_by_name(names[i]);
            qDebug() << " " << names[i] << (f ? "YES" : "NO");
        }
        QVERIFY(true);
    }
};

QTEST_MAIN(TstFilterGraph)
#include "tst_filtergraph.moc"
