#include "mergeworker.h"
#include "concatdemuxer.h"
#include "concatfilter.h"
#include "simplemuxer.h"
#include "logger.h"
#include <vector>
#include <QTemporaryFile>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

MergeWorker::MergeWorker(QObject* parent) : QThread(parent) {}

MergeWorker::~MergeWorker()
{
    if (isRunning()) {
        requestInterruption();
        wait(5000);
    }
}

void MergeWorker::prepare(const Task& task)
{
    task_ = task;
}

// 音频拼接：逐文件顺序解码 → SwrContext 重采样 → AAC 编码，手动累积 PTS。
// 不使用 avfilter concat（该滤镜按段等待且不自动刷新解码器，容易丢帧）。
static bool execAudioConcat(const QStringList& inputs, const QString& output,
                             MergeWorker* worker)
{
    const int N        = inputs.size();
    const int outRate  = 44100;
    const int outCh    = 2;

    AVCodecContext*  aEncCtx  = nullptr;
    AVFormatContext* outCtx   = nullptr;
    AVStream*        outStream = nullptr;
    AVPacket*        pkt      = nullptr;
    AVFrame*         frame    = nullptr;
    AVFrame*         swrFrame = nullptr;
    AVPacket*        encPkt   = nullptr;
    bool ok = false;

    // ── 创建 AAC 编码器 ──
    const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!aac) { emit worker->errorOccurred("找不到 AAC 编码器"); goto done; }

    aEncCtx = avcodec_alloc_context3(aac);
    aEncCtx->sample_rate = outRate;
    av_channel_layout_default(&aEncCtx->ch_layout, outCh);
    aEncCtx->sample_fmt  = aac->sample_fmts[0];   // fltp
    aEncCtx->bit_rate    = 192000;
    aEncCtx->time_base   = {1, outRate};
    aEncCtx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(aEncCtx, aac, nullptr) < 0) {
        emit worker->errorOccurred("AAC 编码器打开失败"); goto done;
    }

    // ── 创建输出文件 ──
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        output.toUtf8().constData()) < 0) {
        emit worker->errorOccurred("无法创建输出上下文"); goto done;
    }
    outStream = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_from_context(outStream->codecpar, aEncCtx);
    outStream->time_base = aEncCtx->time_base;

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, output.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            emit worker->errorOccurred("无法写入: " + output); goto done;
        }
    }
    if (avformat_write_header(outCtx, nullptr) < 0) {
        emit worker->errorOccurred("写文件头失败"); goto done;
    }

    // ── 逐文件解码 + 编码 ──
    {
        pkt      = av_packet_alloc();
        frame    = av_frame_alloc();
        swrFrame = av_frame_alloc();
        encPkt   = av_packet_alloc();

        int64_t totalUs = 0;
        for (const QString& f : inputs) {
            AVFormatContext* probe = nullptr;
            if (avformat_open_input(&probe, f.toUtf8().constData(), nullptr, nullptr) >= 0) {
                avformat_find_stream_info(probe, nullptr);
                totalUs += av_rescale_q(probe->duration, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
                avformat_close_input(&probe);
            }
        }

        int64_t accumPts = 0;  // 编码器 PTS 基点（以采样数为单位）
        int64_t doneUs   = 0;

        // 写入编码包的 helper lambda
        auto writeEncoded = [&]() {
            while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
                encPkt->stream_index = outStream->index;
                encPkt->pos = -1;
                av_write_frame(outCtx, encPkt);
                av_packet_unref(encPkt);
            }
        };

        for (int i = 0; i < N && !worker->isInterruptionRequested(); ++i) {
            AVFormatContext* inCtx  = nullptr;
            AVCodecContext*  decCtx = nullptr;
            SwrContext*      swrCtx = nullptr;
            int streamIdx = -1;

            if (avformat_open_input(&inCtx, inputs[i].toUtf8().constData(),
                                     nullptr, nullptr) < 0) {
                emit worker->errorOccurred("无法打开: " + inputs[i]);
                ok = false; break;
            }
            avformat_find_stream_info(inCtx, nullptr);
            streamIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (streamIdx < 0) {
                emit worker->errorOccurred("未找到音频流: " + inputs[i]);
                avformat_close_input(&inCtx); ok = false; break;
            }
            {
                AVCodecParameters* par = inCtx->streams[streamIdx]->codecpar;
                const AVCodec* codec = avcodec_find_decoder(par->codec_id);
                if (!codec) {
                    emit worker->errorOccurred("找不到解码器"); avformat_close_input(&inCtx); ok = false; break;
                }
                decCtx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(decCtx, par);
                if (avcodec_open2(decCtx, codec, nullptr) < 0) {
                    avcodec_free_context(&decCtx); avformat_close_input(&inCtx); ok = false; break;
                }
            }

            // SwrContext：输入格式 → AAC 编码器格式（fltp, 44100, stereo）
            swrCtx = swr_alloc();
            av_opt_set_chlayout   (swrCtx, "in_chlayout",   &decCtx->ch_layout, 0);
            av_opt_set_int        (swrCtx, "in_sample_rate", decCtx->sample_rate, 0);
            av_opt_set_sample_fmt (swrCtx, "in_sample_fmt",  decCtx->sample_fmt, 0);
            av_opt_set_chlayout   (swrCtx, "out_chlayout",   &aEncCtx->ch_layout, 0);
            av_opt_set_int        (swrCtx, "out_sample_rate", aEncCtx->sample_rate, 0);
            av_opt_set_sample_fmt (swrCtx, "out_sample_fmt",  aEncCtx->sample_fmt, 0);
            swr_init(swrCtx);

            // 解码 + 重采样 + 编码（含最后刷新解码器）
            bool fileOk = true;
            for (int pass = 0; pass < 2 && fileOk; ++pass) {
                // pass 0: 正常读包; pass 1: 刷新解码器缓冲
                if (pass == 0) {
                    while (!worker->isInterruptionRequested()) {
                        int ret = av_read_frame(inCtx, pkt);
                        if (ret < 0) break;
                        if (pkt->stream_index != streamIdx) { av_packet_unref(pkt); continue; }
                        avcodec_send_packet(decCtx, pkt);
                        av_packet_unref(pkt);
                        while (avcodec_receive_frame(decCtx, frame) >= 0) {
                            int outSamples = (int)av_rescale_rnd(
                                swr_get_delay(swrCtx, aEncCtx->sample_rate) + frame->nb_samples,
                                aEncCtx->sample_rate, decCtx->sample_rate, AV_ROUND_UP);
                            av_frame_unref(swrFrame);
                            swrFrame->nb_samples  = outSamples;
                            swrFrame->sample_rate = aEncCtx->sample_rate;
                            swrFrame->format      = aEncCtx->sample_fmt;
                            av_channel_layout_copy(&swrFrame->ch_layout, &aEncCtx->ch_layout);
                            av_frame_get_buffer(swrFrame, 0);
                            swr_convert_frame(swrCtx, swrFrame, frame);
                            swrFrame->pts = accumPts;
                            accumPts += swrFrame->nb_samples;
                            avcodec_send_frame(aEncCtx, swrFrame);
                            writeEncoded();
                            av_frame_unref(frame);
                        }
                    }
                } else {
                    // 刷新解码器
                    avcodec_send_packet(decCtx, nullptr);
                    while (avcodec_receive_frame(decCtx, frame) >= 0) {
                        int outSamples = (int)av_rescale_rnd(
                            swr_get_delay(swrCtx, aEncCtx->sample_rate) + frame->nb_samples,
                            aEncCtx->sample_rate, decCtx->sample_rate, AV_ROUND_UP);
                        av_frame_unref(swrFrame);
                        swrFrame->nb_samples  = outSamples;
                        swrFrame->sample_rate = aEncCtx->sample_rate;
                        swrFrame->format      = aEncCtx->sample_fmt;
                        av_channel_layout_copy(&swrFrame->ch_layout, &aEncCtx->ch_layout);
                        av_frame_get_buffer(swrFrame, 0);
                        swr_convert_frame(swrCtx, swrFrame, frame);
                        swrFrame->pts = accumPts;
                        accumPts += swrFrame->nb_samples;
                        avcodec_send_frame(aEncCtx, swrFrame);
                        writeEncoded();
                        av_frame_unref(frame);
                    }
                }
            }

            doneUs += av_rescale_q(inCtx->duration, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
            if (totalUs > 0) emit worker->progressed((int)(doneUs * 100 / totalUs));

            swr_free(&swrCtx);
            avcodec_free_context(&decCtx);
            avformat_close_input(&inCtx);
        }

        // Flush 编码器
        avcodec_send_frame(aEncCtx, nullptr);
        writeEncoded();
        av_write_trailer(outCtx);
    }

    qInfo() << "execAudioConcat 完成:" << output << "accumPts";
    ok = true;

done:
    av_packet_free(&encPkt);
    av_frame_free(&swrFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&aEncCtx);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }
    return ok;
}

