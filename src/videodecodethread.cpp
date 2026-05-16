#include "videodecodethread.h"
#include "hwaccel.h"
#include <QDebug>
#include <QFileInfo>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>
}

VideoDecodeThread::~VideoDecodeThread() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

// 根据 codec_id 查找并打开解码器，同时以直通模式初始化 FilterGraph。
// 若 hwEnabled 为 true，先尝试创建 D3D11VA 硬解设备并绑定到解码上下文；
// 创建失败或绑定失败时静默回退到软解（qWarning 记日志）。
bool VideoDecodeThread::init(AVCodecParameters* params, bool hwEnabled) {
    abort_.store(false, std::memory_order_relaxed);

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;

    // 保存时间基，供外部查询
    if (params->width > 0 && params->height > 0)
        timeBase_ = {1, 25};  // 默认值，后续由 PlayerController 覆盖

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

    // 初始化滤镜图为直通模式，后续由 rebuild 按需激活。
    // 硬解时 codecCtx_->pix_fmt = AV_PIX_FMT_D3D11，但帧经 av_hwframe_transfer_data
    // 后为 NV12（D3D11VA 通用软解输出），必须用软解格式初始化 buffersrc，否则送帧时崩溃。
    AVPixelFormat filterFmt = hwAccel_ ? AV_PIX_FMT_NV12 : codecCtx_->pix_fmt;
    swFmt_ = AV_PIX_FMT_NONE;  // 重置，等首帧后由 run() 更新
    filterGraph_.init(codecCtx_->width, codecCtx_->height,
                      filterFmt, timeBase_, QString{});

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) return false;
    qInfo() << "VideoDecodeThread::init ok codec=" << codec->name
            << "size=" << params->width << "x" << params->height
            << "hw=" << hwAccel_;
    return true;
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

// 滤镜参数 setter（GUI 线程安全）
void VideoDecodeThread::setBrightness(float v)   { brightness_ = v; filterDirty_ = true; }
void VideoDecodeThread::setContrast(float v)     { contrast_   = v; filterDirty_ = true; }
void VideoDecodeThread::setSaturation(float v)   { saturation_ = v; filterDirty_ = true; }
void VideoDecodeThread::setWatermark(const QString& path) {
    // 诊断：直接用 avformat_open_input 测试 FFmpeg 能否访问该文件
    if (!path.isEmpty()) {
        QString fwd = path; fwd.replace('\\', '/');
        AVFormatContext* probe = nullptr;
        int r = avformat_open_input(&probe, fwd.toUtf8().constData(), nullptr, nullptr);
        if (r < 0) {
            char eb[128]; av_strerror(r, eb, sizeof(eb));
            qWarning() << "VideoDecodeThread::setWatermark avformat_open_input FAILED"
                       << fwd << "error:" << eb;
        } else {
            qInfo() << "VideoDecodeThread::setWatermark avformat_open_input OK" << fwd;
            avformat_close_input(&probe);
        }
    }
    { QMutexLocker lk(&watermarkMtx_); watermarkPath_ = path; }
    filterDirty_ = true;
}

