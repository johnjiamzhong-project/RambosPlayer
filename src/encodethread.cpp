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
    ptsIdx_ = 0;
    width_ = width; height_ = height; fps_ = fps; bitrate_ = bitrate;

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
    codecCtx_->gop_size  = (gopSize_ > 0) ? gopSize_ : fps;   // 默认 1 秒一个关键帧

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

// seek 后重建编码器：libx264 调用 avcodec_flush_buffers 会进入 EOS/drain 模式，
// 之后所有 avcodec_send_frame 返回错误。必须销毁并重建编码器上下文。
bool EncodeThread::reopenCodec() {
    avcodec_free_context(&codecCtx_);
    if (swsCtx_)   { sws_freeContext(swsCtx_);  swsCtx_   = nullptr; }
    if (swsFrame_) { av_frame_free(&swsFrame_);  swsFrame_ = nullptr; }

    const AVCodec* codec = nullptr;
    if (hwEnc_) codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec)  codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)  codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec)  codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        qWarning() << "EncodeThread::reopenCodec: no H.264 encoder";
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;

    codecCtx_->width     = width_;
    codecCtx_->height    = height_;
    codecCtx_->time_base = {1, fps_};
    codecCtx_->framerate = {fps_, 1};
    codecCtx_->pix_fmt   = AV_PIX_FMT_YUV420P;
    codecCtx_->bit_rate  = bitrate_;
    codecCtx_->max_b_frames = 0;
    codecCtx_->gop_size  = (gopSize_ > 0) ? gopSize_ : fps_;
    codecCtx_->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (hwEnc_) {
        av_opt_set(codecCtx_->priv_data, "preset",      "p4",  0);
        av_opt_set(codecCtx_->priv_data, "tune",        "ll",  0);
        av_opt_set(codecCtx_->priv_data, "zerolatency", "1",   0);
        av_opt_set(codecCtx_->priv_data, "rc",          "cbr", 0);
    } else {
        av_opt_set(codecCtx_->priv_data, "preset", "ultrafast",   0);
        av_opt_set(codecCtx_->priv_data, "tune",   "zerolatency", 0);
    }

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        qWarning() << "EncodeThread::reopenCodec: avcodec_open2 failed";
        avcodec_free_context(&codecCtx_);
        return false;
    }
    qInfo() << "EncodeThread::reopenCodec ok codec =" << codec->name;
    return true;
}

// 设置停止标志并 abort 两侧队列
void EncodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
    for (auto* q : outputQueues_) q->abort();
}

// 刷新编码器缓冲：发送空帧以排出残留包，fan-out 到所有输出队列
void EncodeThread::flush() {
    if (!codecCtx_) return;
    avcodec_send_frame(codecCtx_, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        for (auto* q : outputQueues_) {
            AVPacket* copy = av_packet_clone(pkt);
            q->push(copy);
        }
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

        if (!frame) {
            // sentinel（seek）：只在已有帧时 flush（ptsIdx_>0），避免对刚初始化的编码器 flush 导致状态异常
            qInfo() << "EncodeThread: seek sentinel received ptsIdx_=" << ptsIdx_
                    << "outputQueues=" << outputQueues_.size();
            if (ptsIdx_ > 0) reopenCodec();
            ptsIdx_ = 0;
            firstFrameAfterSentinel_ = true;
            for (auto* q : outputQueues_) { q->clear(); q->tryPush(nullptr); }
            qInfo() << "EncodeThread: nullptr pushed to" << outputQueues_.size() << "output queues";
            continue;
        }

        if (firstFrameAfterSentinel_) {
            qInfo() << "EncodeThread: first frame after sentinel pts=" << frame->pts
                    << "format=" << frame->format
                    << "size=" << frame->width << "x" << frame->height;
            firstFrameAfterSentinel_ = false;
        }

        frame->pts = ptsIdx_++;

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

        int sendRet = avcodec_send_frame(codecCtx_, encodeFrame);
        if (sendRet < 0) {
            qWarning() << "EncodeThread: avcodec_send_frame failed ret=" << sendRet
                       << "pts=" << encodeFrame->pts;
            av_frame_free(&frame); continue;
        }
        av_frame_free(&frame);

        // 收已编码的包，fan-out clone 到每路 MuxThread 输入队列
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
            if (ptsIdx_ <= 2) {  // 只打印 sentinel 后前两帧的包，验证编码器状态
                qInfo() << "EncodeThread: pkt produced ptsIdx_=" << ptsIdx_
                        << "pkt.pts=" << pkt->pts
                        << "flags=" << pkt->flags
                        << "size=" << pkt->size;
            }
            for (auto* q : outputQueues_) {
                AVPacket* copy = av_packet_clone(pkt);
                q->push(copy);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    // 推流结束前 flush 编码器残留包
    flush();
    emit finished();
}
