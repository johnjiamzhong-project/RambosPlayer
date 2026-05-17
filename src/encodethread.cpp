#include "encodethread.h"
#include <QDebug>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

EncodeThread::~EncodeThread() {
    stop(); wait();
    if (codecCtx_)  avcodec_free_context(&codecCtx_);
    if (swsCtx_)    sws_freeContext(swsCtx_);
    if (swsFrame_)  av_frame_free(&swsFrame_);
}

// 打开 H.264 编码器：优先 h264_nvenc（GPU 硬编），失败回退 libx264（CPU 软编），
// 再失败则尝试 openh264 / 通用 H.264。每一步失败原因写入日志便于排查。
bool EncodeThread::init(int width, int height, int fps, int bitrate) {
    // 重复调用时先释放上一次的编码器和转换上下文
    if (codecCtx_)  { avcodec_free_context(&codecCtx_); }
    if (swsCtx_)    { sws_freeContext(swsCtx_);  swsCtx_  = nullptr; }
    if (swsFrame_)  { av_frame_free(&swsFrame_);          }

    abort_.store(false, std::memory_order_relaxed);

    const AVCodec* codec = nullptr;

    // 1) 尝试 NVIDIA 硬编
    codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (codec) {
        hwEnc_ = true;
        qInfo() << "EncodeThread: using h264_nvenc (NVENC GPU encoder)";
    } else {
        qInfo() << "EncodeThread: h264_nvenc not available (no NVENC in vcpkg FFmpeg build)";
    }

    // 2) 尝试 libx264（需要 ffmpeg[gpl,x264]）
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libx264");
        if (codec) {
            qInfo() << "EncodeThread: using libx264 (GPL licensed)";
        } else {
            qInfo() << "EncodeThread: libx264 not available (vcpkg: install ffmpeg[gpl,x264])";
        }
    }

    // 3) 尝试 openh264（Cisco，BSD 许可，无需 GPL）
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libopenh264");
        if (codec) {
            qInfo() << "EncodeThread: using libopenh264 (Cisco)";
        } else {
            qInfo() << "EncodeThread: libopenh264 not available (vcpkg: install ffmpeg[openh264])";
        }
    }

    // 4) 兜底：按 codec_id 查找 H.264 编码器
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (codec)
            qInfo() << "EncodeThread: using built-in H.264 encoder:" << codec->name;
    }

    if (!codec) {
        qWarning() << "EncodeThread: NO H.264 encoder available!";
        qWarning() << "  -> Install: .\\vcpkg install ffmpeg[gpl,x264]  (recommended)";
        qWarning() << "  -> Or:     .\\vcpkg install ffmpeg[openh264]    (BSD license)";
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        qWarning() << "EncodeThread: avcodec_alloc_context3 failed";
        return false;
    }

    codecCtx_->width     = width;
    codecCtx_->height    = height;
    codecCtx_->time_base = {1, fps};
    codecCtx_->framerate = {fps, 1};
    codecCtx_->pix_fmt   = AV_PIX_FMT_YUV420P;
    codecCtx_->bit_rate  = bitrate;
    codecCtx_->max_b_frames = 0;  // 低延迟推流关 B 帧
    codecCtx_->gop_size  = fps;   // 1 秒一个关键帧

    // NVENC 特定优化
    if (hwEnc_) {
        av_opt_set(codecCtx_->priv_data, "preset",   "p4", 0);    // 低延迟预设
        av_opt_set(codecCtx_->priv_data, "tune",     "ll",  0);   // 低延迟调优
        av_opt_set(codecCtx_->priv_data, "zerolatency", "1", 0);
        av_opt_set(codecCtx_->priv_data, "rc",       "cbr", 0);   // 恒定码率
    } else {
        av_opt_set(codecCtx_->priv_data, "preset",   "ultrafast", 0);
        av_opt_set(codecCtx_->priv_data, "tune",     "zerolatency", 0);
    }

    // FLV 容器要求 SPS/PPS 存于 extradata（AVCC 格式），必须在 open 前设置
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        qWarning() << "EncodeThread: avcodec_open2 failed for" << codec->name;
        return false;
    }

    qInfo() << "EncodeThread::init ok codec =" << codec->name
            << "hw =" << hwEnc_ << "size =" << width << "x" << height;
    return true;
}

// 设置停止标志并 abort 两侧队列
void EncodeThread::stop() {
    abort_ = true;
    if (inputQueue_)  inputQueue_->abort();
    if (outputQueue_) outputQueue_->abort();
}

// 刷新编码器缓冲：发送空帧以排出残留包
void EncodeThread::flush() {
    if (!codecCtx_) return;
    avcodec_send_frame(codecCtx_, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        AVPacket* out = av_packet_clone(pkt);
        outputQueue_->push(out);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    qInfo() << "EncodeThread::flush done";
}

// 主循环：从原始帧队列取帧 → 编码 → 推编码包队列
void EncodeThread::run() {
    AVFrame* frame = nullptr;

    while (!abort_.load(std::memory_order_relaxed)) {
        if (!inputQueue_->tryPop(frame, 20)) continue;

        static int64_t idx = 0;
        frame->pts = idx++;

        // gdigrab 输出 bgr0/bgra，libx264 要求 yuv420p，按需做像素格式转换
        AVFrame* encodeFrame = frame;
        if (frame->format != (int)codecCtx_->pix_fmt) {
            if (!swsCtx_) {
                swsCtx_ = sws_getContext(
                    frame->width, frame->height, (AVPixelFormat)frame->format,
                    codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                swsFrame_ = av_frame_alloc();
                swsFrame_->format = (int)codecCtx_->pix_fmt;
                swsFrame_->width  = codecCtx_->width;
                swsFrame_->height = codecCtx_->height;
                av_frame_get_buffer(swsFrame_, 0);
                qInfo() << "EncodeThread: sws_scale"
                        << av_get_pix_fmt_name((AVPixelFormat)frame->format)
                        << "->" << av_get_pix_fmt_name(codecCtx_->pix_fmt);
            }
            sws_scale(swsCtx_,
                      frame->data, frame->linesize, 0, frame->height,
                      swsFrame_->data, swsFrame_->linesize);
            swsFrame_->pts = frame->pts;
            encodeFrame = swsFrame_;
        }

        if (avcodec_send_frame(codecCtx_, encodeFrame) < 0) {
            av_frame_free(&frame); continue;
        }
        av_frame_free(&frame);

        // 收已编码的包
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
            // pts/dts 由编码器按 frame->pts 填好，不覆写；
            // 时间基转换留给 MuxThread 在写包前做（它知道 stream->time_base）
            AVPacket* out = av_packet_clone(pkt);
            outputQueue_->push(out);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    // 推流结束前 flush 编码器残留包
    flush();
    emit finished();
}
