#include "mergeworker.h"
#include "concatdemuxer.h"
#include "concatfilter.h"
#include "logger.h"
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
#include <libavutil/audio_fifo.h>
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

// 音频拼接：解码 → SWR → AVAudioFifo → 以 frame_size（AAC=1024）为单位编码 → AAC
// 必须使用 AVAudioFifo 桥接：MP3 解码帧为 1152 采样，AAC 编码器要求严格的 1024 采样，
// 直接送变长帧会导致 avcodec_send_frame 静默失败，几乎无编码输出。
static bool execAudioConcat(const QStringList& inputs, const QString& output,
                             MergeWorker* worker)
{
    const int N = inputs.size();

    AVCodecContext*  aEncCtx   = nullptr;
    AVFormatContext* outCtx    = nullptr;
    AVStream*        outStream = nullptr;
    AVAudioFifo*     fifo      = nullptr;
    AVPacket*        pkt       = nullptr;
    AVFrame*         decFrame  = nullptr;
    AVFrame*         swrFrame  = nullptr;
    AVPacket*        encPkt    = nullptr;
    bool ok = false;

    qInfo() << "[execAudioConcat] === 开始音频重编码拼接 ===";
    qInfo() << "[execAudioConcat] 输入文件数:" << N;
    qInfo() << "[execAudioConcat] 输出文件:" << output;

    // ── 从首个输入文件探测目标音频参数 ──
    int outRate = 44100;
    int outCh   = 2;
    {
        AVFormatContext* probe = nullptr;
        if (avformat_open_input(&probe, inputs[0].toUtf8().constData(), nullptr, nullptr) >= 0) {
            avformat_find_stream_info(probe, nullptr);
            int ai = av_find_best_stream(probe, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (ai >= 0) {
                outRate = probe->streams[ai]->codecpar->sample_rate;
                outCh   = probe->streams[ai]->codecpar->ch_layout.nb_channels;
            }
            avformat_close_input(&probe);
        }
    }
    qInfo() << "[execAudioConcat] 目标输出参数: sample_rate=" << outRate << " channels=" << outCh;

    // ── 先创建输出上下文，确定目标格式 ──
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        output.toUtf8().constData()) < 0) {
        emit worker->errorOccurred("无法创建输出上下文"); goto done;
    }
    qInfo() << "[execAudioConcat] 输出格式:" << outCtx->oformat->name
            << "default_audio_codec=" << avcodec_get_name(outCtx->oformat->audio_codec);

    // ── 编码器选择：根据输出格式匹配，fallback 链 ──
    {
    const AVCodec* encCodec = nullptr;
    bool needExperimental = false;
    AVCodecID targetCid = outCtx->oformat->audio_codec;  // 输出格式默认音频编码
    if (targetCid == AV_CODEC_ID_NONE) targetCid = AV_CODEC_ID_AAC;

    // 按输出格式选编码器，找不到则走 fallback
    auto tryEncoder = [&](const char* name, bool exp) -> bool {
        encCodec = avcodec_find_encoder_by_name(name);
        if (encCodec) {
            needExperimental = exp;
            return true;
        }
        return false;
    };
    auto tryCodecId = [&](AVCodecID cid, bool exp) -> bool {
        encCodec = avcodec_find_encoder(cid);
        if (encCodec) {
            needExperimental = exp;
            return true;
        }
        return false;
    };

    if (targetCid == AV_CODEC_ID_MP3) {
        // MP3 容器：libmp3lame → mp3 原生
        if (!tryEncoder("libmp3lame", false))
            tryCodecId(AV_CODEC_ID_MP3, false);
    } else {
        // AAC/M4A 容器：libfdk_aac → aac(experimental) → aac id
        if (!tryEncoder("libfdk_aac", false))
            if (!tryEncoder("aac", true))
                tryCodecId(AV_CODEC_ID_AAC, true);
    }
    // 最终回退：任意可用编码器
    if (!encCodec) {
        if (!tryEncoder("libmp3lame", false))
            if (!tryEncoder("aac", true))
                if (!tryEncoder("libfdk_aac", false))
                    tryCodecId(AV_CODEC_ID_AAC, true);
    }
    if (!encCodec) {
        emit worker->errorOccurred(
            QString::fromUtf8("无可用音频编码器 — 需要 libmp3lame 或 AAC (FFmpeg 需编译时启用)"));
        goto done;
    }
    qInfo() << "[execAudioConcat] 选用编码器:" << encCodec->name
            << "experimental=" << needExperimental
            << "目标codec=" << avcodec_get_name(targetCid);

    aEncCtx = avcodec_alloc_context3(encCodec);
    aEncCtx->sample_rate = outRate;
    av_channel_layout_default(&aEncCtx->ch_layout, outCh);
    aEncCtx->sample_fmt  = encCodec->sample_fmts[0];
    aEncCtx->bit_rate    = 192000;
    aEncCtx->time_base   = {1, outRate};
    aEncCtx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (needExperimental) {
        aEncCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }
    if (avcodec_open2(aEncCtx, encCodec, nullptr) < 0) {
        emit worker->errorOccurred(
            QString::fromUtf8("音频编码器打开失败: ") + encCodec->name);
        goto done;
    }
    qInfo() << "[execAudioConcat]" << encCodec->name << "frame_size=" << aEncCtx->frame_size;
    }

    // ── FIFO：桥接解码帧（MP3=1152）与编码帧（AAC=1024）──
    fifo = av_audio_fifo_alloc(aEncCtx->sample_fmt, outCh, aEncCtx->frame_size * 4);
    if (!fifo) { emit worker->errorOccurred("无法分配 audio FIFO"); goto done; }

    // ── 创建输出流 ──
    outStream = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_from_context(outStream->codecpar, aEncCtx);
    outStream->time_base = aEncCtx->time_base;
    qInfo() << "[execAudioConcat] 输出音频流 time_base="
            << outStream->time_base.num << "/" << outStream->time_base.den;

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
        decFrame = av_frame_alloc();
        swrFrame = av_frame_alloc();
        encPkt   = av_packet_alloc();

        // 预探总时长（进度条）
        int64_t totalUs = 0;
        int64_t totalSamples = 0;
        qInfo() << "[execAudioConcat] === 逐个文件探测时长 ===";
        for (int i = 0; i < inputs.size(); ++i) {
            AVFormatContext* probe = nullptr;
            if (avformat_open_input(&probe, inputs[i].toUtf8().constData(), nullptr, nullptr) >= 0) {
                avformat_find_stream_info(probe, nullptr);
                int64_t fileUs = av_rescale_q(probe->duration, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
                totalUs += fileUs;
                qInfo() << "  文件[" << i << "]:"
                        << inputs[i]
                        << "container_duration(us)=" << fileUs
                        << "container_duration(s)=" << (fileUs / 1000000.0);
                int ai = av_find_best_stream(probe, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                if (ai >= 0) {
                    AVStream* as = probe->streams[ai];
                    int64_t durSamples = av_rescale_q(as->duration, as->time_base, {1, as->codecpar->sample_rate});
                    totalSamples += durSamples;
                    qInfo() << "    音频流: sample_rate=" << as->codecpar->sample_rate
                            << "channels=" << as->codecpar->ch_layout.nb_channels
                            << "duration(samples)=" << durSamples
                            << "duration(s)=" << (durSamples * 1.0 / as->codecpar->sample_rate)
                            << "time_base=" << as->time_base.num << "/" << as->time_base.den;
                }
                avformat_close_input(&probe);
            }
        }
        qInfo() << "[execAudioConcat] 累计预估总时长(us)=" << totalUs
                << " S=" << (totalUs / 1000000.0) << "s";
        qInfo() << "[execAudioConcat] 累计预估总采样数=" << totalSamples
                << " S=" << (totalSamples * 1.0 / outRate) << "s";

        int64_t accumPts    = 0;  // 编码器 PTS（以采样数计）
        int64_t doneUs      = 0;
        int64_t encPktTotal = 0;

        // 从 FIFO 取帧送编码器；flush=true 时将剩余不足 frame_size 的尾帧也编码
        auto encodeFromFifo = [&](bool flush) {
            int minSamples = flush ? 1 : aEncCtx->frame_size;
            while (av_audio_fifo_size(fifo) >= minSamples) {
                int readN = qMin(av_audio_fifo_size(fifo), aEncCtx->frame_size);
                av_frame_unref(swrFrame);
                swrFrame->nb_samples  = readN;
                swrFrame->sample_rate = aEncCtx->sample_rate;
                swrFrame->format      = aEncCtx->sample_fmt;
                av_channel_layout_copy(&swrFrame->ch_layout, &aEncCtx->ch_layout);
                av_frame_get_buffer(swrFrame, 0);
                av_audio_fifo_read(fifo, (void**)swrFrame->data, readN);
                swrFrame->pts = accumPts;
                accumPts += readN;
                if (avcodec_send_frame(aEncCtx, swrFrame) < 0) break;
                while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                    av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
                    encPkt->stream_index = outStream->index;
                    encPkt->pos = -1;
                    av_write_frame(outCtx, encPkt);
                    av_packet_unref(encPkt);
                    ++encPktTotal;
                }
            }
        };

        // SWR 转换一帧（或 flush SWR 延迟）并写入 FIFO
        // inRate 为解码器采样率，srcFrame=nullptr 表示只冲刷 SWR 内部延迟
        auto swrToFifo = [&](SwrContext* swrCtx, int inRate, AVFrame* srcFrame) {
            int inSamples  = srcFrame ? srcFrame->nb_samples : 0;
            int outSamples = (int)av_rescale_rnd(
                swr_get_delay(swrCtx, inRate) + inSamples,
                outRate, inRate, AV_ROUND_UP);
            if (outSamples <= 0) return;
            av_frame_unref(swrFrame);
            swrFrame->nb_samples  = outSamples;
            swrFrame->sample_rate = outRate;
            swrFrame->format      = aEncCtx->sample_fmt;
            av_channel_layout_copy(&swrFrame->ch_layout, &aEncCtx->ch_layout);
            av_frame_get_buffer(swrFrame, 0);
            int n = swr_convert(swrCtx, swrFrame->data, outSamples,
                                srcFrame ? (const uint8_t**)srcFrame->data : nullptr,
                                inSamples);
            if (n > 0)
                av_audio_fifo_write(fifo, (void**)swrFrame->data, n);
        };

        bool loopOk = true;
        for (int i = 0; i < N && !worker->isInterruptionRequested() && loopOk; ++i) {
            AVFormatContext* inCtx  = nullptr;
            AVCodecContext*  decCtx = nullptr;
            SwrContext*      swrCtx = nullptr;
            int streamIdx = -1;

            do {  // 用 do-while(false) 实现带清理的早退
                if (avformat_open_input(&inCtx, inputs[i].toUtf8().constData(),
                                         nullptr, nullptr) < 0) {
                    emit worker->errorOccurred("无法打开: " + inputs[i]);
                    loopOk = false; break;
                }
                avformat_find_stream_info(inCtx, nullptr);
                streamIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                if (streamIdx < 0) {
                    emit worker->errorOccurred("未找到音频流: " + inputs[i]);
                    loopOk = false; break;
                }
                {
                    AVStream* as = inCtx->streams[streamIdx];
                    int64_t durUs = (as->duration != AV_NOPTS_VALUE)
                        ? av_rescale_q(as->duration, as->time_base, AV_TIME_BASE_Q) : -1;
                    qInfo() << "[execAudioConcat] 输入[" << i << "]:"
                            << inputs[i]
                            << "codec=" << avcodec_get_name(as->codecpar->codec_id)
                            << "sample_rate=" << as->codecpar->sample_rate
                            << "channels=" << as->codecpar->ch_layout.nb_channels
                            << "duration=" << durUs / 1000000.0 << "s"
                            << "accumPts_before=" << accumPts;
                }
                {
                    AVCodecParameters* par = inCtx->streams[streamIdx]->codecpar;
                    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
                    if (!codec) {
                        emit worker->errorOccurred("找不到解码器"); loopOk = false; break;
                    }
                    decCtx = avcodec_alloc_context3(codec);
                    avcodec_parameters_to_context(decCtx, par);
                    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
                        avcodec_free_context(&decCtx); loopOk = false; break;
                    }
                }

                swrCtx = swr_alloc();
                av_opt_set_chlayout   (swrCtx, "in_chlayout",   &decCtx->ch_layout, 0);
                av_opt_set_int        (swrCtx, "in_sample_rate", decCtx->sample_rate, 0);
                av_opt_set_sample_fmt (swrCtx, "in_sample_fmt",  decCtx->sample_fmt, 0);
                av_opt_set_chlayout   (swrCtx, "out_chlayout",   &aEncCtx->ch_layout, 0);
                av_opt_set_int        (swrCtx, "out_sample_rate", aEncCtx->sample_rate, 0);
                av_opt_set_sample_fmt (swrCtx, "out_sample_fmt",  aEncCtx->sample_fmt, 0);
                swr_init(swrCtx);

                int64_t fileTotalSamples = 0;  // 本文件输出的采样数
                // pass 0: 正常读包；pass 1: 刷新解码器 + SWR 延迟
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 0) {
                        while (!worker->isInterruptionRequested()) {
                            if (av_read_frame(inCtx, pkt) < 0) break;
                            if (pkt->stream_index != streamIdx) { av_packet_unref(pkt); continue; }
                            avcodec_send_packet(decCtx, pkt);
                            av_packet_unref(pkt);
                            while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
                                swrToFifo(swrCtx, decCtx->sample_rate, decFrame);
                                int64_t fifoBefore = av_audio_fifo_size(fifo);
                                encodeFromFifo(false);
                                fileTotalSamples += (fifoBefore - av_audio_fifo_size(fifo));
                                av_frame_unref(decFrame);
                            }
                        }
                    } else {
                        avcodec_send_packet(decCtx, nullptr);
                        while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
                            swrToFifo(swrCtx, decCtx->sample_rate, decFrame);
                            int64_t fifoBefore = av_audio_fifo_size(fifo);
                            encodeFromFifo(false);
                            fileTotalSamples += (fifoBefore - av_audio_fifo_size(fifo));
                            av_frame_unref(decFrame);
                        }
                        // 冲刷 SWR 内部延迟到 FIFO（srcFrame=nullptr）
                        swrToFifo(swrCtx, decCtx->sample_rate, nullptr);
                        int64_t fifoBefore = av_audio_fifo_size(fifo);
                        encodeFromFifo(false);
                        fileTotalSamples += (fifoBefore - av_audio_fifo_size(fifo));
                    }
                }

                qInfo() << "[execAudioConcat] 输入[" << i << "] 处理完毕"
                        << "accumPts_after=" << accumPts
                        << "delta=" << (accumPts - (i == 0 ? 0 : 0))  // less useful
                        << "FIFO剩余=" << av_audio_fifo_size(fifo)
                        << "本文件输出采样数=" << fileTotalSamples
                        << "本文件输出时长约=" << (fileTotalSamples * 1.0 / outRate) << "s";
                doneUs += inCtx->duration;
                if (totalUs > 0) emit worker->progressed((int)(doneUs * 100 / totalUs));
            } while (false);

            // 无论成功还是出错，都释放本文件资源
            swr_free(&swrCtx);
            avcodec_free_context(&decCtx);
            avformat_close_input(&inCtx);
        }

        // 编码 FIFO 中不足 frame_size 的尾帧，再 flush 编码器
        int64_t fifoRemainBeforeFlush = av_audio_fifo_size(fifo);
        qInfo() << "[execAudioConcat] 尾帧处理: FIFO剩余(before flush)=" << fifoRemainBeforeFlush;
        encodeFromFifo(true);
        avcodec_send_frame(aEncCtx, nullptr);
        while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
            encPkt->stream_index = outStream->index;
            encPkt->pos = -1;
            av_write_frame(outCtx, encPkt);
            av_packet_unref(encPkt);
            ++encPktTotal;
        }
        av_write_trailer(outCtx);
        qInfo() << "[execAudioConcat] 完成: 输出包数=" << encPktTotal
                << "最终accumPts=" << accumPts
                << "时长约=" << (accumPts / (double)outRate) << "s";

        // ── Log: 输出流实际时长 ──
        qInfo() << "[execAudioConcat] === 写trailer后输出流状态 ===";
        for (unsigned i = 0; i < outCtx->nb_streams; ++i) {
            AVStream* st = outCtx->streams[i];
            int64_t durUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
            qInfo() << "  输出流#" << i
                    << " type=" << av_get_media_type_string(st->codecpar->codec_type)
                    << " duration(ts)=" << st->duration
                    << " duration(us)=" << durUs
                    << " duration(s)=" << (durUs / 1000000.0)
                    << " nb_frames=" << st->nb_frames
                    << " time_base=" << st->time_base.num << "/" << st->time_base.den
                    << " start_time=" << st->start_time;
        }
    }

    ok = true;

