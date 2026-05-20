// StreamConfigDialog：推流配置对话框
// 三个可勾选场景（直播平台/局域网设备/本地录制），单选或多选，
// 至少勾一项才能点确认。配置通过 QSettings 持久化。
#pragma once
#include <QDialog>
#include "streamcontroller.h"

class QGroupBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QDialogButtonBox;

class StreamConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamConfigDialog(QWidget* parent = nullptr);

    // 返回用户勾选的目标列表（1–3 项）
    QList<StreamDestination> destinations() const;

private slots:
    void onBrowse();
    void updateOkButton();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();

    QGroupBox*       rtmpGroup_;     // 直播平台 (RTMP)
    QLineEdit*       rtmpUrlEdit_;

    QGroupBox*       srtGroup_;      // 局域网设备 (SRT)
    QSpinBox*        srtPortSpin_;
    QLabel*          srtHintLabel_;  // 显示 srt://IP:端口 供用户复制

    QGroupBox*       localGroup_;    // 本地录制
    QLineEdit*       localFileEdit_;
    QPushButton*     browseBtn_;

    QDialogButtonBox* btnBox_;
};