void MergeWorker::run()
{
    bool ok = false;

    switch (task_.mode) {

    case Mode::ConcatVideo: {
        if (task_.inputFiles.size() < 2) {
            emit errorOccurred("拼接视频至少需要 2 个输入文件");
            break;
        }
        ConcatDemuxer demuxer;
        connect(&demuxer, &ConcatDemuxer::progressed,    this, &MergeWorker::progressed);
        connect(&demuxer, &ConcatDemuxer::errorOccurred, this, &MergeWorker::errorOccurred);

        if (demuxer.checkCompatible(task_.inputFiles)) {
            qInfo() << "MergeWorker: 参数一致，使用无损拼接 (ConcatDemuxer)";
            ok = demuxer.exec(task_.inputFiles, task_.outputFile);
        } else {
            qInfo() << "MergeWorker: 参数不一致，使用重编码拼接 (ConcatFilter)";
            ConcatFilter filter;
            connect(&filter, &ConcatFilter::progressed,    this, &MergeWorker::progressed);
            connect(&filter, &ConcatFilter::errorOccurred, this, &MergeWorker::errorOccurred);
            ok = filter.exec(task_.inputFiles, task_.outputFile);
        }
        break;
    }

    case Mode::MixAudio: {  // UI 显示为"音频拼接"
        if (task_.inputFiles.size() < 2) {
            emit errorOccurred("音频拼接至少需要 2 个输入文件");
            break;
        }
        // 优先无损拼接（参数一致），否则重编码为 AAC
        ConcatDemuxer demuxer;
        connect(&demuxer, &ConcatDemuxer::progressed,    this, &MergeWorker::progressed);
        connect(&demuxer, &ConcatDemuxer::errorOccurred, this, &MergeWorker::errorOccurred);

        if (demuxer.checkCompatible(task_.inputFiles)) {
            qInfo() << "MergeWorker: 音频参数一致，使用无损拼接";
            ok = demuxer.exec(task_.inputFiles, task_.outputFile);
        } else {
            qInfo() << "MergeWorker: 音频参数不一致，使用 concat 滤镜重编码";
            ok = execAudioConcat(task_.inputFiles, task_.outputFile, this);
        }
        break;
    }

    case Mode::MuxAV: {
        if (task_.inputFiles.size() < 2) {
            emit errorOccurred("替换音频至少需要 2 个文件（视频 + 1 个以上音频）");
            break;
        }

        const QString videoFile = task_.inputFiles[0];
        QStringList audioFiles  = task_.inputFiles.mid(1);

        // 只有一个音频文件：直接替换，无需拼接
        if (audioFiles.size() == 1) {
            SimpleMuxer muxer;
            connect(&muxer, &SimpleMuxer::progressed,    this, &MergeWorker::progressed);
            connect(&muxer, &SimpleMuxer::errorOccurred, this, &MergeWorker::errorOccurred);
            ok = muxer.exec(videoFile, audioFiles[0], task_.outputFile);
            break;
        }

        // 多个音频文件：先拼接为临时文件，再替换视频原声
        qInfo() << "MuxAV: 先拼接" << audioFiles.size() << "段音频";
        QTemporaryFile tmpAudio;
        // 用 .m4a（MOV 容器）而非 .aac（ADTS），兼容 MP3/AAC/AC-3 等多种音频编码的直通复制
        tmpAudio.setFileTemplate(QDir::tempPath() + "/rambos_mux_audio_XXXXXX.m4a");
        if (!tmpAudio.open()) {
            emit errorOccurred("无法创建临时音频文件");
            break;
        }
        QString tmpPath = tmpAudio.fileName();
        tmpAudio.close();   // 让 FFmpeg 写入（不锁定文件句柄）

        // 临时音频统一重编码为 AAC，避免 MP3 等格式写入 .m4a 容器失败
        qInfo() << "MuxAV: 重编码拼接" << audioFiles.size() << "段音频 → AAC";
        bool audioOk = execAudioConcat(audioFiles, tmpPath, this);
        if (audioOk) emit progressed(50);

        if (!audioOk) {
            QFile::remove(tmpPath);
            break;
        }

        // 音频拼接完成，合入视频
        SimpleMuxer muxer;
        connect(&muxer, &SimpleMuxer::progressed, this, [this](int p){
            emit progressed(50 + p / 2);
        });
        connect(&muxer, &SimpleMuxer::errorOccurred, this, &MergeWorker::errorOccurred);
        ok = muxer.exec(videoFile, tmpPath, task_.outputFile);

        QFile::remove(tmpPath);
        break;
    }

    }

    // 成功后在输出文件同目录写合成记录
    if (ok) {
        QFileInfo outInfo(task_.outputFile);
        QString logPath = outInfo.dir().filePath(
            outInfo.completeBaseName() + "_merge_log.txt");
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&logFile);
            ts << "合成时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
            const char* modeNames[] = {"拼接视频", "音频拼接", "替换音频"};
            ts << "合成模式: " << modeNames[(int)task_.mode] << "\n";
            ts << "输出文件: " << task_.outputFile << "\n";
            ts << "输入文件:\n";
            for (int i = 0; i < task_.inputFiles.size(); ++i)
                ts << QString("  [%1] %2\n").arg(i + 1).arg(task_.inputFiles[i]);
            qInfo() << "合成记录已写入:" << logPath;
        }
    }

    emit mergeFinished(ok);
}
