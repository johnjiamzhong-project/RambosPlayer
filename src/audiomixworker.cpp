#include "audiomixworker.h"
#include "logger.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <vector>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

AudioMixWorker::AudioMixWorker(QObject* parent) : QThread(parent) {}

// 从 buffersink 取帧并编码写入输出文件，返回本次编码的帧数
int AudioMixWorker::drainAndEncode(AVFilterContext* sink, AVCodecContext* enc,
                                    AVFormatContext* out, AVStream* stream,
                                    AVFrame* filt, AVPacket* encPkt, int64_t& pts)
{
    int count = 0;
    while (av_buffersink_get_frame(sink, filt) >= 0) {
        filt->pts = pts;
        pts += filt->nb_samples;
        avcodec_send_frame(enc, filt);
        while (avcodec_receive_packet(enc, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, enc->time_base, stream->time_base);
            encPkt->stream_index = stream->index;
            encPkt->pos = -1;
            int ret = av_write_frame(out, encPkt);
            if (ret < 0) {
                char errBuf[128] = {};
                av_strerror(ret, errBuf, sizeof(errBuf));
                qWarning() << "AudioMixWorker::drainAndEncode 写入失败 ret=" << ret << errBuf;
            }
            av_packet_unref(encPkt);
        }
        av_frame_unref(filt);
        ++count;
    }
    return count;
}

void AudioMixWorker::prepare(const AudioMixTask& task)
{
    task_ = task;
}

// 构建 avfilter 滤镜字符串：
//   [in0] = 源视频音频，对各区间时间窗分段降低 srcVol
//   [in1..inN] = 各区间附加音频，经 atrim+adelay+apad+volume 处理
//   最终 amix=normalize=0:duration=first 保持与源音频等长
//
// 源音量分段算法：对所有区间边界点排序后，对每个子段取覆盖它的
// 所有区间中最小的 srcVol（衰减最大优先），每段只生成一个
// volume 滤镜，避免重叠区间被串联相乘（如两个 0.5 变成 0.25）
QString AudioMixWorker::buildFilterStr() const
{
    const int N = task_.regions.size();
    QString s;

    // 源音频：按时间分段构建 srcVol 滤镜
    s += "[in0]";
    {
        // 收集所有有衰减区间的时间边界
        std::vector<double> bounds;
        for (const auto& r : task_.regions) {
            if (r.srcVol < 0.999f) {
                bounds.push_back(r.videoStartUs / 1e6);
                bounds.push_back((r.videoStartUs + r.audioDurationUs) / 1e6);
            }
        }

        if (!bounds.empty()) {
            std::sort(bounds.begin(), bounds.end());
            bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

            bool anyFilter = false;
            for (size_t bi = 0; bi + 1 < bounds.size(); ++bi) {
                double t0  = bounds[bi];
                double t1  = bounds[bi + 1];
                double mid = (t0 + t1) * 0.5;

                // 对该子段取所有覆盖它的区间中最小的 srcVol
                float minVol = 1.0f;
                for (const auto& r : task_.regions) {
                    double rs = r.videoStartUs / 1e6;
                    double re = (r.videoStartUs + r.audioDurationUs) / 1e6;
                    if (mid >= rs && mid < re && r.srcVol < 0.999f)
                        minVol = std::min(minVol, r.srcVol);
                }

                if (minVol < 0.999f) {
                    if (anyFilter) s += ",";
                    s += QString("volume=volume=%1:enable='between(t,%2,%3)'")
                         .arg(minVol, 0, 'f', 3)
                         .arg(t0, 0, 'f', 3)
                         .arg(t1, 0, 'f', 3);
                    anyFilter = true;
                }
            }
            if (!anyFilter) s += "anull";
        } else {
            s += "anull";
        }
    }
    s += "[src_adj];";

    // 各区间附加音频
    for (int i = 0; i < N; ++i) {
        const auto& r       = task_.regions[i];
        double audioStartSec = r.audioOffsetUs / 1e6;
        int64_t videoStartMs = r.videoStartUs / 1000LL;

        s += QString("[in%1]").arg(i + 1);

        // 如有偏移或时长限制则先 trim
        if (r.audioOffsetUs > 0 || r.audioDurationUs > 0) {
            s += QString("atrim=start=%1").arg(audioStartSec, 0, 'f', 3);
            if (r.audioDurationUs > 0) {
                double audioEndSec = audioStartSec + r.audioDurationUs / 1e6;
                s += QString(":end=%1").arg(audioEndSec, 0, 'f', 3);
            }
            s += ",asetpts=PTS-STARTPTS,";
        }

        // 延迟到视频贴入点 + 静音填充 + 音量
        s += QString("adelay=%1|%1,apad,volume=%2[add%3];")
             .arg(videoStartMs)
             .arg(r.mixVol, 0, 'f', 3)
             .arg(i + 1);
    }

    // amix：normalize=0 直接叠加，duration=first 与源音频等长
    s += "[src_adj]";
    for (int i = 0; i < N; ++i)
        s += QString("[add%1]").arg(i + 1);
    s += QString("amix=inputs=%1:normalize=0:duration=first[out]").arg(N + 1);

    return s;
}

