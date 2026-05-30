#pragma once
#include <QObject>
#include <QString>

// SimpleMuxer：音视频混流（独立视频文件 + 独立音频文件 → 合并为一个文件）。
// 分别读取视频输入的视频流和音频输入的音频流，以 -c copy 交错写入输出容器。
// 若视频文件已含音轨，替换为音频输入文件的音轨（丢弃视频文件自身的音频包）。
class SimpleMuxer : public QObject {
    Q_OBJECT
public:
    explicit SimpleMuxer(QObject* parent = nullptr);

    // videoFile: 提供视频流的文件
    // audioFile: 提供音频流的文件
    // output:    输出路径
    bool exec(const QString& videoFile,
              const QString& audioFile,
              const QString& output);

signals:
    void progressed(int percent);       // 0–100
    void errorOccurred(const QString& msg);
};