done:
    av_audio_fifo_free(fifo);
    av_packet_free(&encPkt);
    av_frame_free(&swrFrame);
    av_frame_free(&decFrame);
    av_packet_free(&pkt);
    avcodec_free_context(&aEncCtx);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }

    // ── Log: 输出文件实际时长验证 ──
    if (ok) {
        qInfo() << "[execAudioConcat] === 输出文件验证（重新打开） ===";
        AVFormatContext* verifyCtx = nullptr;
        if (avformat_open_input(&verifyCtx, output.toUtf8().constData(), nullptr, nullptr) >= 0) {
            avformat_find_stream_info(verifyCtx, nullptr);
            qInfo() << "  container duration(us)=" << verifyCtx->duration
                    << " =" << (verifyCtx->duration / 1000000.0) << "s"
                    << " bit_rate=" << verifyCtx->bit_rate
                    << " nb_streams=" << verifyCtx->nb_streams;
            for (unsigned i = 0; i < verifyCtx->nb_streams; ++i) {
                AVStream* st = verifyCtx->streams[i];
                int64_t durUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
                qInfo() << "  流#" << i
                        << " type=" << av_get_media_type_string(st->codecpar->codec_type)
                        << " duration(us)=" << durUs
                        << " duration(s)=" << (durUs / 1000000.0)
                        << " time_base=" << st->time_base.num << "/" << st->time_base.den
                        << " start_time=" << st->start_time
                        << " nb_frames=" << st->nb_frames;
            }
            avformat_close_input(&verifyCtx);
        } else {
            qWarning() << "  无法重新打开输出文件进行验证:" << output;
        }
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
        qInfo() << "[MergeWorker::run] === ConcatVideo 模式 ===";
        qInfo() << "[MergeWorker::run] 输入文件:" << task_.inputFiles;
        qInfo() << "[MergeWorker::run] 输出文件:" << task_.outputFile;

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

    case Mode::MixAudio: {
        if (task_.inputFiles.size() < 2) {
            emit errorOccurred("音频拼接至少需要 2 个输入文件");
            break;
        }
        qInfo() << "[MergeWorker::run] === MixAudio 模式 ===";
        qInfo() << "[MergeWorker::run] 输入文件:" << task_.inputFiles;
        qInfo() << "[MergeWorker::run] 输出文件:" << task_.outputFile;

        // 优先无损拼接（参数一致），否则重编码为 AAC
        ConcatDemuxer demuxer;
        connect(&demuxer, &ConcatDemuxer::progressed,    this, &MergeWorker::progressed);
        connect(&demuxer, &ConcatDemuxer::errorOccurred, this, &MergeWorker::errorOccurred);

        if (demuxer.checkCompatible(task_.inputFiles)) {
            qInfo() << "MergeWorker: 音频参数一致，使用无损拼接";
            ok = demuxer.exec(task_.inputFiles, task_.outputFile);
        } else {
            qInfo() << "MergeWorker: 音频参数不一致，使用重编码拼接";
            ok = execAudioConcat(task_.inputFiles, task_.outputFile, this);
        }
        break;
    }

    }

    // 成功后在输出文件同目录写合成记录
    if (ok) {
        QFileInfo outInfo(task_.outputFile);
        QString logPath = outInfo.dir().filePath(
            outInfo.completeBaseName() + "_merge_log.txt");
        // 全部用 QString::fromUtf8() 显式构建，杜绝 QTextStream 编码歧义
        {
            QString text;
            text += QString::fromUtf8("合成时间: ");
            text += QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            text += QString::fromUtf8("\n合成模式: ");
            text += QString::fromUtf8((task_.mode == Mode::ConcatVideo) ? "拼接视频" : "音频拼接");
            text += QString::fromUtf8("\n输出文件: ");
            text += task_.outputFile;
            text += QString::fromUtf8("\n输入文件:\n");
            for (int i = 0; i < task_.inputFiles.size(); ++i) {
                text += QString::fromUtf8("  [%1] ").arg(i + 1);
                text += task_.inputFiles[i];
                text += QString::fromUtf8("\n");
            }

            // 同时写入输出文件的实际时长信息
            AVFormatContext* verifyCtx = nullptr;
            if (avformat_open_input(&verifyCtx, task_.outputFile.toUtf8().constData(), nullptr, nullptr) >= 0) {
                avformat_find_stream_info(verifyCtx, nullptr);
                text += QString::fromUtf8("\n--- 输出文件信息 ---\n");
                text += QString::fromUtf8("容器总时长: %1 us = %2 s\n")
                    .arg(verifyCtx->duration)
                    .arg(verifyCtx->duration / 1000000.0);
                for (unsigned i = 0; i < verifyCtx->nb_streams; ++i) {
                    AVStream* st = verifyCtx->streams[i];
                    int64_t durUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
                    text += QString::fromUtf8("流#%1: %2 时长=%3 us = %4 s time_base=%5/%6 start_time=%7\n")
                        .arg(i)
                        .arg(av_get_media_type_string(st->codecpar->codec_type))
                        .arg(durUs)
                        .arg(durUs / 1000000.0)
                        .arg(st->time_base.num).arg(st->time_base.den)
                        .arg(st->start_time);
                }
                avformat_close_input(&verifyCtx);
            }

            // 转 UTF-8 + 手动 BOM，一次性写入文件
            QByteArray bom("\xEF\xBB\xBF", 3);
            QByteArray utf8 = text.toUtf8();
            QFile logFile(logPath);
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                logFile.write(bom);
                logFile.write(utf8);
                qInfo() << "合成记录已写入(UTF-8+BOM):" << logPath;
            }
        }
    }

    qInfo() << "[MergeWorker::run] 结果: ok=" << ok << " output=" << task_.outputFile;
    emit mergeFinished(ok);
}