// 步骤1：将源视频音频 + 各区间音频混合，输出为临时 AAC 文件
bool AudioMixWorker::execMixAudio(const QString& tempAacPath)
{
    const int N = task_.regions.size();
    qInfo() << "=== AudioMixWorker::execMixAudio 开始 ===";
    qInfo() << "  区间数=" << N << "临时文件=" << tempAacPath;

    struct Src {
        AVFormatContext* fmtCtx    = nullptr;
        AVCodecContext*  decCtx    = nullptr;
        AVFilterContext* bufSrc    = nullptr;
        int              streamIdx = -1;
        bool             eof       = false;
    };

    // N+1 个输入：[0]=源视频音频，[1..N]=各区间音频文件
    std::vector<Src> srcs(N + 1);
    AVFilterGraph*   graph     = nullptr;
    AVFilterContext* bufSink   = nullptr;
    AVCodecContext*  aEncCtx   = nullptr;
    AVFormatContext* outCtx    = nullptr;
    AVStream*        outStream = nullptr;
    AVPacket*        pkt       = nullptr;
    AVFrame*         frame     = nullptr;
    AVFrame*         filt      = nullptr;
    AVPacket*        encPkt    = nullptr;
    bool ok = false;

    // 打开源视频取音频流
    {
        Src& s = srcs[0];
        if (avformat_open_input(&s.fmtCtx, task_.originalVideoPath.toUtf8().constData(),
                                nullptr, nullptr) < 0) {
            emit errorOccurred("无法打开源视频: " + task_.originalVideoPath);
            goto done;
        }
        avformat_find_stream_info(s.fmtCtx, nullptr);
        s.streamIdx = av_find_best_stream(s.fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (s.streamIdx < 0) { emit errorOccurred("源视频无音频流"); goto done; }
        {
            AVCodecParameters* par = s.fmtCtx->streams[s.streamIdx]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec) { emit errorOccurred("找不到源视频音频解码器"); goto done; }
            s.decCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(s.decCtx, par);
            if (avcodec_open2(s.decCtx, codec, nullptr) < 0) {
                emit errorOccurred("源视频音频解码器打开失败"); goto done;
            }
            qInfo() << "AudioMixWorker: 源视频音频 codec=" << codec->name
                     << "sr=" << s.decCtx->sample_rate << "ch=" << s.decCtx->ch_layout.nb_channels
                     << "duration=" << s.fmtCtx->duration;
        }
    }

    // 打开各区间音频文件
    for (int i = 0; i < N; ++i) {
        Src& s = srcs[i + 1];
        const QString& path = task_.regions[i].sourcePath;
        if (avformat_open_input(&s.fmtCtx, path.toUtf8().constData(), nullptr, nullptr) < 0) {
            emit errorOccurred("无法打开音频: " + path); goto done;
        }
        avformat_find_stream_info(s.fmtCtx, nullptr);
        s.streamIdx = av_find_best_stream(s.fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (s.streamIdx < 0) { emit errorOccurred("无音频流: " + path); goto done; }
        {
            AVCodecParameters* par = s.fmtCtx->streams[s.streamIdx]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec) { emit errorOccurred("找不到音频解码器: " + path); goto done; }
            s.decCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(s.decCtx, par);
            if (avcodec_open2(s.decCtx, codec, nullptr) < 0) {
                emit errorOccurred("音频解码器打开失败: " + path); goto done;
            }
            qInfo() << "AudioMixWorker: 区间[" << i << "] opened," << path
                     << "codec=" << codec->name << "sr=" << s.decCtx->sample_rate;
        }
    }

    // 构建 avfilter 图
    {
        graph = avfilter_graph_alloc();
        AVFilterInOut* gInputs  = nullptr;
        AVFilterInOut* gOutputs = nullptr;

        QString fstr = buildFilterStr();
        qInfo() << "AudioMixWorker filter:" << fstr;

        if (avfilter_graph_parse_ptr(graph, fstr.toUtf8().constData(),
                                      &gInputs, &gOutputs, nullptr) < 0) {
            qWarning() << "AudioMixWorker: avfilter_graph_parse_ptr 失败! 滤镜字符串:";
            qWarning() << fstr;
            emit errorOccurred("avfilter_graph_parse_ptr 失败");
            avfilter_inout_free(&gInputs);
            avfilter_inout_free(&gOutputs);
            goto done;
        }

        const AVFilter* abuf = avfilter_get_by_name("abuffer");
        for (AVFilterInOut* cur = gInputs; cur; cur = cur->next) {
            QString nameStr = QString::fromLatin1(cur->name);
            int idx = -1;
            if (nameStr.startsWith("in"))
                idx = nameStr.mid(2).toInt();
            if (idx < 0 || idx > N) continue;

            AVCodecContext* dec = srcs[idx].decCtx;
            AVRational tb = srcs[idx].fmtCtx->streams[srcs[idx].streamIdx]->time_base;
            char chlayout[64] = {};
            av_channel_layout_describe(&dec->ch_layout, chlayout, sizeof(chlayout));

            char args[256];
            snprintf(args, sizeof(args),
                     "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=%d/%d",
                     dec->sample_rate,
                     av_get_sample_fmt_name(dec->sample_fmt),
                     chlayout,
                     tb.num, tb.den);

            char name[32]; snprintf(name, sizeof(name), "src%d", idx);
            qInfo() << "AudioMixWorker: 创建 abuffersrc[" << idx << "] args=" << args;
            if (avfilter_graph_create_filter(&srcs[idx].bufSrc, abuf, name,
                                              args, nullptr, graph) < 0) {
                qWarning() << "AudioMixWorker: 创建 abuffersrc[" << idx << "] 失败!";
                emit errorOccurred(QString("创建 abuffersrc[%1] 失败").arg(idx));
                avfilter_inout_free(&gInputs);
                avfilter_inout_free(&gOutputs);
                goto done;
            }
            avfilter_link(srcs[idx].bufSrc, 0, cur->filter_ctx, cur->pad_idx);
        }
        avfilter_inout_free(&gInputs);

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
            emit errorOccurred("avfilter_graph_config 失败"); goto done;
        }
        qInfo() << "AudioMixWorker: avfilter_graph 构建成功, filters=" << graph->nb_filters;
    }

    // 创建 AAC 编码器 + 输出文件
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
            qWarning() << "AudioMixWorker: AAC 编码器打开失败!";
            emit errorOccurred("AAC 编码器打开失败"); goto done;
        }
        qInfo() << "AudioMixWorker: AAC 编码器配置: sr=" << outRate
                 << "bitrate=" << aEncCtx->bit_rate
                 << "sample_fmt=" << av_get_sample_fmt_name(aEncCtx->sample_fmt)
                 << "frame_size=" << aEncCtx->frame_size;

        if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                            tempAacPath.toUtf8().constData()) < 0) {
            qWarning() << "AudioMixWorker: 无法创建临时输出上下文:" << tempAacPath;
            emit errorOccurred("无法创建临时输出上下文"); goto done;
        }
        qInfo() << "AudioMixWorker: 临时输出格式=" << outCtx->oformat->name
                 << "文件=" << tempAacPath;
        outStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_from_context(outStream->codecpar, aEncCtx);
        outStream->time_base = aEncCtx->time_base;

        if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&outCtx->pb, tempAacPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
                emit errorOccurred("无法写入临时文件"); goto done;
            }
        }
        if (avformat_write_header(outCtx, nullptr) < 0) {
            qWarning() << "AudioMixWorker: 写入临时文件头失败!";
            emit errorOccurred("写入临时文件头失败"); goto done;
        }
        qInfo() << "AudioMixWorker: 临时文件头写入成功";
    }

    // 解码 + 滤镜 + 编码主循环
    {
        pkt    = av_packet_alloc();
        frame  = av_frame_alloc();
        filt   = av_frame_alloc();
        encPkt = av_packet_alloc();

        int64_t totalUs = srcs[0].fmtCtx->duration;
        int64_t filterPts = 0;
        int64_t lastProgressUs = 0;
        int activeSrcs = N + 1;
        int64_t totalEncodedFrames = 0;
        qInfo() << "AudioMixWorker: 开始主循环, totalUs=" << totalUs << "activeSrcs=" << activeSrcs;

        while (activeSrcs > 0) {
            if (isInterruptionRequested()) goto done;
            for (int i = 0; i <= N; ++i) {
                Src& s = srcs[i];
                if (s.eof) continue;

                bool fed = false;
                while (!fed) {
                    int ret = av_read_frame(s.fmtCtx, pkt);
                    if (ret < 0) {
                        // EOF: flush decoder then signal filter
                        qInfo() << "AudioMixWorker: 源[" << i << "] 到达EOF, flushing decoder";
                        avcodec_send_packet(s.decCtx, nullptr);
                        while (avcodec_receive_frame(s.decCtx, frame) >= 0) {
                            av_buffersrc_add_frame_flags(s.bufSrc, frame,
                                                          AV_BUFFERSRC_FLAG_KEEP_REF);
                            av_frame_unref(frame);
                        }
                        av_buffersrc_add_frame_flags(s.bufSrc, nullptr,
                                                      AV_BUFFERSRC_FLAG_PUSH);
                        s.eof = true;
                        --activeSrcs;
                        qInfo() << "AudioMixWorker: 源[" << i << "] EOF 处理完成, activeSrcs=" << activeSrcs;
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
                filt->pts  = filterPts;
                filterPts += filt->nb_samples;
                ++totalEncodedFrames;
                avcodec_send_frame(aEncCtx, filt);
                while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                    av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
                    encPkt->stream_index = outStream->index;
                    encPkt->pos = -1;
                    int ret = av_write_frame(outCtx, encPkt);
                    if (ret < 0) {
                        char errBuf[128] = {};
                        av_strerror(ret, errBuf, sizeof(errBuf));
                        qWarning() << "AudioMixWorker: 主循环写入失败 ret=" << ret << errBuf;
                    }
                    av_packet_unref(encPkt);
                }
                if (totalUs > 0) {
                    int64_t us = av_rescale_q(filterPts, aEncCtx->time_base, AV_TIME_BASE_Q);
                    if (us - lastProgressUs > 500000) {
                        lastProgressUs = us;
                        emit progressed((int)(us * 50 / totalUs));  // 0–50%
                    }
                }
                av_frame_unref(filt);
            }
        }

        qInfo() << "AudioMixWorker: 主循环结束, totalEncodedFrames=" << totalEncodedFrames;
        if (totalEncodedFrames == 0) {
            qWarning() << "AudioMixWorker: 警告! 主循环编码了0帧，滤镜图可能没有输出!";
        }

        // 继续排空 sink
        qInfo() << "AudioMixWorker: drain sink...";
        int drainCount = drainAndEncode(bufSink, aEncCtx, outCtx, outStream, filt, encPkt, filterPts);
        qInfo() << "AudioMixWorker: drain 编码帧数=" << drainCount;

        // 冲刷编码器
        qInfo() << "AudioMixWorker: flush 编码器...";
        int flushCount = 0;
        avcodec_send_frame(aEncCtx, nullptr);
        while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, aEncCtx->time_base, outStream->time_base);
            encPkt->stream_index = outStream->index;
            encPkt->pos = -1;
            int ret = av_write_frame(outCtx, encPkt);
            if (ret < 0) {
                char errBuf[128] = {};
                av_strerror(ret, errBuf, sizeof(errBuf));
                qWarning() << "AudioMixWorker: flush 写入失败 ret=" << ret << errBuf;
            }
            av_packet_unref(encPkt);
            ++flushCount;
        }
        qInfo() << "AudioMixWorker: flush 编码帧数=" << flushCount;

        qInfo() << "AudioMixWorker: filterPts 最终值=" << filterPts
                 << "(" << (filterPts > 0 ? (double)filterPts / aEncCtx->sample_rate : 0) << "秒)";
        qInfo() << "AudioMixWorker: 写入临时 AAC trailer...";
        av_write_trailer(outCtx);
    }

    qInfo() << "=== AudioMixWorker::execMixAudio 完成 ===:" << tempAacPath;
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
    for (auto& s : srcs) {
        if (s.decCtx) avcodec_free_context(&s.decCtx);
        if (s.fmtCtx) avformat_close_input(&s.fmtCtx);
    }
    return ok;
}

