#include "exportworker.h"
#include "logger.h"
#include <QFileInfo>
#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
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
    inputPath_  = inputPath;
    outputPath_ = outputPath;
    inPts_      = inPts;
    outPts_     = outPts;
    start();
}

void ExportWorker::run()
{
    qInfo() << "ExportWorker 启动 inPts:" << inPts_ / 1000000.0 << "s outPts:" << outPts_ / 1000000.0 << "s";

    // === 1. 打开输入文件 ===
    AVFormatContext* inCtx = nullptr;
    if (avformat_open_input(&inCtx, inputPath_.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit errorOccurred("无法打开输入文件: " + inputPath_);
        emit exportFinished(false);
        return;
    }
    if (avformat_find_stream_info(inCtx, nullptr) < 0) {
        emit errorOccurred("无法获取输入流信息");
        avformat_close_input(&inCtx);
        emit exportFinished(false);
        return;
    }

    // === 2. 创建输出上下文 ===
    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, nullptr,
                                       outputPath_.toUtf8().constData()) < 0) {
        emit errorOccurred("无法创建输出文件: " + outputPath_);
        avformat_close_input(&inCtx);
        emit exportFinished(false);
        return;
    }

    // 流索引映射：inputIdx → outputIdx
    int streamMap[256];
    memset(streamMap, -1, sizeof(streamMap));

    for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
        AVCodecParameters* inPar = inCtx->streams[i]->codecpar;
        if (inPar->codec_type != AVMEDIA_TYPE_VIDEO &&
            inPar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        AVStream* outStream = avformat_new_stream(outCtx, nullptr);
        if (!outStream)
            continue;

        avcodec_parameters_copy(outStream->codecpar, inPar);
        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inCtx->streams[i]->time_base;
        streamMap[i] = outStream->index;
    }

    // === 3. 打开输出文件并写头部 ===
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, outputPath_.toUtf8().constData(),
                      AVIO_FLAG_WRITE) < 0) {
            emit errorOccurred("无法打开输出 IO: " + outputPath_);
            avformat_free_context(outCtx);
            avformat_close_input(&inCtx);
            emit exportFinished(false);
            return;
        }
    }

    if (avformat_write_header(outCtx, nullptr) < 0) {
        emit errorOccurred("写入文件头失败");
        if (!(outCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&outCtx->pb);
        avformat_free_context(outCtx);
        avformat_close_input(&inCtx);
        emit exportFinished(false);
        return;
    }

    // === 4. Seek 到入口位置 ===
    // 对每个流 seek（使用 AV_TIME_BASE 全局时间基）
    int64_t seekTarget = inPts_;
    int ret = av_seek_frame(inCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        qInfo() << "seek 失败，从头开始";
    }

    // === 5. 读取并透传包 ===
    // -c copy 模式：从 seek 点（最近关键帧）开始全部写入，不跳帧，保证 GOP 完整。
    // 输出起点略早于 inPts_（到上一个关键帧），末尾在 outPts_ 后第一个关键帧截止。
    AVPacket* pkt = av_packet_alloc();
    int64_t lastPts    = 0;
    bool    videoDone  = false;
    bool    audioDone  = false;
    int     videoCount = 0;
    int     audioCount = 0;

    // 统计流数量，判断何时全部到达出口
    for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
        if (inCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) videoCount++;
        if (inCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audioCount++;
    }

    int64_t firstPtsUs  = -1;
    int      writtenCount = 0;
    int64_t  keyframePtsUs[256] = {};  // 每个流第一个关键帧的绝对 PTS（微秒），用作偏移
    bool     keyframeSeen[256] = {};   // 每个视频流是否已等到第一个关键帧
    bool     allKeyframesReady = (videoCount == 0);
    int64_t  ptsOffsetUs = 0;          // PTS 归零偏移（所有流取最早关键帧）

    while (!isInterruptionRequested()) {
        ret = av_read_frame(inCtx, pkt);
        if (ret < 0) {
            qInfo() << "av_read_frame 返回" << ret << "，停止读取";
            break;
        }

        int outIdx = streamMap[pkt->stream_index];
        if (outIdx < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* inStream  = inCtx->streams[pkt->stream_index];
        AVStream* outStream = outCtx->streams[outIdx];
        bool      isVideo   = (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        AVRational tb    = inStream->time_base;
        int64_t    ptsUs = av_rescale_q(pkt->pts, tb, AV_TIME_BASE_Q);

        // 等待所有视频流都遇到关键帧后再开始写（音视频同步启动）
        if (!allKeyframesReady) {
            if (isVideo && (pkt->flags & AV_PKT_FLAG_KEY)) {
                keyframePtsUs[outIdx] = ptsUs;
                keyframeSeen[outIdx] = true;
                allKeyframesReady = true;
                for (unsigned i = 0; i < inCtx->nb_streams; ++i) {
                    int oi = streamMap[i];
                    if (oi >= 0 && inCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        if (!keyframeSeen[oi]) { allKeyframesReady = false; break; }
                    }
                }
                if (allKeyframesReady) {
                    ptsOffsetUs = INT64_MAX;
                    for (int k = 0; k < 256; ++k) {
                        if (keyframeSeen[k] && keyframePtsUs[k] < ptsOffsetUs)
                            ptsOffsetUs = keyframePtsUs[k];
                    }
                    qInfo() << "关键帧就绪，PTS 偏移:" << (ptsOffsetUs / 1000000.0) << "s";
                }
            }
            // 关键帧就绪前丢弃所有包（含非关键帧视频 + 音频）；就绪后当前包不丢，继续写入
            if (!allKeyframesReady) {
                av_packet_unref(pkt);
                continue;
            }
        }

        // 出口检查：该流超过 outPts 后停止写入，等待其他流也完成
        if (ptsUs >= outPts_ && outPts_ > 0) {
            if (isVideo) videoDone = true;
            else        audioDone = true;
            av_packet_unref(pkt);

            bool allDone = (videoCount == 0 || videoDone) && (audioCount == 0 || audioDone);
            if (allDone) break;
            continue;
        }

        // PTS 归零：以第一个关键帧的实际 PTS 为偏移，避免多帧挤在 0 位
        int64_t ptsOffsetTb = av_rescale_q(ptsOffsetUs, AV_TIME_BASE_Q, tb);
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt->pts -= ptsOffsetTb;
            if (pkt->pts < 0) pkt->pts = 0;
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            pkt->dts -= ptsOffsetTb;
            if (pkt->dts < 0) pkt->dts = 0;
        }

        // 重映射并写入
        pkt->stream_index = outIdx;
        av_packet_rescale_ts(pkt, tb, outStream->time_base);
        pkt->pos = -1;

        av_interleaved_write_frame(outCtx, pkt);
        if (firstPtsUs < 0) firstPtsUs = ptsUs;
        lastPts = qMax(lastPts, ptsUs);
        writtenCount++;

        if (lastPts > 0)
            emit progressed(lastPts, outPts_);

        av_packet_unref(pkt);
    }

    qInfo() << "写入完成：首帧" << (firstPtsUs / 1000000.0)
            << "s 末帧" << (lastPts / 1000000.0) << "s"
            << "偏移" << (inPts_ / 1000000.0) << "s"
            << "输出时长≈" << ((lastPts - inPts_) / 1000000.0) << "s"
            << "共" << writtenCount << "个包";

    // === 6. 清理 ===
    av_packet_free(&pkt);
    av_write_trailer(outCtx);

    if (!(outCtx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outCtx->pb);

    avformat_free_context(outCtx);
    avformat_close_input(&inCtx);

    emit exportFinished(true);
}
