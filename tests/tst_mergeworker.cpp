#include <QtTest>
#include <QFile>
#include <QDir>
#include <QSignalSpy>

#include "mergeworker.h"
#include "concatdemuxer.h"
#include "concatfilter.h"
#include "audiomixer.h"
#include "simplemuxer.h"

extern "C" {
#include <libavformat/avformat.h>
}

// 测试素材（相对于运行目录，与其他测试保持一致）
static const char* kSample    = "tests/data/sample.mp4";     // 2 秒合成视频
static const char* kSample30s = "tests/data/sample_30s.mp4"; // 30 秒合成视频

// 读取文件总时长（微秒），失败返回 -1
static int64_t fileDuration(const QString& path)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return -1;
    avformat_find_stream_info(ctx, nullptr);
    int64_t dur = ctx->duration;
    avformat_close_input(&ctx);
    return dur;
}

// 统计文件中指定类型的流数量，失败返回 -1
static int countStreams(const QString& path, AVMediaType type)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return -1;
    avformat_find_stream_info(ctx, nullptr);
    int n = 0;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
        if (ctx->streams[i]->codecpar->codec_type == type)
            ++n;
    avformat_close_input(&ctx);
    return n;
}

// 构造临时输出路径并确保文件不存在
static QString tmpOut(const char* name)
{
    QString p = QDir::tempPath() + "/rambos_tst_" + name;
    QFile::remove(p);
    return p;
}

// ====================================================================

class TstMergeWorker : public QObject {
    Q_OBJECT

private slots:

    // ------------------------------------------------------------------
    // ConcatDemuxer
    // ------------------------------------------------------------------

    // 单文件输入视为兼容
    void checkCompatible_singleFile_returnsTrue()
    {
        ConcatDemuxer d;
        QVERIFY(d.checkCompatible({kSample}));
    }

    // 两份相同文件参数完全一致，应返回 true
    void checkCompatible_identicalFiles_returnsTrue()
    {
        ConcatDemuxer d;
        QVERIFY(d.checkCompatible({kSample, kSample}));
    }