// 步骤2：复制源视频流 + 新 AAC 音频流 → 输出 MP4
bool AudioMixWorker::execMuxFinal(const QString& tempAacPath)
{
    qInfo() << "=== AudioMixWorker::execMuxFinal 开始 ===";
    qInfo() << "  临时音频=" << tempAacPath;
    qInfo() << "  输出=" << task_.outputPath;

    AVFormatContext* videoCtx  = nullptr;
    AVFormatContext* audioCtx  = nullptr;
    AVFormatContext* outCtx    = nullptr;
    AVPacket*        pkt       = nullptr;
    bool ok = false;

    // 打开源视频（取视频流）
    if (avformat_open_input(&videoCtx, task_.originalVideoPath.toUtf8().constData(),
                             nullptr, nullptr) < 0) {
        emit errorOccurred("封装：无法打开源视频"); goto done;
    }
    avformat_find_stream_info(videoCtx, nullptr);
    int videoStreamIdx = av_find_best_stream(videoCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx < 0) { emit errorOccurred("封装：源视频无视频流"); goto done; }
    qInfo() << "AudioMixWorker: 源视频流 idx=" << videoStreamIdx;

    // 打开临时 AAC
    if (avformat_open_input(&audioCtx, tempAacPath.toUtf8().constData(),
                             nullptr, nullptr) < 0) {
        emit errorOccurred("封装：无法打开临时音频"); goto done;
    }
    avformat_find_stream_info(audioCtx, nullptr);
    int audioStreamIdx = av_find_best_stream(audioCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx < 0) {
        qWarning() << "AudioMixWorker::execMuxFinal 错误: 临时音频文件中找不到音频流!";
        emit errorOccurred("封装：临时音频无音频流"); goto done;
    }
    qInfo() << "AudioMixWorker: 临时音频流 idx=" << audioStreamIdx
             << "codec=" << audioCtx->streams[audioStreamIdx]->codecpar->codec_id
             << "duration=" << audioCtx->duration;

    // 创建输出
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        task_.outputPath.toUtf8().constData()) < 0) {
        qWarning() << "AudioMixWorker::execMuxFinal 错误: 无法创建输出上下文:" << task_.outputPath;
        emit errorOccurred("封装：无法创建输出上下文"); goto done;
    }
    qInfo() << "AudioMixWorker: 输出格式=" << outCtx->oformat->name
             << "文件=" << task_.outputPath;

    {
        // 添加视频流
        AVStream* ovs = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_copy(ovs->codecpar,
                                videoCtx->streams[videoStreamIdx]->codecpar);
        ovs->codecpar->codec_tag = 0;
        ovs->time_base = videoCtx->streams[videoStreamIdx]->time_base;

        // 添加音频流
        AVStream* oas = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_copy(oas->codecpar,
                                audioCtx->streams[audioStreamIdx]->codecpar);
        oas->codecpar->codec_tag = 0;
        oas->time_base = audioCtx->streams[audioStreamIdx]->time_base;

        qInfo() << "AudioMixWorker: 输出流已创建: nb_streams=" << outCtx->nb_streams
                 << "视频codec=" << ovs->codecpar->codec_id
                 << "音频codec=" << oas->codecpar->codec_id;

        if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&outCtx->pb, task_.outputPath.toUtf8().constData(),
                          AVIO_FLAG_WRITE) < 0) {
                emit errorOccurred("封装：无法写入输出文件"); goto done;
            }
        }
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags", "faststart", 0);
        if (avformat_write_header(outCtx, &opts) < 0) {
            av_dict_free(&opts);
            qWarning() << "AudioMixWorker::execMuxFinal 错误: 写入文件头失败!";
            emit errorOccurred("封装：写入文件头失败"); goto done;
        }
        av_dict_free(&opts);
        qInfo() << "AudioMixWorker: 输出文件头写入成功";

        pkt = av_packet_alloc();
        AVRational srcVideoTB = videoCtx->streams[videoStreamIdx]->time_base;
        AVRational srcAudioTB = audioCtx->streams[audioStreamIdx]->time_base;

        qInfo() << "AudioMixWorker: 开始双源交替 mux 循环...";
        qInfo() << "  视频流 time_base=" << srcVideoTB.num << "/" << srcVideoTB.den;
        qInfo() << "  音频流 time_base=" << srcAudioTB.num << "/" << srcAudioTB.den;
        qInfo() << "  输出视频流 time_base=" << ovs->time_base.num << "/" << ovs->time_base.den;
        qInfo() << "  输出音频流 time_base=" << oas->time_base.num << "/" << oas->time_base.den;
        // 双源交替读取：按 DTS 顺序混合写入，保证长视频音画交错正确
        bool videoDone = false, audioDone = false;
        AVPacket* videoPkt = av_packet_alloc();
        AVPacket* audioPkt = av_packet_alloc();
        bool hasVideoPkt = false, hasAudioPkt = false;
        int64_t videoPktCount = 0, audioPktCount = 0;
        bool firstVideoPkt = true, firstAudioPkt = true;

        while (!videoDone || !audioDone) {
            if (isInterruptionRequested()) {
                av_packet_free(&videoPkt);
                av_packet_free(&audioPkt);
                goto done;
            }
            // 读取下一个视频包（若尚未缓存）
            if (!hasVideoPkt && !videoDone) {
                while (av_read_frame(videoCtx, videoPkt) >= 0) {
                    if (videoPkt->stream_index == videoStreamIdx) {
                        hasVideoPkt = true;
                        break;
                    }
                    av_packet_unref(videoPkt);
                }
                if (!hasVideoPkt) videoDone = true;
            }
            // 读取下一个音频包（若尚未缓存）
            if (!hasAudioPkt && !audioDone) {
                while (av_read_frame(audioCtx, audioPkt) >= 0) {
                    if (audioPkt->stream_index == audioStreamIdx) {
                        hasAudioPkt = true;
                        break;
                    }
                    av_packet_unref(audioPkt);
                }
                if (!hasAudioPkt) audioDone = true;
            }

            // 比较 DTS，优先写入时间戳更小的包
            bool writeVideo = false;
            if (hasVideoPkt && hasAudioPkt) {
                // 统一到输出 time_base 比较
                int64_t vDts = av_rescale_q(videoPkt->dts, srcVideoTB, ovs->time_base);
                int64_t aDts = av_rescale_q(audioPkt->dts, srcAudioTB, oas->time_base);
                writeVideo = (vDts <= aDts);
            } else if (hasVideoPkt) {
                writeVideo = true;
            }

            if (writeVideo) {
                int64_t origDts = videoPkt->dts;
                int64_t origPts = videoPkt->pts;
                av_packet_rescale_ts(videoPkt, srcVideoTB, ovs->time_base);
                videoPkt->stream_index = ovs->index;
                if (firstVideoPkt) {
                    qInfo() << "AudioMixWorker: 第一个视频包 原始dts=" << origDts << "pts=" << origPts
                             << " rescale后 dts=" << videoPkt->dts << "pts=" << videoPkt->pts;
                    firstVideoPkt = false;
                }
                int ret = av_interleaved_write_frame(outCtx, videoPkt);
                if (ret < 0) {
                    char errBuf[128] = {};
                    av_strerror(ret, errBuf, sizeof(errBuf));
                    qWarning() << "AudioMixWorker: 视频包写入失败 ret=" << ret << errBuf
                             << "dts=" << videoPkt->dts << "pts=" << videoPkt->pts;
                }
                av_packet_unref(videoPkt);
                hasVideoPkt = false;
                ++videoPktCount;
            } else if (hasAudioPkt) {
                int64_t origDts = audioPkt->dts;
                int64_t origPts = audioPkt->pts;
                av_packet_rescale_ts(audioPkt, srcAudioTB, oas->time_base);
                audioPkt->stream_index = oas->index;
                if (firstAudioPkt) {
                    qInfo() << "AudioMixWorker: 第一个音频包 原始dts=" << origDts << "pts=" << origPts
                             << " rescale后 dts=" << audioPkt->dts << "pts=" << audioPkt->pts;
                    firstAudioPkt = false;
                }
                int ret = av_interleaved_write_frame(outCtx, audioPkt);
                if (ret < 0) {
                    char errBuf[128] = {};
                    av_strerror(ret, errBuf, sizeof(errBuf));
                    qWarning() << "AudioMixWorker: 音频包写入失败 ret=" << ret << errBuf
                             << "dts=" << audioPkt->dts << "pts=" << audioPkt->pts;
                }
                av_packet_unref(audioPkt);
                hasAudioPkt = false;
                ++audioPktCount;
            }
        }
        av_packet_free(&videoPkt);
        av_packet_free(&audioPkt);
        qInfo() << "AudioMixWorker: mux 循环统计: 视频包=" << videoPktCount
                 << "音频包=" << audioPktCount;
        if (audioPktCount == 0) {
            qWarning() << "AudioMixWorker: 警告! 写入了0个音频包，输出视频将没有音频!";
        }
        emit progressed(90);

        qInfo() << "AudioMixWorker: mux 循环结束, 写入 trailer...";
        av_write_trailer(outCtx);
    }

    qInfo() << "=== AudioMixWorker::execMuxFinal 完成 ===:" << task_.outputPath;
    ok = true;

