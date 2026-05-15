#include "capturethread.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavdevice/avdevice.h>
}

CaptureThread::~CaptureThread() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
    if (fmtCtx_)   avformat_close_input(&fmtCtx_);
}

// 根据 source 选择采集设备，打开输入格式、探测流、创建解码器。
// desktop → gdigrab（Windows 桌面采集），其他 → dshow 摄像头。
bool CaptureThread::init(const QString& source, int width, int height, int fps) {
    // 重复调用时先释放上一次的资源，避免 avformat_open_input 收到非空指针
    if (codecCtx_) { avcodec_free_context(&codecCtx_); }
    if (fmtCtx_)   { avformat_close_input(&fmtCtx_); }
    videoIdx_ = -1;

    abort_.store(false, std::memory_order_relaxed);
    source_ = source;
    width_  = width;
    height_ = height;
    fps_    = fps;

    avdevice_register_all();

    const AVInputFormat* fmt = nullptr;
    AVDictionary* opts = nullptr;

    if (source.toLower() == QStringLiteral("desktop")) {
        fmt = av_find_input_format("gdigrab");
        if (!fmt) { qWarning() << "CaptureThread: gdigrab not available"; return false; }
    } else {
        fmt = av_find_input_format("dshow");
        if (!fmt) { qWarning() << "CaptureThread: dshow not available"; return false; }
        av_dict_set(&opts, "video_size",  QString("%1x%2").arg(width).arg(height).toUtf8(), 0);
        av_dict_set(&opts, "framerate",  QString::number(fps).toUtf8(), 0);
        av_dict_set(&opts, "rtbufsize",  "50M", 0);
    }

    // 桌面模式额外设置帧率和缓冲区
    if (source.toLower() == QStringLiteral("desktop")) {
        av_dict_set(&opts, "framerate", QString::number(fps).toUtf8(), 0);
    }

    if (avformat_open_input(&fmtCtx_, source == "desktop" ? "desktop" : ("video=" + source).toUtf8(),
                            fmt, &opts) < 0) {
        qWarning() << "CaptureThread: avformat_open_input failed for" << source;
        return false;
    }

    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0) {
        avformat_close_input(&fmtCtx_);
        return false;
    }

    // 找第一条视频流
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        if (fmtCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx_ = (int)i;
            break;
        }
    }
    if (videoIdx_ < 0) { qWarning() << "CaptureThread: no video stream found"; return false; }

    AVCodecParameters* cp = fmtCtx_->streams[videoIdx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) { qWarning() << "CaptureThread: codec not found"; return false; }
    codecCtx_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx_, cp) < 0) return false;
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) return false;

    qInfo() << "CaptureThread::init ok source =" << source
            << "codec =" << codec->name << "size =" << width_ << "x" << height_;
    return true;
}

// 设置停止标志并 abort 输出队列
void CaptureThread::stop() {
    abort_ = true;
    if (outputQueue_) outputQueue_->abort();
}

// 主循环：av_read_frame → 解码 → clone → push 到输出队列
void CaptureThread::run() {
    if (!fmtCtx_ || !codecCtx_ || !outputQueue_) return;

    emit captureStarted();
    AVPacket* pkt = av_packet_alloc();
    AVFrame* src   = av_frame_alloc();

    while (!abort_.load(std::memory_order_relaxed)) {
        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            // 采集设备偶发错误（如图形卡顿），短暂等待后重试
            QThread::msleep(10);
            continue;
        }

        if (pkt->stream_index != videoIdx_) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(codecCtx_, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(codecCtx_, src) == 0) {
            AVFrame* out = av_frame_clone(src);
            outputQueue_->push(out);
            emit frameCaptured();
            av_frame_unref(src);
        }
    }

    av_frame_free(&src);
    av_packet_free(&pkt);
    qInfo() << "CaptureThread::run finished";
}
