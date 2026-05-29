#include "exportworker.h"
#include "logger.h"
#include <QFileInfo>
#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

ExportWorker::ExportWorker(QObject* parent)
    : QThread(parent), inPts_(0), outPts_(0)
{
}

ExportWorker::~ExportWorker()
{
    if (isRunning()) {
        requestInterruption();
        wait(5000);
    }
}

void ExportWorker::run(const QString& inputPath, const QString& outputPath,
                        int64_t inPts, int64_t outPts)
{
    if (isRunning()) {
        emit errorOccurred("上一次导出尚未完成");
        return;
    }
    isBatch_     = false;
    inputPath_   = inputPath;
    outputPath_  = outputPath;
    inPts_       = inPts;
    outPts_      = outPts;
    batchSegments_.clear();
    start();
}

void ExportWorker::runBatch(const QString& inputPath, const QList<ExportSegment>& segments)
{
    if (isRunning()) {
        emit errorOccurred("上一次导出尚未完成");
        return;
    }
    if (segments.isEmpty()) {
        emit errorOccurred("批量导出列表为空");
        return;
    }
    isBatch_     = true;
    inputPath_   = inputPath;
    batchSegments_ = segments;
    start();
}

void ExportWorker::run()
{
    // ====================================================================
    // 会话层：打开输入文件 + 创建解码器（批量模式复用）
    // ====================================================================
    AVFormatContext* inCtx = nullptr;
    AVCodecContext* vDecCtx = nullptr;
    AVCodecContext* aDecCtx = nullptr;
    SwsContext*     swsCtx  = nullptr;
    AVFrame*        swsFrame = nullptr;
    int srcWidth = 0, srcHeight = 0;
    int videoStreamIdx = -1, audioStreamIdx = -1;
    AVRational videoTimeBase = {1, 1}, audioTimeBase = {1, 1};
    AVRational frameRate = {30, 1};

    // --- 打开输入文件 ---
    if (avformat_open_input(&inCtx, inputPath_.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit errorOccurred("无法打开输入文件: " + inputPath_);
        emit exportFinished(false);
        return;
    }
    inCtx->probesize = 1 << 20;
    inCtx->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(inCtx, nullptr) < 0) {
        emit errorOccurred("无法获取输入流信息");
        avformat_close_input(&inCtx);
        emit exportFinished(false);
        return;
    }

    // --- 视频解码器 ---
    videoStreamIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx < 0) {
        emit errorOccurred("输入文件中未找到视频流");
        avformat_close_input(&inCtx);
        emit exportFinished(false);
        return;
    }
    {
        AVCodecParameters* par = inCtx->streams[videoStreamIdx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) { emit errorOccurred("找不到视频解码器"); goto session_cleanup; }
        vDecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(vDecCtx, par);
        vDecCtx->thread_count = 0;
        if (avcodec_open2(vDecCtx, codec, nullptr) < 0) {
            emit errorOccurred("视频解码器打开失败");
            avcodec_free_context(&vDecCtx);
            goto session_cleanup;
        }
        srcWidth  = vDecCtx->width;
        srcHeight = vDecCtx->height;
        videoTimeBase = inCtx->streams[videoStreamIdx]->time_base;
        frameRate = av_guess_frame_rate(inCtx, inCtx->streams[videoStreamIdx], nullptr);
        if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = {30, 1};
        qInfo() << "视频:" << srcWidth << "x" << srcHeight
                << "fps:" << frameRate.num << "/" << frameRate.den;
    }

    // --- 音频解码器（非必需）---
    audioStreamIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx >= 0) {
        AVCodecParameters* par = inCtx->streams[audioStreamIdx]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (codec) {
            aDecCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(aDecCtx, par);
            if (avcodec_open2(aDecCtx, codec, nullptr) < 0) {
                avcodec_free_context(&aDecCtx);
                aDecCtx = nullptr;
            } else {
                audioTimeBase = inCtx->streams[audioStreamIdx]->time_base;
                qInfo() << "音频:" << aDecCtx->sample_rate << "Hz"
                        << "ch:" << aDecCtx->ch_layout.nb_channels;
            }
        }
    }

    // --- 硬件解码（CUDA/NVDEC）---
    AVBufferRef* hwDeviceCtx = nullptr;
    if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                                nullptr, nullptr, 0) >= 0) {
        vDecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        qInfo() << "硬件解码: CUDA/NVDEC";
    } else {
        qInfo() << "CUDA 不可用，使用 CPU 解码";
    }

    // --- 像素格式转换器（会话级复用）---
    swsCtx = sws_getContext(srcWidth, srcHeight, vDecCtx->pix_fmt,
                            srcWidth, srcHeight, AV_PIX_FMT_YUV420P,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    swsFrame = av_frame_alloc();
    swsFrame->format = AV_PIX_FMT_YUV420P;
    swsFrame->width  = srcWidth;
    swsFrame->height = srcHeight;
    av_frame_get_buffer(swsFrame, 0);

    // ====================================================================
    // 片段循环：每个片段独立创建输出 + 编码器 → seek → 编码 → 关闭
    // ====================================================================
    int totalSegs = isBatch_ ? batchSegments_.size() : 1;
    int64_t totalVideoFrames = 0;
    bool allOk = true;

    for (int segIdx = 0; segIdx < totalSegs; segIdx++) {
        QString outPath;
        int64_t inPts, outPts;

        if (isBatch_) {
            auto& seg = batchSegments_[segIdx];
            outPath = seg.outputPath;
            inPts   = seg.inPts;
            outPts  = seg.outPts;
        } else {
            outPath = outputPath_;
            inPts   = inPts_;
            outPts  = outPts_;
        }

        qInfo() << "ExportWorker 片段" << (segIdx + 1) << "/" << totalSegs
                << "in:" << inPts / 1000000.0 << "s out:" << outPts / 1000000.0 << "s";

        int64_t segFrames = 0;
        bool segOk = processSegment(inCtx, videoStreamIdx, vDecCtx,
                                    audioStreamIdx, aDecCtx,
                                    srcWidth, srcHeight,
                                    videoTimeBase, audioTimeBase, frameRate,
                                    swsCtx, swsFrame, hwDeviceCtx,
                                    outPath, inPts, outPts,
                                    segIdx, totalSegs, segFrames);
        totalVideoFrames += segFrames;

        if (!segOk) { allOk = false; break; }
        if (isBatch_) emit segmentCompleted(segIdx);
    }

    // ====================================================================
    // 会话清理
    // ====================================================================
session_cleanup:
    av_frame_free(&swsFrame);
    sws_freeContext(swsCtx);
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    if (aDecCtx) avcodec_free_context(&aDecCtx);
    avcodec_free_context(&vDecCtx);
    avformat_close_input(&inCtx);

    qInfo() << "导出全部完成: 共" << totalVideoFrames << "视频帧";
    emit exportFinished(allOk);
}

