#pragma once
#include <QThread>
#include <QMutex>
#include <atomic>
#include "framequeue.h"
#include "filtergraph.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

// VideoDecodeThread: 视频解码线程
// 消费 FrameQueue<AVPacket*> 输入队列，解码为 AVFrame* 后推入输出队列。
// init() 打开解码器，run() 循环 avcodec_send_packet / avcodec_receive_frame。
// flush() 用于 seek 后清空解码器缓冲。
// 解码后可选经过 FilterGraph 过滤（亮度/对比度/饱和度/水印），
// 滤镜参数通过 atomic 变量跨线程传递，FilterPanel 由 GUI 线程调用 setter。
class VideoDecodeThread : public QThread {
    Q_OBJECT
public:
    ~VideoDecodeThread() override;

    // hwEnabled: 尝试 D3D11VA 硬解，失败则静默回退软解
    bool init(AVCodecParameters* params, bool hwEnabled = false);
    void stop();
    void flush();

    void setInputQueue(FrameQueue<AVPacket*>* q)  { inputQueue_  = q; }
    void setOutputQueue(FrameQueue<AVFrame*>* q)  { outputQueue_ = q; }

    int width()  const;
    int height() const;
    AVRational timeBase() const;

    // 滤镜参数 setter（GUI 线程调用，线程安全）
    void setFilterEnabled(bool on)    { filterEnabled_ = on; filterDirty_ = true; }
    void setBrightness(float v);      // -1.0 .. 1.0
    void setContrast(float v);        // -1.0 .. 1.0（映射到 colorbalance 范围）
    void setSaturation(float v);      // 0.0 .. 3.0
    void setWatermark(const QString& path);

signals:
    void finished();

protected:
    void run() override;

private:
    QString buildFilterDesc() const;   // 拼装滤镜描述字符串

    AVCodecContext* codecCtx_ = nullptr;            // 解码器上下文
    FrameQueue<AVPacket*>* inputQueue_ = nullptr;   // 输入：视频包队列
    FrameQueue<AVFrame*>* outputQueue_ = nullptr;   // 输出：解码帧队列
    std::atomic<bool> abort_{false};                // 停止标志
    std::atomic<bool> flush_{false};                // Seek 后清空解码器缓冲
    AVRational timeBase_{1, 1};                     // 时间基，用于外部查询
    bool hwAccel_ = false;                          // 是否成功启用了硬件加速

    // 滤镜参数（GUI 线程写，解码线程读）
    std::atomic<bool>  filterEnabled_{false};       // 滤镜开关
    std::atomic<bool>  filterDirty_{false};         // 参数变更标记
    std::atomic<float> brightness_{0.0f};           // -1.0 .. 1.0
    std::atomic<float> contrast_{0.0f};             // -1.0 .. 1.0
    std::atomic<float> saturation_{1.0f};           // 0.0 .. 3.0
    mutable QMutex     watermarkMtx_;               // 水印路径互斥锁（mutable 允许 const 函数加锁）
    QString            watermarkPath_;              // 空串=禁用水印
    FilterGraph        filterGraph_;                // libavfilter 滤镜图
};
