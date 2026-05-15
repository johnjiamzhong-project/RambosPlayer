#include "muxthread.h"
#include <QDebug>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

MuxThread::~MuxThread() {
    stop(); wait();
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_close(fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
    }
}

// 打开 FLV 输出：创建 AVFormatContext，添加 H.264 视频流并设 codecpar，avio 连接目标。
// RTMP URL 走 librtmp 协议，本地 .flv 路径走文件写入。
bool MuxThread::init(const QString& url, int width, int height, AVRational timeBase,
                     const uint8_t* extradata, int extradataSize) {
    // 重复调用时先关闭上一次的 IO 和格式上下文
    if (fmtCtx_) {
        if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
        avformat_free_context(fmtCtx_);
        fmtCtx_ = nullptr;
    }
    vStream_ = nullptr;

    abort_.store(false, std::memory_order_relaxed);
    url_    = url;
    isRtmp_ = url.startsWith(QStringLiteral("rtmp://"));

    if (avformat_alloc_output_context2(&fmtCtx_, nullptr, "flv", nullptr) < 0) {
        qWarning() << "MuxThread: avformat_alloc_output_context2 failed"; return false;
    }

    // 添加视频流并填充 codecpar（含 extradata = SPS/PPS）
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { qWarning() << "MuxThread: H.264 codec not found"; return false; }
    vStream_ = avformat_new_stream(fmtCtx_, codec);
    if (!vStream_) { qWarning() << "MuxThread: avformat_new_stream failed"; return false; }

    vStream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vStream_->codecpar->codec_id   = AV_CODEC_ID_H264;
    vStream_->codecpar->width      = width;
    vStream_->codecpar->height     = height;
    vStream_->time_base            = timeBase;
    codecTimeBase_                 = timeBase;  // 保存编码器时间基，run() 中做 rescale

    // 从编码器复制 extradata（H.264 SPS/PPS），否则 FLV 文件无法解码
    if (extradata && extradataSize > 0) {
        vStream_->codecpar->extradata = (uint8_t*)av_mallocz(extradataSize + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(vStream_->codecpar->extradata, extradata, extradataSize);
        vStream_->codecpar->extradata_size = extradataSize;
    }

    // 打开输出 IO
    if (!(fmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx_->pb, url.toUtf8(), AVIO_FLAG_WRITE) < 0) {
            qWarning() << "MuxThread: avio_open failed" << url;
            return false;
        }
    }

    // 写文件头（RTMP 时写 FLV header）
    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        qWarning() << "MuxThread: avformat_write_header failed";
        return false;
    }

    connected_ = true;
    emit connected();
    qInfo() << "MuxThread::init ok url =" << url << "isRtmp =" << isRtmp_
            << "size =" << width << "x" << height;
    return true;
}

// 设置停止标志并 abort 输入队列
void MuxThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
}

// 主循环：从编码包队列取包 → av_interleaved_write_frame
void MuxThread::run() {
    if (!fmtCtx_ || !inputQueue_) return;

    AVPacket* pkt = nullptr;
    int64_t frameCount = 0;

    while (!abort_.load(std::memory_order_relaxed)) {
        if (!inputQueue_->tryPop(pkt, 20)) continue;

        // 将 pts/dts/duration 从编码器时间基转换到 stream 时间基（FLV muxer 用 {1,1000}）
        pkt->stream_index = vStream_->index;
        av_packet_rescale_ts(pkt, codecTimeBase_, vStream_->time_base);

        if (av_interleaved_write_frame(fmtCtx_, pkt) < 0) {
            av_packet_free(&pkt);
            qWarning() << "MuxThread: write frame error, retrying";
            continue;
        }

        av_packet_free(&pkt);
        frameCount++;
    }

    // 推流结束：写封装尾后立即关闭 IO，确保缓冲区冲刷到磁盘文件
    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    connected_ = false;
    qInfo() << "MuxThread::run finished, frames written =" << frameCount;
    emit finished();
}
