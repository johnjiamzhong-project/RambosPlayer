#pragma once
#include <QThread>
#include <QList>
#include <QString>
#include <cstdint>
#include "audiomixpanel.h"

struct AVFilterContext;
struct AVCodecContext;
struct AVFormatContext;
struct AVStream;
struct AVFrame;
struct AVPacket;

// AudioMixWorker 的任务描述：源视频、音频区间列表和输出路径
struct AudioMixTask {
    QString               originalVideoPath;  // 始终为原始源视频
    QList<AudioMixRegion> regions;            // 音频区间（>0 个）
    QString               outputPath;         // 输出 MP4 路径
};

// AudioMixWorker：FFmpeg 音频混合线程。
// 分两步执行：
//   1. execMixAudio — 用 amix+adelay avfilter 将各区间音频混入源音频 → 临时 AAC
//   2. execMuxFinal — 复制源视频流 + 新 AAC 音频流 → 输出 MP4
// 每个区间可独立设置贴入时间、来源偏移、时长和音量比例。
class AudioMixWorker : public QThread {
    Q_OBJECT
public:
    explicit AudioMixWorker(QObject* parent = nullptr);
    void prepare(const AudioMixTask& task);

signals:
    void progressed(int percent);       // 0–100
    void finished(bool ok);
    void errorOccurred(const QString& msg);

protected:
    void run() override;

private:
    bool execMixAudio(const QString& tempAacPath);
    bool execMuxFinal(const QString& tempAacPath);
    QString buildFilterStr() const;     // 构建 amix+adelay 滤镜字符串
    // 从 sink 取帧并编码写入输出，返回编码帧数
    int drainAndEncode(AVFilterContext* sink, AVCodecContext* enc,
                       AVFormatContext* out, AVStream* stream,
                       AVFrame* filt, AVPacket* encPkt, int64_t& pts);

    AudioMixTask task_;
};
