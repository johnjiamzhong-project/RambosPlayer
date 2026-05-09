#include "videodecodethread.h"

extern "C" {
#include <libavutil/avutil.h>
}

VideoDecodeThread::~VideoDecodeThread() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

// 根据 codec_id 查找并打开解码器
bool VideoDecodeThread::init(AVCodecParameters* params) {
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;
    return avcodec_open2(codecCtx_, codec, nullptr) >= 0;
}

// 设置停止标志并 abort 两侧队列，解除 run() 阻塞
void VideoDecodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
    if (outputQueue_) outputQueue_->abort();
}

// 标记需要 flush，run() 检测后调用 avcodec_flush_buffers 并清空输出队列
void VideoDecodeThread::flush() { flush_ = true; }

int VideoDecodeThread::width()  const { return codecCtx_ ? codecCtx_->width  : 0; }
int VideoDecodeThread::height() const { return codecCtx_ ? codecCtx_->height : 0; }
AVRational VideoDecodeThread::timeBase() const { return timeBase_; }

// 主循环：取包 → send_packet → receive_frame → clone 推入输出队列
void VideoDecodeThread::run() {
    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    while (!abort_) {
        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            outputQueue_->clear();
        }
        if (!inputQueue_->tryPop(pkt, 20)) continue;
        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);
        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            AVFrame* out = av_frame_clone(frame);
            outputQueue_->push(out);
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    emit finished();
}
