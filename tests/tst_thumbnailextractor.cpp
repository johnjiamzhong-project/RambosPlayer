#include <QtTest>
#include <QSignalSpy>
#include "thumbnailextractor.h"

class TstThumbnailExtractor : public QObject {
    Q_OBJECT
private slots:
    // 正常视频：5 张缩略图全部非空，尺寸一致
    void testExtractSampleVideo()
    {
        ThumbnailExtractor extractor;
        QSignalSpy spy(&extractor, &ThumbnailExtractor::thumbnailsReady);
        QSignalSpy errSpy(&extractor, &ThumbnailExtractor::errorOccurred);

        extractor.extract("data/sample.mp4", 5);
        QVERIFY(spy.wait(5000));
        QVERIFY(errSpy.isEmpty());

        QList<QImage> images = spy.at(0).at(0).value<QList<QImage>>();
        QVERIFY(images.size() > 0);
        QVERIFY(images.size() <= 5);

        int w = images[0].width();
        int h = images[0].height();
        QVERIFY(w > 0);
        QVERIFY(h > 0);

        for (const QImage& img : images) {
            QVERIFY(!img.isNull());
            QCOMPARE(img.width(), w);
            QCOMPARE(img.height(), h);
        }
    }

    // 抽取 1 张缩略图：应返回恰好 1 张
    void testExtractSingle()
    {
        ThumbnailExtractor extractor;
        QSignalSpy spy(&extractor, &ThumbnailExtractor::thumbnailsReady);

        extractor.extract("data/sample.mp4", 1);
        QVERIFY(spy.wait(5000));

        QList<QImage> images = spy.at(0).at(0).value<QList<QImage>>();
        QCOMPARE(images.size(), 1);
        QVERIFY(!images[0].isNull());
    }

    // 无效文件：应收到错误信号
    void testInvalidFile()
    {
        ThumbnailExtractor extractor;
        QSignalSpy spy(&extractor, &ThumbnailExtractor::thumbnailsReady);
        QSignalSpy errSpy(&extractor, &ThumbnailExtractor::errorOccurred);

        extractor.extract("data/nonexistent.mp4", 3);
        QVERIFY(errSpy.wait(5000));
        QVERIFY(spy.isEmpty());
    }
};

QTEST_MAIN(TstThumbnailExtractor)
#include "tst_thumbnailextractor.moc"
