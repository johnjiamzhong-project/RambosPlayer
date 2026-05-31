#include "concatfilter.h"
#include "logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

ConcatFilter::ConcatFilter(QObject* parent) : QObject(parent) {}

// 初始化 H.264 编码器，优先 nvenc，回退 libx264
static AVCodecContext* openVideoEncoder(int w, int h, AVRational fps)
{
    const char* names[] = {"h264_nvenc", "libx264", nullptr};
    const AVCodec* codec = nullptr;
    bool hw = false;
    for (int i = 0; names[i]; ++i) {
        codec = avcodec_find_encoder_by_name(names[i]);
        if (codec) { hw = (i == 0); break; }
    }
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return nullptr;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width      = w;
    ctx->height     = h;
    ctx->time_base  = AV_TIME_BASE_Q;
    ctx->framerate  = fps;
    ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    ctx->gop_size   = (fps.den > 0) ? 2 * fps.num / fps.den : 50;
    ctx->max_b_frames = 0;
    ctx->flags     |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (hw) {
        av_opt_set(ctx->priv_data, "preset", "p1",  0);
        av_opt_set(ctx->priv_data, "rc",     "vbr", 0);
        av_opt_set(ctx->priv_data, "cq",     "18",  0);
    } else {
        av_opt_set(ctx->priv_data, "preset",  "superfast", 0);
        av_opt_set(ctx->priv_data, "crf",     "17",        0);
        av_opt_set(ctx->priv_data, "profile", "high",      0);
        av_opt_set(ctx->priv_data, "threads", "auto",      0);
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    qInfo() << "[ConcatFilter] 视频编码器:" << codec->name
            << "hw=" << hw
            << "resolution=" << w << "x" << h
            << "fps=" << fps.num << "/" << fps.den
            << "time_base=" << ctx->time_base.num << "/" << ctx->time_base.den;
    return ctx;
}

bool ConcatFilter::exec(const QStringList& inputs, const QString& output)
{
    if (inputs.isEmpty()) { emit errorOccurred("输入文件列表为空"); return false; }

    qInfo() << "[ConcatFilter] === 开始重编码拼接 ===";
    qInfo() << "[ConcatFilter] 输入文件数:" << inputs.size();
    qInfo() << "[ConcatFilter] 输出文件:" << output;

    // 从第一个文件获取目标分辨率和帧率
    int targetW = 0, targetH = 0;
    AVRational targetFps = {30, 1};
    int targetSampleRate = 44100;
    AVChannelLayout targetCh = AV_CHANNEL_LAYOUT_STEREO;

    {
        AVFormatContext* probe = nullptr;
        if (avformat_open_input(&probe, inputs[0].toUtf8().constData(), nullptr, nullptr) < 0) {
            emit errorOccurred("无法打开第一个输入文件"); return false;
        }
        avformat_find_stream_info(probe, nullptr);
        int vi = av_find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int ai = av_find_best_stream(probe, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (vi >= 0) {
            targetW   = probe->streams[vi]->codecpar->width;
            targetH   = probe->streams[vi]->codecpar->height;
            targetFps = av_guess_frame_rate(probe, probe->streams[vi], nullptr);
            if (targetFps.num <= 0 || targetFps.den <= 0) targetFps = {30, 1};
            qInfo() << "[ConcatFilter] 目标参数(来自首个文件):"
                    << "resolution=" << targetW << "x" << targetH
                    << "fps=" << targetFps.num << "/" << targetFps.den
                    << "pix_fmt=" << probe->streams[vi]->codecpar->format;
        }
        if (ai >= 0) {
            targetSampleRate = probe->streams[ai]->codecpar->sample_rate;
            av_channel_layout_copy(&targetCh, &probe->streams[ai]->codecpar->ch_layout);
            qInfo() << "[ConcatFilter] 目标音频参数:"
                    << "sample_rate=" << targetSampleRate
                    << "channels=" << targetCh.nb_channels;
        }
        avformat_close_input(&probe);
    }

    if (targetW <= 0 || targetH <= 0) { emit errorOccurred("无法获取目标分辨率"); return false; }

    // ===== 创建编码器 =====
    AVCodecContext* vEncCtx = openVideoEncoder(targetW, targetH, targetFps);
    if (!vEncCtx) { emit errorOccurred("无可用 H.264 编码器"); return false; }

    const AVCodec* aacCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* aEncCtx = nullptr;
    if (aacCodec) {
        aEncCtx = avcodec_alloc_context3(aacCodec);
        aEncCtx->sample_rate = targetSampleRate;
        av_channel_layout_copy(&aEncCtx->ch_layout, &targetCh);
        aEncCtx->sample_fmt  = aacCodec->sample_fmts[0];
        aEncCtx->bit_rate    = 192000;
        aEncCtx->time_base   = {1, targetSampleRate};
        aEncCtx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aEncCtx, aacCodec, nullptr) < 0) {
            avcodec_free_context(&aEncCtx);
            aEncCtx = nullptr;
        }
        qInfo() << "[ConcatFilter] 音频编码器: AAC"
                << "sample_rate=" << aEncCtx->sample_rate
                << "bit_rate=" << aEncCtx->bit_rate
                << "time_base=" << aEncCtx->time_base.num << "/" << aEncCtx->time_base.den;
    }
    av_channel_layout_uninit(&targetCh);

    // ===== 创建输出文件 =====
    AVFormatContext* outCtx = nullptr;
    AVStream* vOutStream = nullptr;
    AVStream* aOutStream = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        output.toUtf8().constData()) < 0) {
        avcodec_free_context(&vEncCtx);
        avcodec_free_context(&aEncCtx);
        emit errorOccurred("无法创建输出上下文");
        return false;
    }
    vOutStream = avformat_new_stream(outCtx, nullptr);
    avcodec_parameters_from_context(vOutStream->codecpar, vEncCtx);
    vOutStream->time_base = vEncCtx->time_base;
    qInfo() << "[ConcatFilter] 输出视频流 time_base=" << vOutStream->time_base.num << "/" << vOutStream->time_base.den;
    if (aEncCtx) {
        aOutStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_from_context(aOutStream->codecpar, aEncCtx);
        aOutStream->time_base = aEncCtx->time_base;
        qInfo() << "[ConcatFilter] 输出音频流 time_base=" << aOutStream->time_base.num << "/" << aOutStream->time_base.den;
    }
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, output.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&vEncCtx);
            avcodec_free_context(&aEncCtx);
            avformat_free_context(outCtx);
            emit errorOccurred("无法写入输出文件: " + output);
            return false;
        }
    }
    avformat_write_header(outCtx, nullptr);

    // ===== 估算总时长（用于进度）=====
    int64_t totalUs = 0;
    int64_t totalDurFromFiles = 0;
    qInfo() << "[ConcatFilter] === 逐个文件探测时长 ===";
    for (int i = 0; i < inputs.size(); ++i) {
        AVFormatContext* tmp = nullptr;
        if (avformat_open_input(&tmp, inputs[i].toUtf8().constData(), nullptr, nullptr) >= 0) {
            avformat_find_stream_info(tmp, nullptr);
            int64_t fileUs = av_rescale_q(tmp->duration, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
            totalUs += fileUs;
            totalDurFromFiles += tmp->duration;
            qInfo() << "  文件[" << i << "]:"
                    << inputs[i]
                    << "container_duration(us)=" << fileUs
                    << "container_duration(s)=" << (fileUs / 1000000.0)
                    << "nb_streams=" << tmp->nb_streams;
            for (unsigned si = 0; si < tmp->nb_streams; ++si) {
                AVStream* st = tmp->streams[si];
                int64_t sdurUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
                qInfo() << "    流#" << si
                        << "type=" << av_get_media_type_string(st->codecpar->codec_type)
                        << "duration(us)=" << sdurUs
                        << "duration(s)=" << (sdurUs / 1000000.0)
                        << "time_base=" << st->time_base.num << "/" << st->time_base.den;
            }
            avformat_close_input(&tmp);
        }
    }
    qInfo() << "[ConcatFilter] 累计预估输出总时长(us)=" << totalUs
            << " S=" << (totalUs / 1000000.0) << "s";

    // ===== 逐文件顺序解码并编码 =====
    int64_t videoPtsAccum = 0;  // 视频 PTS 续接基点（AV_TIME_BASE 单位）
    int64_t audioPtsAccum = 0;  // 音频 PTS 续接基点（采样数单位）
    int64_t processedUs   = 0;
    int64_t totalEncodedVideoFrames = 0;
    int64_t totalEncodedVideoPackets = 0;
    int64_t totalEncodedAudioFrames = 0;
    int64_t totalEncodedAudioPackets = 0;
    bool ok = true;

    AVPacket* inPkt   = av_packet_alloc();
    AVFrame*  vFrame  = av_frame_alloc();
    AVFrame*  aFrame  = av_frame_alloc();
    AVFrame*  swsFrame = av_frame_alloc();
    AVPacket* encPkt  = av_packet_alloc();

    // 目标分辨率帧缓冲（复用）
    swsFrame->format = AV_PIX_FMT_YUV420P;
    swsFrame->width  = targetW;
    swsFrame->height = targetH;
    av_frame_get_buffer(swsFrame, 0);

    for (int fi = 0; fi < inputs.size() && ok; ++fi) {
        AVFormatContext* inCtx  = nullptr;
        AVCodecContext*  vDecCtx = nullptr;
        AVCodecContext*  aDecCtx = nullptr;
        SwsContext*      swsCtx  = nullptr;
        SwrContext*      swrCtx  = nullptr;
        AVFrame*         swrFrame = nullptr;

        qInfo() << "[ConcatFilter] --- 处理文件[" << fi << "]:"
                << inputs[fi] << " ---";
        qInfo() << "  videoPtsAccum(before)=" << videoPtsAccum
                << "=" << (videoPtsAccum / 1000000.0) << "s";
        qInfo() << "  audioPtsAccum(before)=" << audioPtsAccum
                << "=" << (audioPtsAccum * 1.0 / aEncCtx->sample_rate) << "s";
        qInfo() << "  processedUs(before)=" << processedUs;

        // 打开输入文件
        if (avformat_open_input(&inCtx, inputs[fi].toUtf8().constData(),
                                 nullptr, nullptr) < 0) {
            emit errorOccurred("无法打开输入: " + inputs[fi]);
            ok = false; goto next_file;
        }
        avformat_find_stream_info(inCtx, nullptr);

        {
            int vi = av_find_best_stream(inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            int ai = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

            if (vi < 0) { emit errorOccurred("未找到视频流: " + inputs[fi]); ok = false; goto next_file; }

            // 视频解码器
            {
                const AVCodec* codec = avcodec_find_decoder(inCtx->streams[vi]->codecpar->codec_id);
                if (!codec) { emit errorOccurred("找不到视频解码器"); ok = false; goto next_file; }
                vDecCtx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(vDecCtx, inCtx->streams[vi]->codecpar);
                vDecCtx->thread_count = 0;
                if (avcodec_open2(vDecCtx, codec, nullptr) < 0) {
                    emit errorOccurred("视频解码器打开失败"); ok = false; goto next_file;
                }
                qInfo() << "  视频流#"<< vi << ": codec=" << codec->name
                        << "pix_fmt=" << vDecCtx->pix_fmt
                        << "resolution=" << vDecCtx->width << "x" << vDecCtx->height
                        << "time_base=" << inCtx->streams[vi]->time_base.num
                        << "/" << inCtx->streams[vi]->time_base.den;
            }

            // SwsContext（来源像素格式 → YUV420P，同时缩放到目标分辨率）
            swsCtx = sws_getContext(vDecCtx->width, vDecCtx->height, vDecCtx->pix_fmt,
                                     targetW, targetH, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

            // 音频解码器
            if (ai >= 0 && aEncCtx) {
                const AVCodec* codec = avcodec_find_decoder(inCtx->streams[ai]->codecpar->codec_id);
                if (codec) {
                    aDecCtx = avcodec_alloc_context3(codec);
                    avcodec_parameters_to_context(aDecCtx, inCtx->streams[ai]->codecpar);
                    if (avcodec_open2(aDecCtx, codec, nullptr) < 0) {
                        avcodec_free_context(&aDecCtx); aDecCtx = nullptr;
                    } else {
                        // SwrContext：输入音频 → 目标采样率/格式
                        swrCtx = swr_alloc();
                        av_opt_set_chlayout(swrCtx,    "in_chlayout",    &aDecCtx->ch_layout, 0);
                        av_opt_set_int(swrCtx,         "in_sample_rate", aDecCtx->sample_rate, 0);
                        av_opt_set_sample_fmt(swrCtx,  "in_sample_fmt",  aDecCtx->sample_fmt, 0);
                        av_opt_set_chlayout(swrCtx,    "out_chlayout",   &aEncCtx->ch_layout, 0);
                        av_opt_set_int(swrCtx,         "out_sample_rate",aEncCtx->sample_rate, 0);
                        av_opt_set_sample_fmt(swrCtx,  "out_sample_fmt", aEncCtx->sample_fmt, 0);
                        swr_init(swrCtx);

                        swrFrame = av_frame_alloc();
                        qInfo() << "  音频流#"<< ai << ": codec=" << codec->name
                                << "sample_rate=" << aDecCtx->sample_rate
                                << "channels=" << aDecCtx->ch_layout.nb_channels
                                << "time_base=" << inCtx->streams[ai]->time_base.num
                                << "/" << inCtx->streams[ai]->time_base.den;
                    }
                }
            }

            // 记录本文件的视频/音频时长，用于 PTS 续接
            AVRational vTb = inCtx->streams[vi]->time_base;
            int64_t vDurUs = av_rescale_q(inCtx->streams[vi]->duration, vTb, AV_TIME_BASE_Q);
            int64_t aDurSamples = aDecCtx
                ? av_rescale_q(
                    (ai >= 0 ? inCtx->streams[ai]->duration : 0),
                    (ai >= 0 ? inCtx->streams[ai]->time_base : AVRational{1,1}),
                    {1, aEncCtx ? aEncCtx->sample_rate : targetSampleRate})
                : 0;

            qInfo() << "  本文件视频时长(us)=" << vDurUs
                    << "=" << (vDurUs / 1000000.0) << "s";
            qInfo() << "  本文件音频时长(samples)=" << aDurSamples
                    << "=" << (aDurSamples * 1.0 / targetSampleRate) << "s";

            // --- 解码循环 ---
            int64_t fileVideoFrames = 0;
            int64_t fileVideoPackets = 0;
            int64_t fileAudioFrames = 0;
            int64_t fileAudioPackets = 0;
            int64_t firstVFramePtsUs = AV_NOPTS_VALUE;
            int64_t lastVFramePtsUs  = 0;
            while (av_read_frame(inCtx, inPkt) >= 0) {
                if (inPkt->stream_index == vi) {
                    avcodec_send_packet(vDecCtx, inPkt);
                    while (avcodec_receive_frame(vDecCtx, vFrame) >= 0) {
                        ++fileVideoFrames;
                        // 将原始 pts 映射到微秒后加上累积基点
                        int64_t fPtsUs = (vFrame->pts != AV_NOPTS_VALUE)
                            ? av_rescale_q(vFrame->pts, vTb, AV_TIME_BASE_Q) : 0;
                        swsFrame->pts = videoPtsAccum + fPtsUs;

                        if (firstVFramePtsUs == AV_NOPTS_VALUE)
                            firstVFramePtsUs = swsFrame->pts;
                        lastVFramePtsUs = swsFrame->pts;

                        // 缩放到目标分辨率
                        sws_scale(swsCtx, vFrame->data, vFrame->linesize, 0, vDecCtx->height,
                                  swsFrame->data, swsFrame->linesize);

                        avcodec_send_frame(vEncCtx, swsFrame);
                        while (avcodec_receive_packet(vEncCtx, encPkt) >= 0) {
                            av_packet_rescale_ts(encPkt, vEncCtx->time_base, vOutStream->time_base);
                            encPkt->stream_index = vOutStream->index;
                            encPkt->pos = -1;
                            av_write_frame(outCtx, encPkt);
                            av_packet_unref(encPkt);
                            ++fileVideoPackets;
                            ++totalEncodedVideoPackets;
                        }

                        // 进度：基于已处理时间
                        if (totalUs > 0) {
                            int64_t doneUs = processedUs + fPtsUs;
                            emit progressed((int)(doneUs * 100 / totalUs));
                        }
                        av_frame_unref(vFrame);
                    }
                } else if (aDecCtx && aEncCtx && inPkt->stream_index ==
                            av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)) {
                    avcodec_send_packet(aDecCtx, inPkt);
                    while (avcodec_receive_frame(aDecCtx, aFrame) >= 0) {
                        ++fileAudioFrames;
                        // 重采样
                        int outSamples = (int)av_rescale_rnd(
                            swr_get_delay(swrCtx, aEncCtx->sample_rate) + aFrame->nb_samples,
                            aEncCtx->sample_rate, aDecCtx->sample_rate, AV_ROUND_UP);
                        av_frame_unref(swrFrame);
                        swrFrame->nb_samples = outSamples;
                        av_channel_layout_copy(&swrFrame->ch_layout, &aEncCtx->ch_layout);
                        swrFrame->sample_rate = aEncCtx->sample_rate;
                        swrFrame->format      = aEncCtx->sample_fmt;
                        av_frame_get_buffer(swrFrame, 0);
                        swr_convert_frame(swrCtx, swrFrame, aFrame);
                        swrFrame->pts = audioPtsAccum;
                        audioPtsAccum += swrFrame->nb_samples;

                        avcodec_send_frame(aEncCtx, swrFrame);
                        while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                            av_packet_rescale_ts(encPkt, aEncCtx->time_base, aOutStream->time_base);
                            encPkt->stream_index = aOutStream->index;
                            encPkt->pos = -1;
                            av_write_frame(outCtx, encPkt);
                            av_packet_unref(encPkt);
                            ++fileAudioPackets;
                            ++totalEncodedAudioPackets;
                        }
                        av_frame_unref(aFrame);
                    }
                }
                av_packet_unref(inPkt);
            }

            totalEncodedVideoFrames += fileVideoFrames;
            totalEncodedAudioFrames += fileAudioFrames;

            qInfo() << "  文件[" << fi << "] 解码统计:"
                    << "视频帧数=" << fileVideoFrames
                    << "视频编码包数=" << fileVideoPackets
                    << "首帧PTS(us)=" << firstVFramePtsUs
                    << "末帧PTS(us)=" << lastVFramePtsUs
                    << "音频帧数=" << fileAudioFrames
                    << "音频编码包数=" << fileAudioPackets;

            // 更新累积 PTS 基点（当前文件时长）
            int64_t oldVideoAccum = videoPtsAccum;
            videoPtsAccum += vDurUs;
            processedUs   += vDurUs;
            (void)aDurSamples;  // audioPtsAccum 已在帧循环中累积

            qInfo() << "  文件[" << fi << "] 完成:"
                    << "videoPtsAccum: " << oldVideoAccum << "->" << videoPtsAccum
                    << " (delta=" << (videoPtsAccum - oldVideoAccum) << "us)"
                    << "audioPtsAccum=" << audioPtsAccum
                    << " processedUs=" << processedUs;
        }

next_file:
        av_frame_free(&swrFrame);
        if (swrCtx) swr_free(&swrCtx);
        if (aDecCtx) avcodec_free_context(&aDecCtx);
        if (swsCtx) sws_freeContext(swsCtx);
        if (vDecCtx) avcodec_free_context(&vDecCtx);
        if (inCtx) avformat_close_input(&inCtx);
    }

    // ===== Flush 编码器 =====
    if (ok) {
        int64_t flushVidPkts = 0;
        avcodec_send_frame(vEncCtx, nullptr);
        while (avcodec_receive_packet(vEncCtx, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, vEncCtx->time_base, vOutStream->time_base);
            encPkt->stream_index = vOutStream->index;
            encPkt->pos = -1;
            av_write_frame(outCtx, encPkt);
            av_packet_unref(encPkt);
            ++flushVidPkts;
        }
        int64_t flushAudPkts = 0;
        if (aEncCtx) {
            avcodec_send_frame(aEncCtx, nullptr);
            while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                av_packet_rescale_ts(encPkt, aEncCtx->time_base, aOutStream->time_base);
                encPkt->stream_index = aOutStream->index;
                encPkt->pos = -1;
                av_write_frame(outCtx, encPkt);
                av_packet_unref(encPkt);
                ++flushAudPkts;
            }
        }
        qInfo() << "[ConcatFilter] Flush 编码器:"
                << "视频flush包数=" << flushVidPkts
                << "音频flush包数=" << flushAudPkts;

        av_write_trailer(outCtx);

        // ── Log: 输出流实际时长 ──
        qInfo() << "[ConcatFilter] === 输出统计 ===";
        qInfo() << "  编码视频帧总数:" << totalEncodedVideoFrames;
        qInfo() << "  编码视频包总数:" << totalEncodedVideoPackets;
        qInfo() << "  编码音频帧总数:" << totalEncodedAudioFrames;
        qInfo() << "  编码音频包总数:" << totalEncodedAudioPackets;
        qInfo() << "  最终videoPtsAccum(us)=" << videoPtsAccum
                << " =" << (videoPtsAccum / 1000000.0) << "s";
        qInfo() << "  最终audioPtsAccum(samples)=" << audioPtsAccum
                << " =" << (audioPtsAccum * 1.0 / targetSampleRate) << "s";

        qInfo() << "[ConcatFilter] === 写trailer后输出流状态 ===";
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

        qInfo() << "ConcatFilter 完成:" << output;
    }

    av_packet_free(&encPkt);
    av_frame_free(&swsFrame);
    av_frame_free(&aFrame);
    av_frame_free(&vFrame);
    av_packet_free(&inPkt);
    avcodec_free_context(&aEncCtx);
    avcodec_free_context(&vEncCtx);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }

    // ── Log: 输出文件实际时长验证 ──
    if (ok) {
        qInfo() << "[ConcatFilter] === 输出文件验证（重新打开） ===";
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
