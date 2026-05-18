#include "streamconfigdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFileDialog>
#include <QSettings>
#include <QCoreApplication>

StreamConfigDialog::StreamConfigDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("推流配置");
    setMinimumWidth(460);
    buildUi();
    loadSettings();
    updateOkButton();
}

void StreamConfigDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // ---- 直播平台 (RTMP) ----
    rtmpGroup_ = new QGroupBox("直播平台 (RTMP)", this);
    rtmpGroup_->setCheckable(true);
    rtmpGroup_->setChecked(false);
    auto* rtmpLayout = new QFormLayout(rtmpGroup_);
    rtmpUrlEdit_ = new QLineEdit(rtmpGroup_);
    rtmpUrlEdit_->setPlaceholderText("rtmp://a.rtmp.twitch.tv/live/直播码");
    rtmpLayout->addRow("推流地址:", rtmpUrlEdit_);
    root->addWidget(rtmpGroup_);

    // ---- 局域网设备 (SRT) ----
    srtGroup_ = new QGroupBox("局域网设备 (SRT)", this);
    srtGroup_->setCheckable(true);
    srtGroup_->setChecked(false);
    auto* srtLayout = new QHBoxLayout(srtGroup_);
    srtLayout->addWidget(new QLabel("监听端口:"));
    srtPortSpin_ = new QSpinBox(srtGroup_);
    srtPortSpin_->setRange(1024, 65535);
    srtPortSpin_->setValue(9000);
    srtLayout->addWidget(srtPortSpin_);
    srtLayout->addWidget(new QLabel("（平板/手机 VLC 输入 srt://本机IP:端口）"));
    srtLayout->addStretch();
    root->addWidget(srtGroup_);

    // ---- 本地录制 ----
    localGroup_ = new QGroupBox("本地录制", this);
    localGroup_->setCheckable(true);
    localGroup_->setChecked(false);
    auto* localLayout = new QHBoxLayout(localGroup_);
    localFileEdit_ = new QLineEdit(localGroup_);
    localFileEdit_->setPlaceholderText("选择保存路径...");
    browseBtn_ = new QPushButton("浏览...", localGroup_);
    browseBtn_->setFixedWidth(64);
    localLayout->addWidget(localFileEdit_);
    localLayout->addWidget(browseBtn_);
    root->addWidget(localGroup_);

    // ---- 按钮 ----
    btnBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    btnBox_->button(QDialogButtonBox::Ok)->setText("开始推流");
    root->addWidget(btnBox_);

    // 连接信号
    connect(rtmpGroup_,  &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(srtGroup_,   &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(localGroup_, &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(browseBtn_,  &QPushButton::clicked, this, &StreamConfigDialog::onBrowse);
    connect(btnBox_, &QDialogButtonBox::accepted, this, [this]{ saveSettings(); accept(); });
    connect(btnBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void StreamConfigDialog::onBrowse() {
    QString path = QFileDialog::getSaveFileName(
        this, "选择录制文件",
        localFileEdit_->text().isEmpty()
            ? QCoreApplication::applicationDirPath() + "/record.flv"
            : localFileEdit_->text(),
        "FLV 文件 (*.flv)");
    if (!path.isEmpty()) localFileEdit_->setText(path);
}

void StreamConfigDialog::updateOkButton() {
    bool any = rtmpGroup_->isChecked() || srtGroup_->isChecked() || localGroup_->isChecked();
    btnBox_->button(QDialogButtonBox::Ok)->setEnabled(any);
}

void StreamConfigDialog::loadSettings() {
    QSettings s("Rambos", "RambosPlayer");
    rtmpGroup_->setChecked(s.value("stream/rtmpEnabled", false).toBool());
    rtmpUrlEdit_->setText(s.value("stream/rtmpUrl", "").toString());
    srtGroup_->setChecked(s.value("stream/srtEnabled", false).toBool());
    srtPortSpin_->setValue(s.value("stream/srtPort", 9000).toInt());
    localGroup_->setChecked(s.value("stream/localEnabled", false).toBool());
    localFileEdit_->setText(s.value("stream/localFile", "").toString());
}

void StreamConfigDialog::saveSettings() {
    QSettings s("Rambos", "RambosPlayer");
    s.setValue("stream/rtmpEnabled",  rtmpGroup_->isChecked());
    s.setValue("stream/rtmpUrl",      rtmpUrlEdit_->text());
    s.setValue("stream/srtEnabled",   srtGroup_->isChecked());
    s.setValue("stream/srtPort",      srtPortSpin_->value());
    s.setValue("stream/localEnabled", localGroup_->isChecked());
    s.setValue("stream/localFile",    localFileEdit_->text());
}

QList<StreamDestination> StreamConfigDialog::destinations() const {
    QList<StreamDestination> list;

    if (rtmpGroup_->isChecked() && !rtmpUrlEdit_->text().trimmed().isEmpty()) {
        StreamDestination d;
        d.type = StreamDestination::Rtmp;
        d.url  = rtmpUrlEdit_->text().trimmed();
        list << d;
    }
    if (srtGroup_->isChecked()) {
        StreamDestination d;
        d.type = StreamDestination::Srt;
        d.url  = QString("srt://:%1").arg(srtPortSpin_->value());
        list << d;
    }
    if (localGroup_->isChecked()) {
        QString path = localFileEdit_->text().trimmed();
        if (path.isEmpty())
            path = QCoreApplication::applicationDirPath() + "/record.flv";
        StreamDestination d;
        d.type = StreamDestination::LocalFile;
        d.url  = path;
        list << d;
    }
    return list;
}