// ====================================================================
// processSegment：处理单个导出片段（创建输出+编码器 → seek → 编码 → flush → 关闭）
// ====================================================================
bool ExportWorker::processSegment(
    AVFormatContext* inCtx,
    int videoStreamIdx, AVCodecContext* vDecCtx,
    int audioStreamIdx, AVCodecContext* aDecCtx,
    int srcWidth, int srcHeight,
    AVRational videoTimeBase, AVRational audioTimeBase,
    AVRational frameRate,
    SwsContext* swsCtx, AVFrame* swsFrame,
    AVBufferRef* hwDeviceCtx,
    const QString& outPath, int64_t inPts, int64_t outPts,
    int segIdx, int totalSegs, int64_t& outFrameCount)
{
    outFrameCount = 0;
    AVFormatContext* outCtx     = nullptr;
    AVCodecContext*  vEncCtx    = nullptr;
    AVStream*        vOutStream = nullptr;
    AVCodecContext*  aEncCtx    = nullptr;
    AVStream*        aOutStream = nullptr;
    SwrContext*      swrCtx     = nullptr;
    AVFrame*         aResampled = nullptr;
    AVFrame*         vFrame     = nullptr;
    AVFrame*         aFrame     = nullptr;
    AVPacket*        inPkt      = nullptr;
    AVPacket*        encPkt     = nullptr;
    bool ok = false;

    AVFrame* hwTransferFrame = nullptr;  // P3 GPU→CPU 传输缓冲

    // --- 创建输出上下文 ---
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        outPath.toUtf8().constData()) < 0) {
        emit errorOccurred("无法创建输出文件: " + outPath);
        goto seg_cleanup;
    }

    // --- 视频编码器 ---
    {
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        bool hwEnc = (codec != nullptr);
        if (!codec) codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) { emit errorOccurred("无可用 H.264 编码器"); goto seg_cleanup; }

        vEncCtx = avcodec_alloc_context3(codec);
        vEncCtx->width      = srcWidth;
        vEncCtx->height     = srcHeight;
        vEncCtx->time_base  = AV_TIME_BASE_Q;
        vEncCtx->framerate  = frameRate;
        vEncCtx->pix_fmt    = AV_PIX_FMT_YUV420P;
        vEncCtx->gop_size   = frameRate.num / frameRate.den > 0
                                ? 2 * frameRate.num / frameRate.den : 50;
        vEncCtx->max_b_frames = 0;
        vEncCtx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (hwEnc) {
            av_opt_set(vEncCtx->priv_data, "preset",     "p1",   0);
            av_opt_set(vEncCtx->priv_data, "rc",         "vbr",  0);
            av_opt_set(vEncCtx->priv_data, "cq",         "18",   0);
            av_opt_set(vEncCtx->priv_data, "b_ref_mode", "disabled", 0);
        } else {
            av_opt_set(vEncCtx->priv_data, "preset",  "superfast", 0);
            av_opt_set(vEncCtx->priv_data, "crf",     "17",        0);
            av_opt_set(vEncCtx->priv_data, "profile", "high",      0);
            av_opt_set(vEncCtx->priv_data, "threads", "auto",      0);
        }

        if (avcodec_open2(vEncCtx, codec, nullptr) < 0) {
            emit errorOccurred("视频编码器打开失败");
            goto seg_cleanup;
        }

        vOutStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_from_context(vOutStream->codecpar, vEncCtx);
        vOutStream->time_base = vEncCtx->time_base;
    }

    // --- 音频编码器 ---
    if (aDecCtx) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (codec) {
            aEncCtx = avcodec_alloc_context3(codec);
            aEncCtx->sample_rate = aDecCtx->sample_rate;
            av_channel_layout_copy(&aEncCtx->ch_layout, &aDecCtx->ch_layout);
            aEncCtx->sample_fmt = codec->sample_fmts[0];
            aEncCtx->bit_rate   = 192000;
            aEncCtx->time_base  = {1, aDecCtx->sample_rate};
            aEncCtx->flags     |= AV_CODEC_FLAG_GLOBAL_HEADER;

            if (avcodec_open2(aEncCtx, codec, nullptr) >= 0) {
                aOutStream = avformat_new_stream(outCtx, nullptr);
                avcodec_parameters_from_context(aOutStream->codecpar, aEncCtx);
                aOutStream->time_base = aEncCtx->time_base;

                swrCtx = swr_alloc();
                av_opt_set_chlayout(swrCtx, "in_chlayout",  &aDecCtx->ch_layout, 0);
                av_opt_set_int(swrCtx, "in_sample_rate",     aDecCtx->sample_rate, 0);
                av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", aDecCtx->sample_fmt, 0);
                av_opt_set_chlayout(swrCtx, "out_chlayout",  &aEncCtx->ch_layout, 0);
                av_opt_set_int(swrCtx, "out_sample_rate",    aEncCtx->sample_rate, 0);
                av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", aEncCtx->sample_fmt, 0);
                swr_init(swrCtx);
            } else {
                avcodec_free_context(&aEncCtx);
                aEncCtx = nullptr;
            }
        }
    }

    // --- 打开输出 + 写头部 ---
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, outPath.toUtf8().constData(),
                       AVIO_FLAG_WRITE) < 0) {
            emit errorOccurred("无法写入输出文件: " + outPath);
            goto seg_cleanup;
        }
    }
    if (avformat_write_header(outCtx, nullptr) < 0) {
        emit errorOccurred("写入文件头失败");
        goto seg_cleanup;
    }

    // --- Seek + 清空解码器 ---
    av_seek_frame(inCtx, -1, inPts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vDecCtx);
    if (aDecCtx) avcodec_flush_buffers(aDecCtx);

    // --- 分配帧/包 ---
    inPkt  = av_packet_alloc();
    vFrame = av_frame_alloc();
    aFrame = av_frame_alloc();
    if (aEncCtx) {
        aResampled = av_frame_alloc();
        aResampled->sample_rate = aEncCtx->sample_rate;
        av_channel_layout_copy(&aResampled->ch_layout, &aEncCtx->ch_layout);
        aResampled->format = aEncCtx->sample_fmt;
    }
    encPkt = av_packet_alloc();

    // ================================================================
    // 主编码循环
    // ================================================================
    int64_t audioFrameCount = 0;
    int64_t lastProgressUs  = 0;

    while (!isInterruptionRequested()) {
        int ret = av_read_frame(inCtx, inPkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                qInfo() << "av_read_frame 返回" << ret;
            break;
        }

        // --- 视频 ---
        if (inPkt->stream_index == videoStreamIdx) {
            int64_t pktPtsUs = (inPkt->pts != AV_NOPTS_VALUE)
                ? av_rescale_q(inPkt->pts, videoTimeBase, AV_TIME_BASE_Q) : 0;

            if (pktPtsUs > outPts + 5 * AV_TIME_BASE) {
                av_packet_unref(inPkt);
                break;
            }

            avcodec_send_packet(vDecCtx, inPkt);
            while (avcodec_receive_frame(vDecCtx, vFrame) >= 0) {
                int64_t fpsUs = (vFrame->pts != AV_NOPTS_VALUE)
                    ? av_rescale_q(vFrame->pts, videoTimeBase, AV_TIME_BASE_Q)
                    : pktPtsUs;

                if (fpsUs < inPts || fpsUs > outPts) {
                    av_frame_unref(vFrame); continue;
                }
                vFrame->pts = fpsUs - inPts;

                // 硬件解码帧 → CPU 内存传输
                AVFrame* srcFrame = vFrame;
                if (hwDeviceCtx && vFrame->format == AV_PIX_FMT_CUDA) {
                    if (!hwTransferFrame) {
                        hwTransferFrame = av_frame_alloc();
                        hwTransferFrame->format = AV_PIX_FMT_YUV420P;
                        hwTransferFrame->width  = srcWidth;
                        hwTransferFrame->height = srcHeight;
                        av_frame_get_buffer(hwTransferFrame, 0);
                    }
                    av_hwframe_transfer_data(hwTransferFrame, vFrame, 0);
                    av_frame_copy_props(hwTransferFrame, vFrame);
                    srcFrame = hwTransferFrame;
                }

                AVFrame* encFrame = srcFrame;
                if (srcFrame->format != AV_PIX_FMT_YUV420P ||
                    srcFrame->width  != srcWidth ||
                    srcFrame->height != srcHeight) {
                    sws_scale(swsCtx,
                              srcFrame->data, srcFrame->linesize, 0, srcHeight,
                              swsFrame->data, swsFrame->linesize);
                    swsFrame->pts = srcFrame->pts;
                    encFrame = swsFrame;
                }

                avcodec_send_frame(vEncCtx, encFrame);
                while (avcodec_receive_packet(vEncCtx, encPkt) >= 0) {
                    av_packet_rescale_ts(encPkt, vEncCtx->time_base, vOutStream->time_base);
                    encPkt->stream_index = vOutStream->index;
                    encPkt->pos = -1;
                    av_write_frame(outCtx, encPkt);
                    av_packet_unref(encPkt);
                }

                outFrameCount++;
                if (fpsUs - lastProgressUs > 1000000) {
                    lastProgressUs = fpsUs;
                    emit progressed(fpsUs, outPts);
                }
                av_frame_unref(vFrame);
            }
        }
        // --- 音频 ---
        else if (aDecCtx && aEncCtx && inPkt->stream_index == audioStreamIdx) {
            avcodec_send_packet(aDecCtx, inPkt);
            while (avcodec_receive_frame(aDecCtx, aFrame) >= 0) {
                int64_t fpsUs = (aFrame->pts != AV_NOPTS_VALUE)
                    ? av_rescale_q(aFrame->pts, audioTimeBase, AV_TIME_BASE_Q) : 0;
                if (fpsUs < inPts || fpsUs > outPts) {
                    av_frame_unref(aFrame); continue;
                }
                aFrame->pts = av_rescale_q(fpsUs - inPts, AV_TIME_BASE_Q, aEncCtx->time_base);

                AVFrame* encAFrame = aFrame;
                if (swrCtx) {
                    int outSamples = (int)av_rescale_rnd(
                        swr_get_delay(swrCtx, aEncCtx->sample_rate) + aFrame->nb_samples,
                        aEncCtx->sample_rate, aDecCtx->sample_rate, AV_ROUND_UP);
                    av_frame_unref(aResampled);
                    aResampled->nb_samples = outSamples;
                    aResampled->ch_layout = aEncCtx->ch_layout;
                    aResampled->sample_rate = aEncCtx->sample_rate;
                    aResampled->format = aEncCtx->sample_fmt;
                    av_frame_get_buffer(aResampled, 0);
                    swr_convert_frame(swrCtx, aResampled, aFrame);
                    aResampled->pts = aFrame->pts;
                    encAFrame = aResampled;
                }

                avcodec_send_frame(aEncCtx, encAFrame);
                while (avcodec_receive_packet(aEncCtx, encPkt) >= 0) {
                    av_packet_rescale_ts(encPkt, aEncCtx->time_base, aOutStream->time_base);
                    encPkt->stream_index = aOutStream->index;
                    encPkt->pos = -1;
                    av_write_frame(outCtx, encPkt);
                    av_packet_unref(encPkt);
                }
                audioFrameCount++;
                av_frame_unref(aFrame);
            }
        }

        av_packet_unref(inPkt);
    }

    // --- Flush 编码器 ---
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
    qInfo() << "片段" << (segIdx + 1) << "/" << totalSegs
            << "完成: 视频" << outFrameCount << "帧 音频" << audioFrameCount
            << "帧 时长" << ((outPts - inPts) / 1000000.0) << "s";

    ok = true;

seg_cleanup:
    av_frame_free(&hwTransferFrame);
    av_frame_free(&aResampled);
    av_frame_free(&aFrame);
    av_frame_free(&vFrame);
    av_packet_free(&encPkt);
    av_packet_free(&inPkt);
    if (swrCtx) swr_free(&swrCtx);
    if (aEncCtx) avcodec_free_context(&aEncCtx);
    avcodec_free_context(&vEncCtx);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }

    return ok;
}
