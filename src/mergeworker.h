#pragma once
#include <QThread>
#include <QStringList>
#include <QVector>
#include <QString>

// MergeWorker：合并任务统一调度线程。
// 根据 MergeTask 的模式分发到对应实现类：
//   ConcatVideo → 先检查参数一致性，一致走 ConcatDemuxer（无损），不一致走 ConcatFilter（重编码）
//   MixAudio    → 先检查参数一致性，一致走 ConcatDemuxer（无损），不一致走 execAudioConcat（重编码 AAC）
class MergeWorker : public QThread {
    Q_OBJECT
public:
    enum class Mode { ConcatVideo, MixAudio };

    struct Task {
        Mode        mode;
        QStringList inputFiles;
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
