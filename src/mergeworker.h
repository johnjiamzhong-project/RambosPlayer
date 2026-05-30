#pragma once
#include <QThread>
#include <QStringList>
#include <QVector>
#include <QString>

// MergeWorker：合并任务统一调度线程。
// 根据 MergeTask 的模式分发到对应实现类：
//   ConcatVideo → 先检查参数一致性，一致走 ConcatDemuxer（无损），不一致走 ConcatFilter（重编码）
//   MixAudio    → AudioMixer（amix 多路混音）
//   MuxAV       → SimpleMuxer（独立视频 + 独立音频合流）
class MergeWorker : public QThread {
    Q_OBJECT
public:
    enum class Mode { ConcatVideo, MixAudio, MuxAV };

    struct Task {
        Mode        mode;
        QStringList inputFiles;   // ConcatVideo/MixAudio: N 路；MuxAV: [0]=视频, [1]=音频
        QString     outputFile;
        QVector<double> volumes;  // MixAudio 各路音量权重（不足时补 1.0）
    };

    explicit MergeWorker(QObject* parent = nullptr);
    ~MergeWorker() override;

    // 设置任务并启动（不可在运行中调用）
    void prepare(const Task& task);

signals:
    void progressed(int percent);           // 0–100
    void mergeFinished(bool ok);
    void errorOccurred(const QString& msg);

protected:
    void run() override;

private:
    Task task_;
};
