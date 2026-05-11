#include "videodecodethread.h"
#include "hwaccel.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

VideoDecodeThread::~VideoDecodeThread() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

// 根据 codec_id 查找并打开解码器。
// 若 hwEnabled 为 true，先尝试创建 D3D11VA 硬解设备并绑定到解码上下文；
// 创建失败或绑定失败时静默回退到软解（qWarning 记日志）。
bool VideoDecodeThread::init(AVCodecParameters* params, bool hwEnabled) {
    abort_.store(false, std::memory_order_relaxed);

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;

    // 尝试 D3D11VA 硬件加速
    if (hwEnabled) {
        HWAccel hw;
        if (hw.create(AV_HWDEVICE_TYPE_D3D11VA)) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hw.deviceCtx());
            hwAccel_ = true;
        } else {
            qWarning() << "VideoDecodeThread: D3D11VA 设备创建失败，回退软解";
        }
    }

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

// 主循环：取包 → send_packet → receive_frame → clone 推入输出队列。
// 硬解路径下，若帧在 GPU 显存中（format == D3D11），先 av_hwframe_transfer_data
// 搬回系统内存再 clone；转移失败时跳过该帧（避免 D3D11 帧进入下游导致灰色画面）。
void VideoDecodeThread::run() {
    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    while (!abort_) {
        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            outputQueue_->clear();
            qInfo() << "VideoDecodeThread: flush done";
        }
        if (!inputQueue_->tryPop(pkt, 20)) continue;
        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);
        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            AVFrame* src = frame;
            AVFrame* swTmp = nullptr;
            if (hwAccel_ && frame->format == AV_PIX_FMT_D3D11) {
                swTmp = av_frame_alloc();
                if (av_hwframe_transfer_data(swTmp, frame, 0) >= 0) {
                    // 手动拷贝时间戳，避免 av_frame_copy_props 误拷 time_base
                    // 导致下游 VideoRenderer 的 pts * timeBase_ 换算错误
                    swTmp->pts     = frame->pts;
                    swTmp->pkt_dts = frame->pkt_dts;
                    src = swTmp;
                } else {
                    qWarning() << "VideoDecodeThread: av_hwframe_transfer_data failed, skip frame"
                               << "pts" << frame->pts;
                    av_frame_free(&swTmp);
                    av_frame_unref(frame);
                    continue;
                }
            }
            AVFrame* out = av_frame_clone(src); 
            outputQueue_->push(out);
            if (swTmp) av_frame_free(&swTmp);
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    emit finished();
}
