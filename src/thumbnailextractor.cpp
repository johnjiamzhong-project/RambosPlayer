#include "thumbnailextractor.h"
#include <QtMath>

ThumbnailExtractor::ThumbnailExtractor(QObject* parent)
    : QThread(parent), count_(8)
{
}

ThumbnailExtractor::~ThumbnailExtractor()
{
    if (isRunning()) {
        requestInterruption();
        wait(3000);
    }
}

void ThumbnailExtractor::extract(const QString& path, int count)
{
    if (isRunning()) {
        emit errorOccurred("上一次提取尚未完成");
        return;
    }
    path_ = path;
    count_ = qMax(1, count);
    start();
}

void ThumbnailExtractor::run()
{
    QList<QImage> result;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, path_.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit errorOccurred("无法打开文件: " + path_);
        return;
    }

    // 限制探测时长，避免大文件卡死
    fmtCtx->max_analyze_duration = 3 * AV_TIME_BASE;   // 最多分析 3 秒
    fmtCtx->probesize = 5 * 1024 * 1024;                 // 最多读 5 MB

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        emit errorOccurred("无法获取流信息");
        avformat_close_input(&fmtCtx);
        return;
    }

    int videoIdx = -1;
    AVCodecParameters* codecParams = nullptr;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            codecParams = fmtCtx->streams[i]->codecpar;
            break;
        }
    }

    if (videoIdx < 0) {
        emit errorOccurred("文件中没有视频流");
        avformat_close_input(&fmtCtx);
        return;
    }

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        emit errorOccurred("找不到解码器");
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecParams);

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        emit errorOccurred("无法打开解码器");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    int thumbHeight = 90;
    int thumbWidth  = codecParams->width * thumbHeight / codecParams->height;
    if (thumbWidth < 80)
        thumbWidth = 80;

    int64_t duration = fmtCtx->duration;
    if (duration <= 0 || duration == AV_NOPTS_VALUE)
        duration = AV_TIME_BASE;

    AVRational videoTimeBase = fmtCtx->streams[videoIdx]->time_base;
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    for (int i = 0; i < count_ && !isInterruptionRequested(); ++i) {
        int64_t targetUs = duration * (i + 1) / (count_ + 1);

        int ret = av_seek_frame(fmtCtx, videoIdx,
                                av_rescale_q(targetUs, AV_TIME_BASE_Q, videoTimeBase),
                                AVSEEK_FLAG_BACKWARD);
        if (ret < 0)
            continue;

        avcodec_flush_buffers(codecCtx);

        bool gotFrame = false;
        while (!gotFrame && !isInterruptionRequested()) {
            ret = av_read_frame(fmtCtx, pkt);
            if (ret < 0)
                break;

            if (pkt->stream_index == videoIdx) {
                ret = avcodec_send_packet(codecCtx, pkt);
                if (ret >= 0) {
                    ret = avcodec_receive_frame(codecCtx, frame);
                    if (ret >= 0) {
                        QImage img = frameToImage(frame, codecCtx, thumbWidth, thumbHeight);
                        if (!img.isNull()) {
                            result.append(img);
                            emit thumbnailReady(img);  // 逐张送达
                        }
                        gotFrame = true;
                    }
                }
            }
            av_packet_unref(pkt);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (!isInterruptionRequested())
        emit thumbnailsReady(result);
}

QImage ThumbnailExtractor::frameToImage(AVFrame* frame, AVCodecContext* codecCtx,
                                         int thumbWidth, int thumbHeight)
{
    SwsContext* sws = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        thumbWidth, thumbHeight, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws)
        return QImage();

    QImage img(thumbWidth, thumbHeight, QImage::Format_RGB32);
    uint8_t* dstData[4]    = { img.bits(), nullptr, nullptr, nullptr };
    int      dstLinesize[4] = { img.bytesPerLine(), 0, 0, 0 };

    sws_scale(sws, frame->data, frame->linesize, 0, codecCtx->height,
              dstData, dstLinesize);
    sws_freeContext(sws);
    return img;
}
