// AudioDecodeThread: 音频解码线程
// 消费 FrameQueue<AVPacket*> 输入队列，解码后 swr_convert 重采样为 S16 Stereo 44100，
// 通过 QAudioOutput 输出 PCM，并更新 AVSync 音频时钟。
#pragma once
#include <QThread>
#include <QAudioOutput>
#include <QAudioFormat>
#include <atomic>
#include "framequeue.h"
#include "avsync.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

class AudioDecodeThread : public QThread {
    Q_OBJECT
public:
    ~AudioDecodeThread() override;

    bool init(AVCodecParameters* params, AVRational timeBase, AVSync* sync);
    void stop();
    void flush();
    void setVolume(float v);

    void setInputQueue(FrameQueue<AVPacket*>* q) { inputQueue_ = q; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext* codecCtx_ = nullptr;   // 解码器上下文
    SwrContext* swrCtx_ = nullptr;         // 重采样上下文 (→ S16 Stereo 44100)
    QAudioOutput* sink_ = nullptr;         // Qt 音频输出设备
    QIODevice* device_ = nullptr;          // QAudioOutput 写入设备（start() 返回值）
    AVSync* sync_ = nullptr;               // 音频时钟（解码后更新 PTS）
    AVRational timeBase_{1, 1};            // 时间基，用于 PTS → 秒
    FrameQueue<AVPacket*>* inputQueue_ = nullptr; // 输入：音频包队列
    std::atomic<bool> abort_{false};       // 停止标志
    std::atomic<bool> flush_{false};       // Seek 后清空缓冲
    std::atomic<float> pendingVolume_{-1.f}; // 待应用的音量（-1 表示无）
};
