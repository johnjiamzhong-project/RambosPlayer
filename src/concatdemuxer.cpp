#include "concatdemuxer.h"
#include "logger.h"
#include <QTemporaryFile>
#include <QTextStream>
#include <QDir>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

ConcatDemuxer::ConcatDemuxer(QObject* parent) : QObject(parent) {}

// 打开单个文件，读取视频/音频 codecpar，关闭后返回（调用方负责释放输出指针）
static bool openAndGetPar(const QString& path,
                           AVCodecParameters** vpar, AVCodecParameters** apar)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    avformat_find_stream_info(ctx, nullptr);

    int vi = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (vpar) *vpar = (vi >= 0) ? avcodec_parameters_alloc() : nullptr;
    if (apar) *apar = (ai >= 0) ? avcodec_parameters_alloc() : nullptr;

    if (vpar && vi >= 0) avcodec_parameters_copy(*vpar, ctx->streams[vi]->codecpar);
    if (apar && ai >= 0) avcodec_parameters_copy(*apar, ctx->streams[ai]->codecpar);

    avformat_close_input(&ctx);
    return true;
}

bool ConcatDemuxer::checkCompatible(const QStringList& inputs) const
{
    if (inputs.size() < 2) return true;

    AVCodecParameters* refV = nullptr;
    AVCodecParameters* refA = nullptr;
    if (!openAndGetPar(inputs[0], &refV, &refA)) return false;

    bool ok = true;
    for (int i = 1; i < inputs.size() && ok; ++i) {
        AVCodecParameters* v = nullptr;
        AVCodecParameters* a = nullptr;
        if (!openAndGetPar(inputs[i], &v, &a)) { ok = false; break; }

        if (refV && v) {
            if (v->codec_id != refV->codec_id ||
                v->width    != refV->width     ||
                v->height   != refV->height    ||
                v->format   != refV->format)
                ok = false;
        } else if (refV != nullptr || v != nullptr) {
            ok = false;  // 一个有视频，另一个没有
        }

        if (ok && refA && a) {
            if (a->codec_id    != refA->codec_id ||
                a->sample_rate != refA->sample_rate ||
                a->ch_layout.nb_channels != refA->ch_layout.nb_channels)
                ok = false;
        }

        avcodec_parameters_free(&v);
        avcodec_parameters_free(&a);
    }

    avcodec_parameters_free(&refV);
    avcodec_parameters_free(&refA);
    return ok;
}

bool ConcatDemuxer::exec(const QStringList& inputs, const QString& output)
{
    // 写 ffconcat 格式临时文件（safe=0 支持绝对路径）
    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QDir::tempPath() + "/rambos_concat_XXXXXX.txt");
    if (!tmpFile.open()) {
        emit errorOccurred("无法创建临时拼接列表文件");
        return false;
    }
    {
        QTextStream ts(&tmpFile);
        ts << "ffconcat version 1.0\n";
        for (const QString& f : inputs) {
            // 写绝对路径：ffconcat 文件在临时目录，相对路径会基于临时目录解析
            QString abs = QFileInfo(f).absoluteFilePath().replace('\\', '/');
            ts << "file '" << abs << "'\n";
        }
    }
    tmpFile.flush();

    // 以 concat demuxer 打开列表文件
    const AVInputFormat* concatFmt = av_find_input_format("concat");
    if (!concatFmt) {
        emit errorOccurred("FFmpeg concat demuxer 不可用");
        return false;
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "safe", "0", 0);

    AVFormatContext* inCtx = nullptr;
    if (avformat_open_input(&inCtx, tmpFile.fileName().toUtf8().constData(),
                             concatFmt, &opts) < 0) {
        av_dict_free(&opts);
        emit errorOccurred("concat demuxer 无法打开文件列表");
        return false;
    }
    av_dict_free(&opts);
    avformat_find_stream_info(inCtx, nullptr);

    // 创建输出上下文，复制所有流参数
    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        output.toUtf8().constData()) < 0) {
        avformat_close_input(&inCtx);
        QString msg = "无法创建输出上下文（检查输出文件格式/扩展名）: " + output;
        qWarning() << "ConcatDemuxer:" << msg;
        emit errorOccurred(msg);
        return false;
    }

    for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
        AVStream* inStream  = inCtx->streams[i];
        AVStream* outStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;
    }

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, output.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            emit errorOccurred("无法写入输出文件: " + output);
            return false;
        }
    }

    {
        int ret = avformat_write_header(outCtx, nullptr);
        if (ret < 0) {
            char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
            QString msg = QString("写入文件头失败 [fmt=%1, err=%2]")
                .arg(outCtx->oformat->name).arg(errbuf);
            qWarning() << "ConcatDemuxer:" << msg;
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            emit errorOccurred(msg);
            return false;
        }
    }

    // 估算总时长（用于进度）
    int64_t totalUs = 0;
    for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
        if (inCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            int64_t dur = inCtx->streams[i]->duration;
            AVRational tb = inCtx->streams[i]->time_base;
            totalUs = av_rescale_q(dur, tb, AV_TIME_BASE_Q);
            break;
        }
    }

    // 直通复制包
    AVPacket* pkt = av_packet_alloc();
    int64_t lastPts = 0;
    while (av_read_frame(inCtx, pkt) >= 0) {
        AVStream* inStream  = inCtx->streams[pkt->stream_index];
        AVStream* outStream = outCtx->streams[pkt->stream_index];
        av_packet_rescale_ts(pkt, inStream->time_base, outStream->time_base);
        pkt->pos = -1;
        av_interleaved_write_frame(outCtx, pkt);

        // 进度（以视频包的 PTS 计）
        if (totalUs > 0 && inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
            && pkt->pts != AV_NOPTS_VALUE) {
            int64_t us = av_rescale_q(pkt->pts, outStream->time_base, AV_TIME_BASE_Q);
            if (us != lastPts) {
                lastPts = us;
                emit progressed((int)(us * 100 / totalUs));
            }
        }
        av_packet_unref(pkt);
    }

    av_write_trailer(outCtx);
    av_packet_free(&pkt);

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&outCtx->pb);
    avformat_free_context(outCtx);
    avformat_close_input(&inCtx);

    qInfo() << "ConcatDemuxer 完成:" << output;
    return true;
}