// 从当前 atomic 参数拼接 FFmpeg 滤镜描述字符串。
// 亮度/饱和度用 hue，对比度用 colorbalance，水印用 movie+overlay。
QString VideoDecodeThread::buildFilterDesc() const {
    QStringList parts;
    float b = brightness_.load(std::memory_order_relaxed);
    float s = saturation_.load(std::memory_order_relaxed);

    // hue 滤镜：b=亮度(-10..10), s=饱和度(0..10)
    if (b != 0.0f || s != 1.0f)
        parts << QString("hue=b=%1:s=%2").arg(b, 0, 'f', 2).arg(s, 0, 'f', 2);

    // colorbalance 滤镜：用 midtones 做"对比度"效果
    float c = contrast_.load(std::memory_order_relaxed);
    if (c != 0.0f)
        parts << QString("colorbalance=rm=%1:gm=%1:bm=%1").arg(c, 0, 'f', 2);

    QMutexLocker lk(&watermarkMtx_);
    if (!watermarkPath_.isEmpty()) {
        if (!QFileInfo::exists(watermarkPath_)) {
            qWarning() << "VideoDecodeThread: watermark file not found:" << watermarkPath_;
        } else {
            QString fwdPath = watermarkPath_;
            fwdPath.replace('\\', '/');
            // Windows 盘符冒号需要两层转义穿透 FFmpeg 两道解析：
            //   Layer 1 av_get_token("[];,"): G\\:/ → 输出 G\:/ (\\ → \)
            //   Layer 2 av_set_options_string:  G\:/ → 输出 G:/  (\: → :)
            //   最终 movie filter 拿到 G:/path，avformat_open_input 正常打开。
            if (fwdPath.length() >= 2 && fwdPath[1] == ':')
                fwdPath.insert(1, "\\\\");  // 插入两个反斜杠（C++ 字面量 \\\\ = 两个字符 \\）
            QString filterStr = QString("movie=filename=%1[wm];[in][wm]overlay=10:10").arg(fwdPath);
            qInfo() << "VideoDecodeThread: watermark filter:" << filterStr;
            parts << filterStr;
        }
    }

    return parts.join(',');
}

// 主循环：取包 → send_packet → receive_frame → [滤镜] → clone 推入输出队列。
void VideoDecodeThread::run() {
    qInfo() << "VideoDecodeThread::run start";
    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* filtered = av_frame_alloc();

    while (!abort_) {
        // flush 处理
        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            outputQueue_->clear();
            qInfo() << "VideoDecodeThread: flush done";
        }

        // 滤镜重建
        if (filterEnabled_.load(std::memory_order_relaxed) && filterDirty_.exchange(false)) {
            QString desc = buildFilterDesc();
            filterGraph_.rebuild(desc);
            qInfo() << "VideoDecodeThread: filter rebuilt:" << desc;
        } else if (!filterEnabled_.load(std::memory_order_relaxed) && filterDirty_.exchange(false)) {
            filterGraph_.rebuild(QString{});
            qInfo() << "VideoDecodeThread: filter disabled (passthrough)";
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
                    swTmp->pts     = frame->pts;
                    swTmp->pkt_dts = frame->pkt_dts;
                    src = swTmp;

                    // 首帧转换后确认实际软解格式（D3D11VA 通常为 NV12，10-bit 内容为 P010）。
                    // 若与滤镜初始化时假定的格式不符，更新格式并触发重建，避免送帧崩溃。
                    if (swFmt_ == AV_PIX_FMT_NONE) {
                        swFmt_ = (AVPixelFormat)swTmp->format;
                        qInfo() << "VideoDecodeThread: hw sw_format =" << swFmt_;
                        if (swFmt_ != filterGraph_.pixFmt()) {
                            filterGraph_.setPixFmt(swFmt_);
                            if (!filterGraph_.isEmpty()) filterDirty_ = true;
                        }
                    }
                } else {
                    qWarning() << "VideoDecodeThread: av_hwframe_transfer_data failed, skip frame"
                               << "pts" << frame->pts;
                    av_frame_free(&swTmp);
                    av_frame_unref(frame);
                    continue;
                }
            }

            // 滤镜处理或克隆
            AVFrame* out;
            if (filterEnabled_.load(std::memory_order_relaxed) && !filterGraph_.isEmpty()) {
                av_frame_unref(filtered);
                if (filterGraph_.process(src, filtered) >= 0)
                    out = av_frame_clone(filtered);
                else
                    out = av_frame_clone(src);  // 滤镜失败，回退原始帧
            } else {
                out = av_frame_clone(src);
            }
            outputQueue_->push(out);

            if (swTmp) av_frame_free(&swTmp);
            av_frame_unref(frame);
        }
    }
    av_frame_free(&filtered);
    av_frame_free(&frame);
    filterGraph_.close();
    qInfo() << "VideoDecodeThread::run finished";
    emit finished();
}
