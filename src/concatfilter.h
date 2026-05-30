#pragma once
#include <QObject>
#include <QStringList>
#include <QString>

// ConcatFilter：异参数视频重编码拼接。
// 对所有输入文件顺序解码，视频帧缩放到第一文件的目标分辨率（保持宽高比+黑边填充），
// 以累积 PTS 送入 H.264（nvenc→libx264 回退）+ AAC 编码器写出连续视频。
// 参数一致的场景应优先由 ConcatDemuxer 处理；此类仅在参数不一致时使用。
class ConcatFilter : public QObject {
    Q_OBJECT
public:
    explicit ConcatFilter(QObject* parent = nullptr);

    bool exec(const QStringList& inputs, const QString& output);

signals:
    void progressed(int percent);       // 0–100
    void errorOccurred(const QString& msg);
};