    // 同参数无损拼接：输出时长约为两倍
    void concatDemuxer_sameParams_doublesTime()
    {
        int64_t srcDur = fileDuration(kSample);
        QVERIFY(srcDur > 0);

        QString out = tmpOut("concat_demuxer.mp4");
        ConcatDemuxer d;
        bool ok = d.exec({kSample, kSample}, out);

        QVERIFY2(ok, "ConcatDemuxer::exec 返回 false");
        QVERIFY(QFile::exists(out));
        QVERIFY(QFileInfo(out).size() > 0);

        int64_t outDur = fileDuration(out);
        // 允许 ±15% 误差（编码器首尾填充会引入小偏差）
        QVERIFY2(outDur >= srcDur * 170 / 100,
                 qPrintable(QString("时长 %1 us < 1.7× 输入").arg(outDur)));
        QVERIFY2(outDur <= srcDur * 230 / 100,
                 qPrintable(QString("时长 %1 us > 2.3× 输入").arg(outDur)));

        // 输出应含视频和音频流
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_VIDEO), 1);
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_AUDIO), 1);

        QFile::remove(out);
    }

    // 三路相同文件拼接：时长约为三倍
    void concatDemuxer_threeFiles_triplesTime()
    {
        int64_t srcDur = fileDuration(kSample);
        QVERIFY(srcDur > 0);

        QString out = tmpOut("concat_demuxer3.mp4");
        ConcatDemuxer d;
        bool ok = d.exec({kSample, kSample, kSample}, out);

        QVERIFY2(ok, "三路拼接 exec 返回 false");
        QVERIFY(QFile::exists(out));

        int64_t outDur = fileDuration(out);
        QVERIFY(outDur >= srcDur * 250 / 100);
        QVERIFY(outDur <= srcDur * 350 / 100);

        QFile::remove(out);
    }

    // ------------------------------------------------------------------
    // ConcatFilter（重编码拼接）
    // ------------------------------------------------------------------

    // 即使参数相同，ConcatFilter 也能输出带音视频的文件
    void concatFilter_twoFiles_producesOutput()
    {
        QString out = tmpOut("concat_filter.mp4");
        ConcatFilter f;
        bool ok = f.exec({kSample, kSample}, out);

        QVERIFY2(ok, "ConcatFilter::exec 返回 false");
        QVERIFY(QFile::exists(out));
        QVERIFY(QFileInfo(out).size() > 1000);
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_VIDEO), 1);
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_AUDIO), 1);

        // 时长应约为两倍
        int64_t srcDur = fileDuration(kSample);
        int64_t outDur = fileDuration(out);
        QVERIFY(outDur >= srcDur * 150 / 100);

        QFile::remove(out);
    }

    // ------------------------------------------------------------------
    // AudioMixer
    // ------------------------------------------------------------------

    // 两路混音：输出文件应含音频流，时长 ≈ 单路时长
    void audioMixer_twoFiles_producesOutput()
    {
        QString out = tmpOut("audio_mix.aac");
        AudioMixer m;
        QVector<double> vols = {1.0, 0.5};
        bool ok = m.exec({kSample, kSample}, vols, out);

        QVERIFY2(ok, "AudioMixer::exec 返回 false");
        QVERIFY(QFile::exists(out));
        QVERIFY(QFileInfo(out).size() > 0);
        QVERIFY(countStreams(out, AVMEDIA_TYPE_AUDIO) >= 1);

        QFile::remove(out);
    }

    // 默认音量（volumes 为空）不崩溃
    void audioMixer_emptyVolumes_usesDefault()
    {
        QString out = tmpOut("audio_mix_defvol.aac");
        AudioMixer m;
        bool ok = m.exec({kSample, kSample}, {}, out);

        QVERIFY2(ok, "空 volumes 时 exec 返回 false");
        QVERIFY(QFile::exists(out));

        QFile::remove(out);
    }

    // ------------------------------------------------------------------
    // SimpleMuxer
    // ------------------------------------------------------------------

    // 视频 + 音频输入：输出应同时含视频流和音频流
    void simpleMuxer_videoAndAudio_bothStreams()
    {
        QString out = tmpOut("simple_mux.mp4");
        SimpleMuxer s;
        // 用同一文件的视频轨 + 音频轨
        bool ok = s.exec(kSample, kSample, out);

        QVERIFY2(ok, "SimpleMuxer::exec 返回 false");
        QVERIFY(QFile::exists(out));
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_VIDEO), 1);
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_AUDIO), 1);

        QFile::remove(out);
    }

    // ------------------------------------------------------------------
    // MergeWorker（端对端）
    // ------------------------------------------------------------------

    // ConcatVideo 模式：同参数文件应走 D1，发射 mergeFinished(true)
    void mergeWorker_concatVideo_emitsFinished()
    {
        QString out = tmpOut("mw_concat.mp4");
        MergeWorker worker;
        MergeWorker::Task task;
        task.mode        = MergeWorker::Mode::ConcatVideo;
        task.inputFiles  = QStringList{kSample, kSample};
        task.outputFile  = out;
        worker.prepare(task);

        QSignalSpy spy(&worker, &MergeWorker::mergeFinished);
        worker.start();
        QVERIFY2(worker.wait(30000), "MergeWorker 超时（30s）");

        QCOMPARE(spy.count(), 1);
        QVERIFY2(spy.at(0).at(0).toBool(), "mergeFinished 带 false");
        QVERIFY(QFile::exists(out));

        QFile::remove(out);
    }

    // MixAudio 模式：发射 mergeFinished(true)
    void mergeWorker_mixAudio_emitsFinished()
    {
        QString out = tmpOut("mw_mix.aac");
        MergeWorker worker;
        MergeWorker::Task task;
        task.mode        = MergeWorker::Mode::MixAudio;
        task.inputFiles  = QStringList{kSample, kSample};
        task.outputFile  = out;
        task.volumes     = QVector<double>{1.0, 0.8};
        worker.prepare(task);

        QSignalSpy spy(&worker, &MergeWorker::mergeFinished);
        worker.start();
        QVERIFY2(worker.wait(30000), "MergeWorker 超时");

        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toBool());

        QFile::remove(out);
    }

    // MuxAV 模式：发射 mergeFinished(true)，输出含音视频
    void mergeWorker_muxAV_emitsFinished()
    {
        QString out = tmpOut("mw_mux.mp4");
        MergeWorker worker;
        MergeWorker::Task task;
        task.mode        = MergeWorker::Mode::MuxAV;
        task.inputFiles  = QStringList{kSample, kSample};
        task.outputFile  = out;
        worker.prepare(task);

        QSignalSpy spy(&worker, &MergeWorker::mergeFinished);
        worker.start();
        QVERIFY2(worker.wait(30000), "MergeWorker 超时");

        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toBool());
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_VIDEO), 1);
        QCOMPARE(countStreams(out, AVMEDIA_TYPE_AUDIO), 1);

        QFile::remove(out);
    }

    // 输入不足时应发射 errorOccurred，mergeFinished(false)
    void mergeWorker_insufficientInputs_emitsError()
    {
        MergeWorker worker;
        MergeWorker::Task task;
        task.mode        = MergeWorker::Mode::ConcatVideo;
        task.inputFiles  = QStringList{kSample};    // 只有 1 个，不够
        task.outputFile  = tmpOut("mw_err.mp4");
        worker.prepare(task);

        QSignalSpy spyErr(&worker,    &MergeWorker::errorOccurred);
        QSignalSpy spyDone(&worker,   &MergeWorker::mergeFinished);
        worker.start();
        worker.wait(5000);

        QVERIFY(spyErr.count() >= 1);
        QCOMPARE(spyDone.count(), 1);
        QVERIFY(!spyDone.at(0).at(0).toBool());
    }
};

QTEST_MAIN(TstMergeWorker)
#include "tst_mergeworker.moc"
