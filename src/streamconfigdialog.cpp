#include "streamconfigdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFileDialog>
#include <QSettings>
#include <QCoreApplication>
#include <QNetworkInterface>
#include <QClipboard>
#include <QApplication>

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
    auto* srtLayout = new QFormLayout(srtGroup_);
    srtPortSpin_ = new QSpinBox(srtGroup_);
    srtPortSpin_->setRange(1024, 65535);
    srtPortSpin_->setValue(9000);
    srtLayout->addRow("监听端口:", srtPortSpin_);

    // 获取局域网 IP，显示完整拉流地址供用户复制
    QString lanIp = "未知";
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        for (const auto& entry : iface.addressEntries()) {
            QString ip = entry.ip().toString();
            if (ip.startsWith("192.168.") || ip.startsWith("10.") || ip.startsWith("172.")) {
                lanIp = ip; break;
            }
        }
        if (lanIp != "未知") break;
    }
    srtHintLabel_ = new QLabel(srtGroup_);
    srtHintLabel_->setText(QString("srt://%1:%2").arg(lanIp).arg(srtPortSpin_->value()));
    srtHintLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    srtHintLabel_->setStyleSheet("color: #888; font-size: 12px;");

    auto* srtHintRow = new QHBoxLayout;
    srtHintRow->addWidget(new QLabel("VLC 拉流地址:"));
    srtHintRow->addWidget(srtHintLabel_);
    auto* copyBtn = new QPushButton("复制", srtGroup_);
    copyBtn->setFixedWidth(48);
    connect(copyBtn, &QPushButton::clicked, this, [this]{
        QApplication::clipboard()->setText(srtHintLabel_->text());
    });
    srtHintRow->addWidget(copyBtn);
    srtHintRow->addStretch();
    srtLayout->addRow(srtHintRow);

    // 端口变化时同步更新提示
    connect(srtPortSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, lanIp](int port){
        srtHintLabel_->setText(QString("srt://%1:%2").arg(lanIp).arg(port));
    });

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

    // ---- 局域网浏览器 (HTTP-FLV) ----
    httpFlvGroup_ = new QGroupBox("局域网浏览器 (HTTP-FLV，无需安装 App)", this);
    httpFlvGroup_->setCheckable(true);
    httpFlvGroup_->setChecked(false);
    auto* httpLayout = new QFormLayout(httpFlvGroup_);
    httpFlvPortSpin_ = new QSpinBox(httpFlvGroup_);
    httpFlvPortSpin_->setRange(1024, 65535);
    httpFlvPortSpin_->setValue(8080);
    httpLayout->addRow("端口:", httpFlvPortSpin_);

    httpFlvHintLabel_ = new QLabel(httpFlvGroup_);
    httpFlvHintLabel_->setText(QString("http://%1:%2/player.html").arg(lanIp).arg(httpFlvPortSpin_->value()));
    httpFlvHintLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    httpFlvHintLabel_->setStyleSheet("color: #888; font-size: 12px;");

    auto* httpHintRow = new QHBoxLayout;
    httpHintRow->addWidget(new QLabel("浏览器打开:"));
    httpHintRow->addWidget(httpFlvHintLabel_);
    auto* httpCopyBtn = new QPushButton("复制", httpFlvGroup_);
    httpCopyBtn->setFixedWidth(48);
    connect(httpCopyBtn, &QPushButton::clicked, this, [this]{
        QApplication::clipboard()->setText(httpFlvHintLabel_->text());
    });
    httpHintRow->addWidget(httpCopyBtn);
    httpHintRow->addStretch();
    httpLayout->addRow(httpHintRow);

    httpLayout->addRow(new QLabel(
        "<span style='color:#888;font-size:11px;'>"
        "需将 flv.min.js 放到 exe 同目录（可从 SRS 安装目录 www/players/ 复制）"
        "</span>"));

    connect(httpFlvPortSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, lanIp](int p){
        httpFlvHintLabel_->setText(QString("http://%1:%2/player.html").arg(lanIp).arg(p));
    });

    root->addWidget(httpFlvGroup_);

    // ---- 低延迟浏览器 (HTTP-MPEG-TS) ----
    mpegTsGroup_ = new QGroupBox("低延迟浏览器 (HTTP-MPEG-TS，≤600ms，GPU 重编码)", this);
    mpegTsGroup_->setCheckable(true);
    mpegTsGroup_->setChecked(false);
    auto* mpegTsLayout = new QFormLayout(mpegTsGroup_);

    mpegTsPortSpin_ = new QSpinBox(mpegTsGroup_);
    mpegTsPortSpin_->setRange(1024, 65535);
    mpegTsPortSpin_->setValue(8090);
    mpegTsLayout->addRow("端口:", mpegTsPortSpin_);

    mpegTsGopSpin_ = new QDoubleSpinBox(mpegTsGroup_);
    mpegTsGopSpin_->setRange(0.1, 5.0);
    mpegTsGopSpin_->setSingleStep(0.1);
    mpegTsGopSpin_->setDecimals(1);
    mpegTsGopSpin_->setValue(0.5);
    mpegTsGopSpin_->setSuffix(" 秒");
    mpegTsLayout->addRow("GOP 时长:", mpegTsGopSpin_);

    mpegTsBitrateCombo_ = new QComboBox(mpegTsGroup_);
    mpegTsBitrateCombo_->addItem("1 Mbps",  1000000);
    mpegTsBitrateCombo_->addItem("2 Mbps",  2000000);
    mpegTsBitrateCombo_->addItem("4 Mbps",  4000000);
    mpegTsBitrateCombo_->addItem("8 Mbps",  8000000);
    mpegTsBitrateCombo_->setCurrentIndex(1);  // 默认 2 Mbps
    mpegTsLayout->addRow("视频码率:", mpegTsBitrateCombo_);

    mpegTsHintLabel_ = new QLabel(mpegTsGroup_);
    mpegTsHintLabel_->setText(QString("http://%1:%2/player.html").arg(lanIp).arg(mpegTsPortSpin_->value()));
    mpegTsHintLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mpegTsHintLabel_->setStyleSheet("color: #888; font-size: 12px;");

    auto* mpegTsHintRow = new QHBoxLayout;
    mpegTsHintRow->addWidget(new QLabel("浏览器打开:"));
    mpegTsHintRow->addWidget(mpegTsHintLabel_);
    auto* mpegTsCopyBtn = new QPushButton("复制", mpegTsGroup_);
    mpegTsCopyBtn->setFixedWidth(48);
    connect(mpegTsCopyBtn, &QPushButton::clicked, this, [this]{
        QApplication::clipboard()->setText(mpegTsHintLabel_->text());
    });
    mpegTsHintRow->addWidget(mpegTsCopyBtn);
    mpegTsHintRow->addStretch();
    mpegTsLayout->addRow(mpegTsHintRow);

    mpegTsLayout->addRow(new QLabel(
        "<span style='color:#888;font-size:11px;'>"
        "需将 mpegts.min.js 放到 exe 同目录（从 mpegts.js 官方发布包获取）"
        "</span>"));

    connect(mpegTsPortSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, lanIp](int p){
        mpegTsHintLabel_->setText(QString("http://%1:%2/player.html").arg(lanIp).arg(p));
    });

    root->addWidget(mpegTsGroup_);

    // ---- 按钮 ----
    btnBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    btnBox_->button(QDialogButtonBox::Ok)->setText("开始推流");
    root->addWidget(btnBox_);

    // 连接信号
    connect(rtmpGroup_,    &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(srtGroup_,     &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(localGroup_,   &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(httpFlvGroup_, &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
    connect(mpegTsGroup_,  &QGroupBox::toggled, this, &StreamConfigDialog::updateOkButton);
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
    bool any = rtmpGroup_->isChecked()  || srtGroup_->isChecked()
            || localGroup_->isChecked() || httpFlvGroup_->isChecked()
            || mpegTsGroup_->isChecked();
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
    httpFlvGroup_->setChecked(s.value("stream/httpFlvEnabled", false).toBool());
    httpFlvPortSpin_->setValue(s.value("stream/httpFlvPort", 8080).toInt());
    mpegTsGroup_->setChecked(s.value("stream/mpegTsEnabled", false).toBool());
    mpegTsPortSpin_->setValue(s.value("stream/mpegTsPort", 8090).toInt());
    mpegTsGopSpin_->setValue(s.value("stream/mpegTsGop", 0.5).toDouble());
    int bitrateIdx = mpegTsBitrateCombo_->findData(s.value("stream/mpegTsBitrate", 2000000).toInt());
    if (bitrateIdx >= 0) mpegTsBitrateCombo_->setCurrentIndex(bitrateIdx);
}

void StreamConfigDialog::saveSettings() {
    QSettings s("Rambos", "RambosPlayer");
    s.setValue("stream/rtmpEnabled",    rtmpGroup_->isChecked());
    s.setValue("stream/rtmpUrl",        rtmpUrlEdit_->text());
    s.setValue("stream/srtEnabled",     srtGroup_->isChecked());
    s.setValue("stream/srtPort",        srtPortSpin_->value());
    s.setValue("stream/localEnabled",   localGroup_->isChecked());
    s.setValue("stream/localFile",      localFileEdit_->text());
    s.setValue("stream/httpFlvEnabled", httpFlvGroup_->isChecked());
    s.setValue("stream/httpFlvPort",    httpFlvPortSpin_->value());
    s.setValue("stream/mpegTsEnabled",  mpegTsGroup_->isChecked());
    s.setValue("stream/mpegTsPort",     mpegTsPortSpin_->value());
    s.setValue("stream/mpegTsGop",      mpegTsGopSpin_->value());
    s.setValue("stream/mpegTsBitrate",  mpegTsBitrateCombo_->currentData().toInt());
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
    if (httpFlvGroup_->isChecked()) {
        StreamDestination d;
        d.type = StreamDestination::HttpFlv;
        d.port = static_cast<quint16>(httpFlvPortSpin_->value());
        list << d;
    }
    if (mpegTsGroup_->isChecked()) {
        StreamDestination d;
        d.type       = StreamDestination::HttpMpegTs;
        d.port       = static_cast<quint16>(mpegTsPortSpin_->value());
        d.gopSeconds = mpegTsGopSpin_->value();
        d.bitrate    = mpegTsBitrateCombo_->currentData().toInt();
        // fps 在 mainwindow 中根据实际视频帧率填充
        list << d;
    }
    return list;
}
