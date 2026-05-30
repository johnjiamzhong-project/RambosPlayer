#pragma once
#include <QObject>
#include <QStringList>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
}

// ConcatDemuxer：同参数视频无损拼接（concat demuxer + -c copy）。
// checkCompatible 先验证所有输入的编解码参数一致，
// exec 将文件路径写为 ffconcat 格式临时文件，以 concat demuxer 打开后直通写出。
// 参数不一致时 checkCompatible 返回 false，调用方应回退到 ConcatFilter 重编码。
class ConcatDemuxer : public QObject {
    Q_OBJECT
public:
    explicit ConcatDemuxer(QObject* parent = nullptr);

    // 检查所有输入文件的视频/音频 codec、分辨率、采样率是否完全一致
    bool checkCompatible(const QStringList& inputs) const;

    // 执行拼接，写入 output，返回成功与否
    bool exec(const QStringList& inputs, const QString& output);

signals:
    void progressed(int percent);       // 0–100
    void errorOccurred(const QString& msg);
};
