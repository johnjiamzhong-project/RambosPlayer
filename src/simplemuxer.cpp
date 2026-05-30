#include "simplemuxer.h"
#include "logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

SimpleMuxer::SimpleMuxer(QObject* parent) : QObject(parent) {}

bool SimpleMuxer::exec(const QString& videoFile,
                        const QString& audioFile,
                        const QString& output)
{
    AVFormatContext* vCtx    = nullptr;
    AVFormatContext* aCtx    = nullptr;
    AVFormatContext* outCtx  = nullptr;
    AVStream*        vOutStream = nullptr;
    AVStream*        aOutStream = nullptr;
    AVPacket*        vPkt    = nullptr;
    AVPacket*        aPkt    = nullptr;
    int              vIdx    = -1;
    int              aIdx    = -1;
    bool             vEof    = false;
    bool             aEof    = false;
    bool             ok      = false;

    // --- 打开视频输入 ---
    if (avformat_open_input(&vCtx, videoFile.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit errorOccurred("无法打开视频文件: " + videoFile);
        goto done;
    }
    avformat_find_stream_info(vCtx, nullptr);
    vIdx = av_find_best_stream(vCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) {
        emit errorOccurred("视频文件中未找到视频流: " + videoFile);
        goto done;
    }

    // --- 打开音频输入 ---
    if (avformat_open_input(&aCtx, audioFile.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit errorOccurred("无法打开音频文件: " + audioFile);
        goto done;
    }
    avformat_find_stream_info(aCtx, nullptr);
    aIdx = av_find_best_stream(aCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aIdx < 0) {
        emit errorOccurred("音频文件中未找到音频流: " + audioFile);
        goto done;
    }

    // --- 创建输出上下文 ---
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                        output.toUtf8().constData()) < 0) {
        emit errorOccurred("无法创建输出上下文");
        goto done;
    }
    {
        vOutStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_copy(vOutStream->codecpar, vCtx->streams[vIdx]->codecpar);
        vOutStream->codecpar->codec_tag = 0;
        vOutStream->time_base = vCtx->streams[vIdx]->time_base;

        aOutStream = avformat_new_stream(outCtx, nullptr);
        avcodec_parameters_copy(aOutStream->codecpar, aCtx->streams[aIdx]->codecpar);
        aOutStream->codecpar->codec_tag = 0;
        aOutStream->time_base = aCtx->streams[aIdx]->time_base;
    }
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, output.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            emit errorOccurred("无法写入输出文件: " + output);
            goto done;
        }
    }
    if (avformat_write_header(outCtx, nullptr) < 0) {
        emit errorOccurred("写入文件头失败");
        goto done;
    }

    // --- 估算总时长 ---
    {
        int64_t vDurUs = av_rescale_q(vCtx->streams[vIdx]->duration,
                                       vCtx->streams[vIdx]->time_base, AV_TIME_BASE_Q);
        int64_t aDurUs = av_rescale_q(aCtx->streams[aIdx]->duration,
                                       aCtx->streams[aIdx]->time_base, AV_TIME_BASE_Q);
        int64_t totalUs = qMax(vDurUs, aDurUs);

        // --- 双源交错写包（按 DTS 选择）---
        vPkt = av_packet_alloc();
        aPkt = av_packet_alloc();

        // 辅助：从视频文件读下一个视频包
        auto readV = [&]() {
            while (!vEof) {
                if (av_read_frame(vCtx, vPkt) < 0) { vEof = true; break; }
                if (vPkt->stream_index == vIdx) return;
                av_packet_unref(vPkt);
            }
        };
        // 辅助：从音频文件读下一个音频包
        auto readA = [&]() {
            while (!aEof) {
                if (av_read_frame(aCtx, aPkt) < 0) { aEof = true; break; }
                if (aPkt->stream_index == aIdx) return;
                av_packet_unref(aPkt);
            }
        };

        readV();
        readA();

        int64_t lastProgressUs = 0;

        while (!vEof || !aEof) {
            int64_t vDtsUs = (!vEof && vPkt->dts != AV_NOPTS_VALUE)
                ? av_rescale_q(vPkt->dts, vCtx->streams[vIdx]->time_base, AV_TIME_BASE_Q)
                : INT64_MAX;
            int64_t aDtsUs = (!aEof && aPkt->dts != AV_NOPTS_VALUE)
                ? av_rescale_q(aPkt->dts, aCtx->streams[aIdx]->time_base, AV_TIME_BASE_Q)
                : INT64_MAX;

            if (!vEof && vDtsUs <= aDtsUs) {
                AVPacket* tmp = av_packet_clone(vPkt);
                tmp->stream_index = vOutStream->index;
                av_packet_rescale_ts(tmp, vCtx->streams[vIdx]->time_base, vOutStream->time_base);
                tmp->pos = -1;
                av_interleaved_write_frame(outCtx, tmp);
                av_packet_free(&tmp);

                if (totalUs > 0 && vDtsUs > lastProgressUs) {
                    lastProgressUs = vDtsUs;
                    emit progressed((int)(vDtsUs * 100 / totalUs));
                }
                av_packet_unref(vPkt);
                readV();
            } else {
                AVPacket* tmp = av_packet_clone(aPkt);
                tmp->stream_index = aOutStream->index;
                av_packet_rescale_ts(tmp, aCtx->streams[aIdx]->time_base, aOutStream->time_base);
                tmp->pos = -1;
                av_interleaved_write_frame(outCtx, tmp);
                av_packet_free(&tmp);

                av_packet_unref(aPkt);
                readA();
            }
        }

        av_write_trailer(outCtx);
    }

    qInfo() << "SimpleMuxer 完成:" << output;
    ok = true;

done:
    av_packet_free(&vPkt);
    av_packet_free(&aPkt);
    if (outCtx) {
        if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb)
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
    }
    if (aCtx) avformat_close_input(&aCtx);
    if (vCtx) avformat_close_input(&vCtx);
    return ok;
}