done:
    av_packet_free(&pkt);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }
    if (audioCtx) avformat_close_input(&audioCtx);
    if (videoCtx) avformat_close_input(&videoCtx);
    return ok;
}

void AudioMixWorker::run()
{
    qInfo() << "=== AudioMixWorker::run 开始 ===";
    qInfo() << "  源视频:" << task_.originalVideoPath;
    qInfo() << "  输出路径:" << task_.outputPath;
    qInfo() << "  区间数:" << task_.regions.size();

    if (task_.regions.isEmpty()) {
        qWarning() << "AudioMixWorker::run 错误: 区间列表为空";
        emit errorOccurred("没有音频区间，请先添加音频");
        emit finished(false);
        return;
    }

    emit progressed(0);

    QString tempAac = QDir::temp().filePath(
        QString("rambos_audiomix_%1.aac").arg(QDateTime::currentMSecsSinceEpoch()));
    qInfo() << "  临时AAC文件:" << tempAac;

    // === 步骤1: 混合音频 ===
    qInfo() << "--- 步骤1: execMixAudio 开始 ---";
    if (!execMixAudio(tempAac)) {
        qWarning() << "AudioMixWorker::run 步骤1失败: execMixAudio 返回 false";
        QFile::remove(tempAac);
        emit finished(false);
        return;
    }
    // 验证临时文件是否生成且非空
    {
        QFile tmpFile(tempAac);
        if (!tmpFile.exists()) {
            qWarning() << "AudioMixWorker::run 错误: 临时AAC文件不存在:" << tempAac;
            emit errorOccurred("临时AAC文件未生成: " + tempAac);
            emit finished(false);
            return;
        }
        qInfo() << "  临时AAC文件大小:" << tmpFile.size() << "bytes";
        if (tmpFile.size() == 0) {
            qWarning() << "AudioMixWorker::run 错误: 临时AAC文件为空";
            emit errorOccurred("临时AAC文件为空（混音可能失败）");
            QFile::remove(tempAac);
            emit finished(false);
            return;
        }
    }
    qInfo() << "--- 步骤1: execMixAudio 完成 ---";

    emit progressed(60);

    // === 步骤2: 封装最终视频 ===
    qInfo() << "--- 步骤2: execMuxFinal 开始 ---";
    if (!execMuxFinal(tempAac)) {
        qWarning() << "AudioMixWorker::run 步骤2失败: execMuxFinal 返回 false";
        QFile::remove(tempAac);
        emit finished(false);
        return;
    }
    qInfo() << "--- 步骤2: execMuxFinal 完成 ---";

    // 验证最终输出文件
    {
        QFile outFile(task_.outputPath);
        if (outFile.exists()) {
            qInfo() << "  最终输出文件大小:" << outFile.size() << "bytes";
        } else {
            qWarning() << "AudioMixWorker::run 警告: 输出文件不存在:" << task_.outputPath;
        }
    }

    QFile::remove(tempAac);
    qInfo() << "=== AudioMixWorker::run 全部完成 ===";
    emit progressed(100);
    emit finished(true);
}
