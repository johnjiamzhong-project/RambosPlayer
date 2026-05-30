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
    return ctx;
}

bool ConcatFilter::exec(const QStringList& inputs, const QString& output)
{
    if (inputs.isEmpty()) { emit errorOccurred("输入文件列表为空"); return false; }

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
        }
        if (ai >= 0) {
            targetSampleRate = probe->streams[ai]->codecpar->sample_rate;
            av_channel_layout_copy(&targetCh, &probe->streams[ai]->codecpar->ch_layout);
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
    if (aEncCtx) {
        aOutStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_from_context(aOutStream->codecpar, aEncCtx);
        aOutStream->time_base = aEncCtx->time_base;
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
    for (const QString& f : inputs) {
        AVFormatContext* tmp = nullptr;
        if (avformat_open_input(&tmp, f.toUtf8().constData(), nullptr, nullptr) >= 0) {
            avformat_find_stream_info(tmp, nullptr);
            totalUs += av_rescale_q(tmp->duration, AV_TIME_BASE_Q, AV_TIME_BASE_Q);
            avformat_close_input(&tmp);
        }
    }

    // ===== 逐文件顺序解码并编码 =====
    int64_t videoPtsAccum = 0;  // 视频 PTS 续接基点（AV_TIME_BASE 单位）
    int64_t audioPtsAccum = 0;  // 音频 PTS 续接基点（采样数单位）
    int64_t processedUs   = 0;
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

            // --- 解码循环 ---
            while (av_read_frame(inCtx, inPkt) >= 0) {
                if (inPkt->stream_index == vi) {
                    avcodec_send_packet(vDecCtx, inPkt);
                    while (avcodec_receive_frame(vDecCtx, vFrame) >= 0) {
                        // 将原始 pts 映射到微秒后加上累积基点
                        int64_t fPtsUs = (vFrame->pts != AV_NOPTS_VALUE)
                            ? av_rescale_q(vFrame->pts, vTb, AV_TIME_BASE_Q) : 0;
                        swsFrame->pts = videoPtsAccum + fPtsUs;

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
                        }
                        av_frame_unref(aFrame);
                    }
                }
                av_packet_unref(inPkt);
            }

            // 更新累积 PTS 基点（当前文件时长）
            videoPtsAccum += vDurUs;
            processedUs   += vDurUs;
            (void)aDurSamples;  // audioPtsAccum 已在帧循环中累积
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
        avcodec_send_frame(vEncCtx, nullptr);
        while (avcodec_receive_packet(vEncCtx, encPkt) >= 0) {
            av_packet_rescale_ts(encPkt, vEncCtx->time_base, vOutStream->time_base);
            encPkt->stream_index = vOutStream->index;
            encPkt->pos = -1;
            av_write_frame(outCtx, encPkt);
            av_packet_unref(encPkt);
        }
        if (aEncCtx) {
            avcodec_send_frame(aEncCtx, nullptr);
            while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                av_packet_rescale_ts(encPkt, aEncCtx->time_base, aOutStream->time_base);
                encPkt->stream_index = aOutStream->index;
                encPkt->pos = -1;
                av_write_frame(outCtx, encPkt);
                av_packet_unref(encPkt);
            }
        }
        av_write_trailer(outCtx);
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

    return ok;
}
