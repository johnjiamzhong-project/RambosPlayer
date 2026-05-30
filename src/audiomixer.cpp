#include "audiomixer.h"
#include "logger.h"
#include <vector>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

AudioMixer::AudioMixer(QObject* parent) : QObject(parent) {}

// 构建 amix 滤镜字符串：[in0]volume=V0[v0];...;[v0][v1]...amix=inputs=N[out]
static QString buildAmixStr(int n, const QVector<double>& vols)
{
    QString s;
    for (int i = 0; i < n; ++i) {
        double v = (i < vols.size()) ? vols[i] : 1.0;
        s += QString("[in%1]volume=%2[v%1];").arg(i).arg(v, 0, 'f', 3);
    }
    for (int i = 0; i < n; ++i) s += QString("[v%1]").arg(i);
    s += QString("amix=inputs=%1:duration=longest:dropout_transition=2[out]").arg(n);
    return s;
}

bool AudioMixer::exec(const QStringList& inputs,
                       const QVector<double>& volumes,
                       const QString& output)
{
    const int N = inputs.size();
    if (N < 2) {
        emit errorOccurred("AudioMixer 至少需要 2 个输入");
        return false;
    }

    struct Src {
        AVFormatContext* fmtCtx   = nullptr;
        AVCodecContext*  decCtx   = nullptr;
        AVFilterContext* bufSrc   = nullptr;
        int              streamIdx = -1;
        bool             eof      = false;
    };

    std::vector<Src> srcs(N);
    AVFilterGraph*   graph    = nullptr;
    AVFilterContext* bufSink  = nullptr;
    AVCodecContext*  aEncCtx  = nullptr;
    AVFormatContext* outCtx   = nullptr;
    AVStream*        outStream = nullptr;
    AVPacket*        pkt      = nullptr;
    AVFrame*         frame    = nullptr;
    AVFrame*         filt     = nullptr;
    AVPacket*        encPkt   = nullptr;
    bool ok = false;

    // ===== 打开 N 个输入，创建解码器 =====
    for (int i = 0; i < N; ++i) {
        Src& s = srcs[i];
        if (avformat_open_input(&s.fmtCtx, inputs[i].toUtf8().constData(),
                                 nullptr, nullptr) < 0) {
            emit errorOccurred("无法打开输入: " + inputs[i]);
            goto done;
        }
        avformat_find_stream_info(s.fmtCtx, nullptr);
        s.streamIdx = av_find_best_stream(s.fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (s.streamIdx < 0) {
            emit errorOccurred("未找到音频流: " + inputs[i]);
            goto done;
        }
        {
            AVCodecParameters* par = s.fmtCtx->streams[s.streamIdx]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec) { emit errorOccurred("找不到音频解码器"); goto done; }
            s.decCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(s.decCtx, par);
            if (avcodec_open2(s.decCtx, codec, nullptr) < 0) {
                emit errorOccurred("音频解码器打开失败"); goto done;
            }
        }
    }

    // ===== 构建 avfilter 图 =====
    {
        graph = avfilter_graph_alloc();
        AVFilterInOut* gInputs  = nullptr;
        AVFilterInOut* gOutputs = nullptr;

        QString fstr = buildAmixStr(N, volumes);
        if (avfilter_graph_parse_ptr(graph, fstr.toUtf8().constData(),
                                      &gInputs, &gOutputs, nullptr) < 0) {
            emit errorOccurred("avfilter_graph_parse_ptr 失败");
            avfilter_inout_free(&gInputs);
            avfilter_inout_free(&gOutputs);
            goto done;
        }

        // 为每个自由输入创建 abuffersrc
        const AVFilter* abuf = avfilter_get_by_name("abuffer");
        for (AVFilterInOut* cur = gInputs; cur; cur = cur->next) {
            int idx = 0;
            sscanf(cur->name, "in%d", &idx);
            if (idx < 0 || idx >= N) continue;

            AVCodecContext* dec = srcs[idx].decCtx;
            char chlayout[64]; chlayout[0] = '\0';
            av_channel_layout_describe(&dec->ch_layout, chlayout, sizeof(chlayout));

            char args[256];
            snprintf(args, sizeof(args),
                     "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d",
                     dec->sample_rate,
                     av_get_sample_fmt_name(dec->sample_fmt),
                     chlayout,
                     dec->sample_rate);

            char name[32]; snprintf(name, sizeof(name), "src%d", idx);
            if (avfilter_graph_create_filter(&srcs[idx].bufSrc, abuf, name,
                                              args, nullptr, graph) < 0) {
                emit errorOccurred(QString("创建 abuffersrc[%1] 失败").arg(idx));
                avfilter_inout_free(&gInputs);
                avfilter_inout_free(&gOutputs);
                goto done;
            }
            avfilter_link(srcs[idx].bufSrc, 0, cur->filter_ctx, cur->pad_idx);
        }
        avfilter_inout_free(&gInputs);

        // 创建 abuffersink
        const AVFilter* abufSink = avfilter_get_by_name("abuffersink");
        if (avfilter_graph_create_filter(&bufSink, abufSink, "sink",
                                          nullptr, nullptr, graph) < 0) {
            emit errorOccurred("创建 abuffersink 失败");
            avfilter_inout_free(&gOutputs);
            goto done;
        }
        if (gOutputs)
            avfilter_link(gOutputs->filter_ctx, gOutputs->pad_idx, bufSink, 0);
        avfilter_inout_free(&gOutputs);

        if (avfilter_graph_config(graph, nullptr) < 0) {
            emit errorOccurred("avfilter_graph_config 失败");
            goto done;
        }
    }

    // ===== 创建 AAC 编码器 + 输出文件 =====
    {
        int outRate = av_buffersink_get_sample_rate(bufSink);
        AVChannelLayout outCh = AV_CHANNEL_LAYOUT_STEREO;
        av_buffersink_get_ch_layout(bufSink, &outCh);

        const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aac) { emit errorOccurred("找不到 AAC 编码器"); goto done; }

        aEncCtx = avcodec_alloc_context3(aac);
        aEncCtx->sample_rate = outRate;
        av_channel_layout_copy(&aEncCtx->ch_layout, &outCh);
        av_channel_layout_uninit(&outCh);
        aEncCtx->sample_fmt  = aac->sample_fmts[0];
        aEncCtx->bit_rate    = 192000;
        aEncCtx->time_base   = {1, outRate};
        aEncCtx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aEncCtx, aac, nullptr) < 0) {
            emit errorOccurred("AAC 编码器打开失败"); goto done;
        }

        if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                            output.toUtf8().constData()) < 0) {
            emit errorOccurred("无法创建输出上下文"); goto done;
        }
        outStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_from_context(outStream->codecpar, aEncCtx);
        outStream->time_base = aEncCtx->time_base;

        if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&outCtx->pb, output.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
                emit errorOccurred("无法写入输出文件: " + output); goto done;
            }
        }
        if (avformat_write_header(outCtx, nullptr) < 0) {
            emit errorOccurred("写入文件头失败"); goto done;
        }
    }

    // ===== 解码 + 滤镜 + 编码主循环 =====
    {
        pkt    = av_packet_alloc();
        frame  = av_frame_alloc();
        filt   = av_frame_alloc();
        encPkt = av_packet_alloc();

        int64_t totalUs = 0;
        for (int i = 0; i < N; ++i) {
            int64_t d = av_rescale_q(
                srcs[i].fmtCtx->streams[srcs[i].streamIdx]->duration,
                srcs[i].fmtCtx->streams[srcs[i].streamIdx]->time_base,
                AV_TIME_BASE_Q);
            totalUs = qMax(totalUs, d);
        }

        int activeSrcs = N;
        int64_t filterPts = 0;
        int64_t lastProgressUs = 0;

        // 逐源轮询解码，直到所有源 EOF
        while (activeSrcs > 0) {
            for (int i = 0; i < N; ++i) {
                Src& s = srcs[i];
                if (s.eof) continue;

                bool fed = false;
                while (!fed) {
                    int ret = av_read_frame(s.fmtCtx, pkt);
                    if (ret < 0) {
                        av_buffersrc_add_frame_flags(s.bufSrc, nullptr,
                                                      AV_BUFFERSRC_FLAG_PUSH);
                        s.eof = true;
                        --activeSrcs;
                        break;
                    }
                    if (pkt->stream_index != s.streamIdx) { av_packet_unref(pkt); continue; }

                    avcodec_send_packet(s.decCtx, pkt);
                    av_packet_unref(pkt);
                    while (avcodec_receive_frame(s.decCtx, frame) >= 0) {
                        av_buffersrc_add_frame_flags(s.bufSrc, frame,
                                                      AV_BUFFERSRC_FLAG_KEEP_REF);
                        av_frame_unref(frame);
                        fed = true;
                    }
                    if (fed) break;
                }
            }

            // 取出滤镜输出并编码
            while (av_buffersink_get_frame(bufSink, filt) >= 0) {
                filt->pts = filterPts;
                filterPts += filt->nb_samples;
                avcodec_send_frame(aEncCtx, filt);
                while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                    av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
                    encPkt->stream_index = outStream->index;
                    encPkt->pos = -1;
                    av_write_frame(outCtx, encPkt);
                    av_packet_unref(encPkt);
                }
                if (totalUs > 0) {
                    int64_t us = av_rescale_q(filterPts, aEncCtx->time_base, AV_TIME_BASE_Q);
                    if (us - lastProgressUs > 500000) {
                        lastProgressUs = us;
                        emit progressed((int)(us * 100 / totalUs));
                    }
                }
                av_frame_unref(filt);
            }
        }

        // 持续 drain sink 直到完全结束
        while (av_buffersink_get_frame(bufSink, filt) >= 0) {
            filt->pts = filterPts;
            filterPts += filt->nb_samples;
            avcodec_send_frame(aEncCtx, filt);
            while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
                encPkt->stream_index = outStream->index;
                encPkt->pos = -1;
                av_write_frame(outCtx, encPkt);
                av_packet_unref(encPkt);
            }
            av_frame_unref(filt);
        }

        // Flush 编码器
        avcodec_send_frame(aEncCtx, nullptr);
        while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
            encPkt->stream_index = outStream->index;
            encPkt->pos = -1;
            av_write_frame(outCtx, encPkt);
            av_packet_unref(encPkt);
        }

        av_write_trailer(outCtx);
    }

    qInfo() << "AudioMixer 完成:" << output;
    ok = true;

done:
    av_packet_free(&encPkt);
    av_frame_free(&filt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&aEncCtx);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }
    if (graph) avfilter_graph_free(&graph);
    for (Src& s : srcs) {
        if (s.decCtx) avcodec_free_context(&s.decCtx);
        if (s.fmtCtx) avformat_close_input(&s.fmtCtx);
    }
    return ok;
}
