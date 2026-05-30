#pragma once
#include <QObject>
#include <QStringList>
#include <QVector>
#include <QString>

// AudioMixer：N 路音频混音（amix avfilter + AAC 重编码）。
// 每路音频文件可指定独立音量权重（1.0 = 原始音量）；
// 输出时长策略为 longest（取各路中最长的）。
class AudioMixer : public QObject {
    Q_OBJECT
public:
    explicit AudioMixer(QObject* parent = nullptr);

    // inputs:  N 个音频输入文件路径
    // volumes: 各路音量权重（长度与 inputs 一致，不足时补 1.0）
    // output:  输出路径
    bool exec(const QStringList& inputs,
              const QVector<double>& volumes,
              const QString& output);

signals:
    void progressed(int percent);       // 0–100
    void errorOccurred(const QString& msg);
};
