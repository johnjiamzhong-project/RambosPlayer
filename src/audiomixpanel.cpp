#include "audiomixpanel.h"
#include "audiomixworker.h"
#include "audiopreviewwindow.h"
#include "playercontroller.h"
#include "ui_audiomixpanel.h"
#include "logger.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QAudioInput>
#include <QAudioDeviceInfo>
#include <QSet>
#include <cmath>
#include <QMediaPlayer>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QDataStream>
#include <QTime>

extern "C" {
#include <libavformat/avformat.h>
}

AudioMixPanel::AudioMixPanel(QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AudioMixPanel)
{
    ui->setupUi(this);

    worker_ = new AudioMixWorker(this);
    connect(worker_, &AudioMixWorker::progressed,    this, &AudioMixPanel::onProgressed);
    connect(worker_, &AudioMixWorker::finished,      this, &AudioMixPanel::onWorkFinished);
    connect(worker_, &AudioMixWorker::errorOccurred, this, &AudioMixPanel::onError);

    connect(ui->openSourceBtn,   &QPushButton::clicked, this, &AudioMixPanel::onOpenSource);
    connect(ui->readAudioBtn,    &QPushButton::clicked, this, &AudioMixPanel::onReadAudioFolder);
    connect(ui->srcVolSlider,    &QSlider::valueChanged, this, &AudioMixPanel::onSrcVolChanged);
    connect(ui->mixVolSlider,    &QSlider::valueChanged, this, &AudioMixPanel::onMixVolChanged);
    connect(ui->addRegionBtn,       &QPushButton::clicked, this, &AudioMixPanel::onAddRegion);
    connect(ui->selectRegionAllBtn, &QPushButton::clicked, this, &AudioMixPanel::onSelectRegionAll);
    connect(ui->removeRegionBtn,    &QPushButton::clicked, this, &AudioMixPanel::onRemoveRegion);
    connect(ui->listenBtn,       &QPushButton::clicked, this, &AudioMixPanel::onListen);
    connect(ui->previewBtn,      &QPushButton::clicked, this, &AudioMixPanel::onPreview);
    connect(ui->browseOutputBtn, &QPushButton::clicked, this, &AudioMixPanel::onBrowseOutput);
    connect(ui->exportBtn,       &QPushButton::clicked, this, &AudioMixPanel::onExport);
    connect(ui->recStartStopBtn, &QPushButton::clicked, this, &AudioMixPanel::onRecStartStop);

    // 模式切换按钮互斥
    connect(ui->switchLocalBtn, &QPushButton::clicked, this, [this]{
        ui->switchLocalBtn->setChecked(true);
        ui->switchRecordBtn->setChecked(false);
        ui->addStack->setCurrentIndex(0);
    });
    connect(ui->switchRecordBtn, &QPushButton::clicked, this, [this]{
        ui->switchRecordBtn->setChecked(true);
        ui->switchLocalBtn->setChecked(false);
        ui->addStack->setCurrentIndex(1);
    });

    // 数值框变化时实时回写选中行（仅编辑模式下有效）
    connect(ui->videoStartMinSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AudioMixPanel::onUpdateSelectedRegion);
    connect(ui->videoStartSecSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AudioMixPanel::onUpdateSelectedRegion);
    connect(ui->audioDurMinSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AudioMixPanel::onUpdateSelectedRegion);
    connect(ui->audioDurSecSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AudioMixPanel::onUpdateSelectedRegion);

    // 列表选中时启用相关按钮，并将参数填入编辑控件
    connect(ui->regionsTable, &QTableWidget::itemSelectionChanged, this, [this]{
        bool sel = !ui->regionsTable->selectedItems().isEmpty();
        ui->removeRegionBtn->setEnabled(sel);
        ui->listenBtn->setEnabled(sel);
        // 试播放仅在当前行已勾选时可用
        int row = ui->regionsTable->currentRow();
        bool rowActive = sel && row >= 0 && row < regions_.size() && regions_[row].active;
        ui->previewBtn->setEnabled(rowActive && player_ != nullptr);

        if (sel && row >= 0 && row < regions_.size()) {
            loadRegionIntoControls(row);
        } else {
            editingRow_ = -1;
            ui->addRegionBtn->setText("加入列表");
        }
    });

    // 复选框状态变化时：同步回 regions_.active，并更新试播放按钮启用状态
    connect(ui->regionsTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != 0) return;
        int row = item->row();
        if (row < 0 || row >= regions_.size()) return;
        regions_[row].active = (item->checkState() == Qt::Checked);
        int curRow = ui->regionsTable->currentRow();
        if (curRow == row) {
            bool sel = !ui->regionsTable->selectedItems().isEmpty();
            ui->previewBtn->setEnabled(sel && player_ != nullptr && regions_[row].active);
        }
    });

    // 表格列宽：col0=复选框（固定），col1=音频文件（拉伸），col2-5=数值（交互）
    ui->regionsTable->horizontalHeader()->setStretchLastSection(false);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    ui->regionsTable->setColumnWidth(0, 28);   // 复选框
    ui->regionsTable->setColumnWidth(2, 75);   // 贴入时间（HH:mm:ss）
    ui->regionsTable->setColumnWidth(3, 75);   // 时长
    ui->regionsTable->setColumnWidth(4, 60);   // 源音量
    ui->regionsTable->setColumnWidth(5, 60);   // 混音量
    // 鼠标追踪：启用后 hover 样式（整行高亮）才能生效
    ui->regionsTable->setMouseTracking(true);

    // 初始填充录音设备列表（按名称去重，Windows 下 WASAPI/DirectSound 会返回重复项）
    refreshInputDevices();
}

AudioMixPanel::~AudioMixPanel()
{
    stopPreview();
    if (audioInput_) { audioInput_->stop(); delete audioInput_; }
    if (recordLevelTimer_) { recordLevelTimer_->stop(); delete recordLevelTimer_; }
    if (recordMaxTimer_)   { recordMaxTimer_->stop();   delete recordMaxTimer_; }
    if (worker_->isRunning()) { worker_->requestInterruption(); worker_->wait(3000); }
    // 清理录音临时文件
    for (const auto& r : regions_) {
        if (r.isRecorded) QFile::remove(r.sourcePath);
    }
    delete ui;
}

// 刷新录音设备列表：按 deviceName() 去重后填充 combo 并缓存到 inputDevices_
void AudioMixPanel::refreshInputDevices()
{
    ui->recDeviceCombo->clear();
    inputDevices_.clear();
    QSet<QString> seen;
    for (const auto& d : QAudioDeviceInfo::availableDevices(QAudio::AudioInput)) {
        if (seen.contains(d.deviceName())) continue;
        seen.insert(d.deviceName());
        inputDevices_.append(d);
        ui->recDeviceCombo->addItem(d.deviceName());
    }
    if (ui->recDeviceCombo->count() == 0)
        ui->recDeviceCombo->addItem(tr("（无可用输入设备）"));
}

void AudioMixPanel::setPlayerController(PlayerController* pc)
{
    player_ = pc;
    // 外部 pause 时自动停止预览（如进度条拖动、MainWindow 暂停等）
    if (player_) {
        connect(player_, &PlayerController::playingChanged, this, [this](bool playing) {
            if (!playing && previewPlayer_) stopPreview();
        });
    }
}

// 由 MainWindow 在文件打开后自动调用，预填源视频路径并生成默认输出路径
void AudioMixPanel::setSourceFile(const QString& path)
{
    sourceFile_ = path;
    ui->sourcePathEdit->setText(path);

    // 默认输出路径：同目录，文件名加 _mixed 后缀
    QFileInfo fi(path);
    ui->outputEdit->setText(
        fi.dir().filePath(fi.completeBaseName() + "_mixed.mp4"));
    updateExportEnabled();
}

void AudioMixPanel::switchToLocalMode()
{
    ui->switchLocalBtn->setChecked(true);
    ui->switchRecordBtn->setChecked(false);
    ui->addStack->setCurrentIndex(0);
}

void AudioMixPanel::switchToRecordMode()
{
    ui->switchRecordBtn->setChecked(true);
    ui->switchLocalBtn->setChecked(false);
    ui->addStack->setCurrentIndex(1);

    // 切换时刷新麦克风列表（热插拔场景下保持最新）
    refreshInputDevices();
}

// 手动选择源视频（不依赖播放器已打开文件）
void AudioMixPanel::onOpenSource()
{
    QString path = QFileDialog::getOpenFileName(this, "选择源视频", QString(),
        "视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)");
    if (path.isEmpty()) return;
    setSourceFile(path);
    emit sourceFileSelected(path);  // 通知 MainWindow 加载到播放器
}

// 扫描用户选择的文件夹，将其中的音频文件填充到下拉列表
void AudioMixPanel::onReadAudioFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, "选择音频文件夹",
        lastAudioDir_.isEmpty() ? QString() : lastAudioDir_);
    if (dir.isEmpty()) return;

    lastAudioDir_ = dir;

    static const QStringList exts = { "mp3", "aac", "wav", "flac", "ogg", "m4a", "wma", "opus" };
    QStringList nameFilters;
    for (const auto& e : exts) nameFilters << "*." + e;

    QDir d(dir);
    QFileInfoList files = d.entryInfoList(nameFilters, QDir::Files, QDir::Name);

    if (files.isEmpty()) {
        QMessageBox::information(this, "未找到音频",
            QString("文件夹中未找到音频文件：\n%1").arg(dir));
        return;
    }

    ui->audioFileCombo->clear();
    for (const QFileInfo& fi : files) {
        // 显示文件名，UserRole 存完整路径
        ui->audioFileCombo->addItem(fi.fileName(), fi.absoluteFilePath());
    }
    ui->audioFileCombo->setCurrentIndex(0);

    qInfo() << "AudioMixPanel: 读取文件夹" << dir << "共" << files.size() << "个音频文件";
}

// srcVol + mixVol = 100%，两个 Slider 互补联动
void AudioMixPanel::onSrcVolChanged(int value)
{
    ui->srcVolLabel->setText(QString("源音频 %1%").arg(value));
    int mixVal = 100 - value;
    ui->mixVolSlider->blockSignals(true);
    ui->mixVolSlider->setValue(mixVal);
    ui->mixVolSlider->blockSignals(false);
    ui->mixVolLabel->setText(QString("新增音频 %1%").arg(mixVal));
    qInfo() << "AudioMixPanel: 滑块调整 srcVol=" << value << "mixVol=" << mixVal;
    onUpdateSelectedRegion();
}

void AudioMixPanel::onMixVolChanged(int value)
{
    ui->mixVolLabel->setText(QString("新增音频 %1%").arg(value));
    int srcVal = 100 - value;
    ui->srcVolSlider->blockSignals(true);
    ui->srcVolSlider->setValue(srcVal);
    ui->srcVolSlider->blockSignals(false);
    ui->srcVolLabel->setText(QString("源音频 %1%").arg(srcVal));
    qInfo() << "AudioMixPanel: 滑块调整 srcVol=" << srcVal << "mixVol=" << value;
    onUpdateSelectedRegion();
}

// 将本地音频添加到区间列表，或在编辑模式下确认更新并退出编辑
void AudioMixPanel::onAddRegion()
{
    // 编辑模式：参数已实时回写，点按钮只需退出编辑状态
    if (editingRow_ >= 0) {
        ui->regionsTable->clearSelection();
        editingRow_ = -1;
        ui->addRegionBtn->setText("加入列表");
        return;
    }

    if (ui->audioFileCombo->count() == 0) {
        QMessageBox::warning(this, "未读取音频", "请先点击[读取]按钮扫描文件夹。");
        return;
    }
    QString audioPath = ui->audioFileCombo->currentData().toString();
    if (audioPath.isEmpty())
        audioPath = ui->audioFileCombo->currentText();
    if (!QFileInfo::exists(audioPath)) {
        QMessageBox::warning(this, "文件不存在", "指定的音频文件不存在：\n" + audioPath);
        return;
    }

    AudioMixRegion r;
    r.sourcePath    = audioPath;
    r.displayName   = QFileInfo(audioPath).fileName();
    r.videoStartUs  = (int64_t)(ui->videoStartMinSpin->value() * 60 + ui->videoStartSecSpin->value()) * 1000000LL;
    r.audioOffsetUs = 0;
    r.srcVol        = ui->srcVolSlider->value() / 100.0f;
    r.mixVol        = ui->mixVolSlider->value() / 100.0f;
    r.isRecorded    = false;

    int userDurSec = ui->audioDurMinSpin->value() * 60 + ui->audioDurSecSpin->value();
    if (userDurSec > 0) {
        r.audioDurationUs = (int64_t)userDurSec * 1000000LL;
    } else {
        r.audioDurationUs = probeDurationUs(audioPath);
        if (r.audioDurationUs <= 0) {
            QMessageBox::warning(this, "无法读取时长", "无法读取音频文件时长，请手动输入使用时长。");
            return;
        }
    }

    qInfo() << "AudioMixPanel: 加入区间 ===";
    qInfo() << "  UI srcVolSlider:" << ui->srcVolSlider->value() << "mixVolSlider:" << ui->mixVolSlider->value();
    qInfo() << "  UI videoStart:" << ui->videoStartMinSpin->value() << "分" << ui->videoStartSecSpin->value() << "秒"
            << "audioDuration:" << ui->audioDurMinSpin->value() << "分" << ui->audioDurSecSpin->value() << "秒";
    qInfo() << "  存入 srcVol:" << r.srcVol << "mixVol:" << r.mixVol;
    qInfo() << "  存入 videoStart:" << r.videoStartUs / 1000000 << "s duration:" << r.audioDurationUs / 1000000 << "s";
    qInfo() << "AudioMixPanel: 加入区间 ===";
    addRegionToList(r);
}

void AudioMixPanel::onRemoveRegion()
{
    int row = ui->regionsTable->currentRow();
    if (row < 0 || row >= regions_.size()) return;
    const AudioMixRegion& r = regions_[row];
    if (r.isRecorded) QFile::remove(r.sourcePath);
    regions_.removeAt(row);
    rebuildTable();
    updateExportEnabled();
}

// 全选/取消全选：切换所有行的复选框状态（已全勾则全取消，否则全勾）
void AudioMixPanel::onSelectRegionAll()
{
    int rows = ui->regionsTable->rowCount();
    if (rows == 0) return;
    bool allChecked = true;
    for (int i = 0; i < rows; ++i) {
        if (ui->regionsTable->item(i, 0)->checkState() != Qt::Checked) {
            allChecked = false; break;
        }
    }
    Qt::CheckState newState = allChecked ? Qt::Unchecked : Qt::Checked;
    ui->regionsTable->blockSignals(true);
    for (int i = 0; i < rows; ++i) {
        ui->regionsTable->item(i, 0)->setCheckState(newState);
        if (i < regions_.size())
            regions_[i].active = (newState == Qt::Checked);
    }
    ui->regionsTable->blockSignals(false);
    // blockSignals 期间 itemChanged 被抑制，手动刷新试播放按钮状态
    int curRow = ui->regionsTable->currentRow();
    bool sel = !ui->regionsTable->selectedItems().isEmpty();
    bool rowActive = sel && curRow >= 0 && curRow < regions_.size() && regions_[curRow].active;
    ui->previewBtn->setEnabled(rowActive && player_ != nullptr);
}

// 试听：弹出简洁播放窗口预览选中的音频文件
void AudioMixPanel::onListen()
{
    int row = ui->regionsTable->currentRow();
    if (row < 0 || row >= regions_.size()) return;
    const AudioMixRegion& r = regions_[row];

    auto* win = new AudioPreviewWindow(r.sourcePath, this->window());
    win->setAttribute(Qt::WA_DeleteOnClose);
    // 居中显示在父窗口上方
    QPoint center = this->window()->geometry().center();
    win->move(center.x() - win->width() / 2, center.y() - win->height() / 2);
    win->show();
}

// 试播放：视频 seek 到区间起始，同时 QMediaPlayer 播放新增音频
// 再次按下则停止试播放；区间结束自动停止；外部 pause 自动清理
void AudioMixPanel::onPreview()
{
    if (previewPlayer_) {
        stopPreview();
        return;
    }

    int row = ui->regionsTable->currentRow();
    if (row < 0 || row >= regions_.size() || !player_) return;
    const AudioMixRegion& r = regions_[row];
    if (!r.active) return;  // 未勾选的区间不参与试播放

    // 使用区间存储值预览
    float curSrcVol = r.srcVol;
    float curMixVol = r.mixVol;

    // 只在无预览时保存原始音量，防止预览音量覆盖原始值
    if (!previewPlayer_ && !previewStopTimer_)
        previewSavedVolume_ = player_->volume();

    qInfo() << "AudioMixPanel: 试播放开始 ===";
    qInfo() << "  选中行:" << row << "文件:" << r.sourcePath;
    qInfo() << "  存储 srcVol:" << r.srcVol << "mixVol:" << r.mixVol;
    qInfo() << "  当前滑块 srcVol:" << curSrcVol << "mixVol:" << curMixVol;
    qInfo() << "  videoStartUs:" << r.videoStartUs << "(" << r.videoStartUs / 1000000 << "s)";
    qInfo() << "  audioDurationUs:" << r.audioDurationUs << "(" << r.audioDurationUs / 1000000 << "s)";
    qInfo() << "  previewPlayer 音量:" << qRound(curMixVol * 100);
    qInfo() << "AudioMixPanel: 试播放参数 ===";

    // clamp 到视频时长内，防止 seek 超出范围导致 EOF 无画面
    double seekSec = r.videoStartUs / 1e6;
    double videoDur = player_->duration() / 1000.0;
    if (videoDur > 0 && seekSec >= videoDur)
        seekSec = qMax(0.0, videoDur - 1.0);
    player_->seek(seekSec);
    player_->setVolume(curSrcVol);
    player_->play();

    previewPlayer_ = new QMediaPlayer(this);
    connect(previewPlayer_, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State state){
        qInfo() << "AudioMixPanel: previewPlayer state changed to" << state;
        // setPosition() 期间 Windows MF 后端会伪发 StoppedState；
        // 必须同时确认 EndOfMedia 才代表 mp3 真正播完。
        // previewPlayer_ 的 null 检查同时防止 p->stop() 触发的重入。
        if (state == QMediaPlayer::StoppedState && previewPlayer_
                && previewPlayer_->mediaStatus() == QMediaPlayer::EndOfMedia) {
            stopPreview();
        }
    });
    connect(previewPlayer_, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),
            this, [this](QMediaPlayer::Error err){
        qWarning() << "AudioMixPanel: previewPlayer error:" << err << previewPlayer_->errorString();
    });
    previewPlayer_->setMedia(QUrl::fromLocalFile(r.sourcePath));
    previewPlayer_->setVolume(qRound(curMixVol * 100));
    previewPlayer_->setPosition(r.audioOffsetUs / 1000LL);
    previewPlayer_->play();
    previewRegionStartUs_ = r.videoStartUs;
    previewRegionDurationUs_ = r.audioDurationUs;

    ui->previewBtn->setText("停止试播放");
    ui->previewBtn->setStyleSheet("QPushButton { background-color: #d32f2f; color: white; }");
    emit previewRegionChanged(r.videoStartUs, r.audioDurationUs);

    // 到区间结束时停止
    int playMs = (int)(r.audioDurationUs / 1000LL);
    if (playMs > 0) {
        previewStopTimer_ = new QTimer(this);
        previewStopTimer_->setSingleShot(true);
        connect(previewStopTimer_, &QTimer::timeout, this, [this]{ stopPreview(); });
        previewStopTimer_->start(playMs);
    }
}

void AudioMixPanel::onBrowseOutput()
{
    QString path = QFileDialog::getSaveFileName(this, "保存混合视频", ui->outputEdit->text(),
        "MP4 文件 (*.mp4);;所有文件 (*)");
    if (!path.isEmpty())
        ui->outputEdit->setText(path);
    updateExportEnabled();
}

void AudioMixPanel::onExport()
{
    if (sourceFile_.isEmpty()) {
        QMessageBox::warning(this, "未指定源视频", "请先选择源视频文件。");
        return;
    }
    // 收集已勾选的区间（通过复选框而非行高亮选中）
    QList<AudioMixRegion> selected;
    for (int i = 0; i < regions_.size(); ++i) {
        if (regions_[i].active)
            selected.append(regions_[i]);
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, "未勾选区间", "请先在列表中勾选要导出的区间。");
        return;
    }
    QString output = ui->outputEdit->text().trimmed();
    if (output.isEmpty()) {
        QMessageBox::warning(this, "未指定输出路径", "请指定输出文件路径。");
        return;
    }

    AudioMixTask task;
    task.originalVideoPath = sourceFile_;
    task.regions           = selected;
    task.outputPath        = output;

    qInfo() << "=== AudioMixPanel::onExport 开始 ===";
    qInfo() << "  源视频:" << task.originalVideoPath;
    qInfo() << "  输出路径:" << task.outputPath;
    qInfo() << "  区间数量:" << task.regions.size();
    for (int i = 0; i < task.regions.size(); ++i) {
        const auto& r = task.regions[i];
        qInfo() << "  区间[" << i << "] src=" << r.sourcePath
                 << "videoStart=" << r.videoStartUs / 1000000 << "s"
                 << "audioDur=" << r.audioDurationUs / 1000000 << "s"
                 << "audioOffset=" << r.audioOffsetUs / 1000000 << "s"
                 << "srcVol=" << r.srcVol << "mixVol=" << r.mixVol
                 << "isRecorded=" << r.isRecorded;
    }
    qInfo() << "=== AudioMixPanel::onExport 任务参数 ===";

    worker_->prepare(task);
    worker_->start();

    ui->exportBtn->setEnabled(false);
    ui->progressBar->setValue(0);
    ui->progressBar->setVisible(true);
}

// 开始/停止实时录入
void AudioMixPanel::onRecStartStop()
{
    if (!recording_) {
        if (!player_) {
            QMessageBox::warning(this, "未打开文件", "请先打开视频文件。");
            return;
        }

        recordVideoStartUs_ = (int64_t)(ui->recVideoStartMinSpin->value() * 60 + ui->recVideoStartSecSpin->value()) * 1000000LL;
        prevVolume_ = player_->volume();  // 保存录音前的实际音量

        player_->seek(recordVideoStartUs_ / 1e6);
        player_->setVolume(0.0f);   // 静音，防止麦克风反馈
        player_->play();

        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(2);
        fmt.setSampleSize(16);
        fmt.setSampleType(QAudioFormat::SignedInt);
        fmt.setByteOrder(QAudioFormat::LittleEndian);
        fmt.setCodec("audio/pcm");

        // 根据下拉框选取设备，fallback 到默认设备
        QAudioDeviceInfo devInfo = QAudioDeviceInfo::defaultInputDevice();
        {
            int idx = ui->recDeviceCombo->currentIndex();
            if (idx >= 0 && idx < inputDevices_.size())
                devInfo = inputDevices_[idx];
        }

        if (!devInfo.isFormatSupported(fmt)) {
            fmt = devInfo.nearestFormat(fmt);
            qWarning() << "AudioMixPanel: 录音格式不支持，使用近似格式:"
                       << fmt.sampleRate() << "Hz" << fmt.channelCount() << "ch" << fmt.sampleSize() << "bit";
            ui->recStatusLabel->setText(
                QString("注意：设备不支持标准格式，已切换为 %1Hz %2声道 %3bit，开始录制…")
                    .arg(fmt.sampleRate()).arg(fmt.channelCount()).arg(fmt.sampleSize()));
        }
        // 保存实际格式供电平计算使用
        recordSampleRate_ = fmt.sampleRate();
        recordChannels_   = fmt.channelCount();
        recordSampleBits_ = fmt.sampleSize();

        audioInput_ = new QAudioInput(devInfo, fmt, this);
        recordBuffer_.setData(QByteArray());
        recordBuffer_.open(QIODevice::WriteOnly);
        audioInput_->start(&recordBuffer_);

        qInfo() << "AudioMixPanel: 录音开始 device=" << devInfo.deviceName()
                << "format=" << fmt.sampleRate() << "Hz" << fmt.channelCount() << "ch" << fmt.sampleSize() << "bit"
                << "audioInput state=" << audioInput_->state()
                << "error=" << audioInput_->error();

        recordLevelLogCount_ = 0;   // 每次录音重置日志计数
        recording_ = true;
        ui->recStartStopBtn->setText("停止录入");
        if (ui->recStatusLabel->text().startsWith("注意"))
            ; // 保留 fallback 警告，不覆盖
        else
            ui->recStatusLabel->setText("录制中… 已录 0:00");
        ui->recDeviceCombo->setEnabled(false);
        ui->recLevelBar->setValue(0);

        // 安全清理旧定时器（防御性）
        if (recordLevelTimer_) { recordLevelTimer_->stop(); delete recordLevelTimer_; recordLevelTimer_ = nullptr; }
        if (recordMaxTimer_)   { recordMaxTimer_->stop();   delete recordMaxTimer_;   recordMaxTimer_   = nullptr; }

        // 每 50ms 刷新一次电平和已录时长（20fps，视觉流畅）
        recordLevelTimer_ = new QTimer(this);
        recordLevelTimer_->setInterval(50);
        connect(recordLevelTimer_, &QTimer::timeout, this, &AudioMixPanel::onRecordLevel);
        recordLevelTimer_->start();

        // 超过 30 分钟自动停止，防止内存无限累积（30min ≈ 300MB PCM）
        recordMaxTimer_ = new QTimer(this);
        recordMaxTimer_->setSingleShot(true);
        connect(recordMaxTimer_, &QTimer::timeout, this, [this]{
            qWarning() << "AudioMixPanel: 录音超过30分钟，自动停止";
            QMessageBox::warning(this, "录音超时", "录音已超过 30 分钟，已自动停止。");
            onRecStartStop();
        });
        recordMaxTimer_->start(30 * 60 * 1000);
    } else {
        // 停止录音
        qInfo() << "AudioMixPanel: 停止录音, buffer size=" << recordBuffer_.data().size();
        if (audioInput_) {
            audioInput_->stop();
            delete audioInput_;
            audioInput_ = nullptr;
        }
        // 清理录音定时器
        if (recordLevelTimer_) {
            recordLevelTimer_->stop();
            delete recordLevelTimer_;
            recordLevelTimer_ = nullptr;
        }
        if (recordMaxTimer_) {
            recordMaxTimer_->stop();
            delete recordMaxTimer_;
            recordMaxTimer_ = nullptr;
        }
        recordBuffer_.close();

        if (player_) {
            player_->pause();
            player_->setVolume(prevVolume_);
        }

        QByteArray pcmData = recordBuffer_.data();
        if (pcmData.isEmpty()) {
            QMessageBox::warning(this, "录入为空", "未捕获到音频数据，请重试。");
        } else {
            QString tempPath = QDir::temp().filePath(
                QString("rambos_rec_%1.wav").arg(QDateTime::currentMSecsSinceEpoch()));

            if (writeWav(tempPath, pcmData, recordSampleRate_, recordChannels_, recordSampleBits_)) {
                AudioMixRegion r;
                r.sourcePath     = tempPath;
                r.isRecorded     = true;
                r.videoStartUs   = recordVideoStartUs_;
                r.audioOffsetUs  = 0;
                // PCM 时长：先乘后除保留精度，避免整数截断；使用实际格式参数
                int64_t bytesPerSec = (int64_t)recordSampleRate_ * recordChannels_ * (recordSampleBits_ / 8);
                r.audioDurationUs = (bytesPerSec > 0)
                    ? pcmData.size() * 1000000LL / bytesPerSec : 0;
                r.displayName    = QString("录音 %1").arg(
                    QTime(0, 0).addSecs(recordVideoStartUs_ / 1000000LL).toString("HH:mm:ss"));
                r.srcVol  = ui->srcVolSlider->value() / 100.0f;
                r.mixVol  = ui->mixVolSlider->value() / 100.0f;
                addRegionToList(r);
                qInfo() << "AudioMixPanel: 录音保存至" << tempPath
                        << "时长" << r.audioDurationUs / 1000000 << "秒";
            } else {
                QMessageBox::warning(this, "保存失败", "录音临时文件写入失败。");
            }
        }

        ui->recLevelBar->setValue(0);
        ui->recDeviceCombo->setEnabled(true);

        recording_ = false;
        ui->recStartStopBtn->setText("开始录入");
        ui->recStatusLabel->setText(QString::fromUtf8("就绪：选择录入起始时间后点击\"开始录入\""));
    }
}

void AudioMixPanel::onProgressed(int percent)
{
    ui->progressBar->setValue(percent);
}

void AudioMixPanel::onWorkFinished(bool ok)
{
    ui->exportBtn->setEnabled(true);
    ui->progressBar->setValue(ok ? 100 : 0);
    if (ok) {
        QMessageBox::information(this, "导出完成",
            "音频混合导出完成！\n输出文件：" + ui->outputEdit->text() +
            "\n\n如需继续混合，可继续添加音频区间后再次导出（将从原始源重新混合）。");
    }
}

void AudioMixPanel::onError(const QString& msg)
{
    ui->exportBtn->setEnabled(true);
    ui->progressBar->setVisible(false);
    QMessageBox::critical(this, "混合失败", msg);
    qWarning() << "AudioMixPanel 错误:" << msg;
}

// ----- 私有辅助函数 -----

void AudioMixPanel::addRegionToList(const AudioMixRegion& r)
{
    regions_.append(r);
    rebuildTable();
    updateExportEnabled();
}

void AudioMixPanel::rebuildTable()
{
    ui->regionsTable->blockSignals(true);
    ui->regionsTable->setRowCount(0);
    for (const auto& r : regions_) {
        int row = ui->regionsTable->rowCount();
        ui->regionsTable->insertRow(row);

        auto makeItem = [](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };

        // col0: 复选框，状态从 region.active 恢复
        // 必须保留 ItemIsSelectable，否则点击 col0 只设置 currentIndex 不触发行高亮，
        // 与 alternatingRowColors 叠加后出现单双行样式不一致的视觉 bug。
        auto* checkItem = new QTableWidgetItem();
        checkItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        checkItem->setCheckState(r.active ? Qt::Checked : Qt::Unchecked);
        ui->regionsTable->setItem(row, 0, checkItem);

        // col1: 音频文件名（tooltip 显示完整路径）
        auto* nameItem = makeItem(r.displayName);
        nameItem->setToolTip(r.sourcePath);
        ui->regionsTable->setItem(row, 1, nameItem);

        // col2-5: 数值列（原 col1-4，整体 +1）
        ui->regionsTable->setItem(row, 2, makeItem(
            QTime(0, 0).addSecs(r.videoStartUs / 1000000LL).toString("HH:mm:ss")));
        ui->regionsTable->setItem(row, 3, makeItem(
            QTime(0, 0).addSecs(r.audioDurationUs / 1000000LL).toString("HH:mm:ss")));
        ui->regionsTable->setItem(row, 4, makeItem(
            QString("%1%").arg(qRound(r.srcVol * 100))));
        ui->regionsTable->setItem(row, 5, makeItem(
            QString("%1%").arg(qRound(r.mixVol * 100))));
    }
    ui->regionsTable->blockSignals(false);
}

void AudioMixPanel::updateExportEnabled()
{
    bool canExport = !sourceFile_.isEmpty()
                  && !regions_.isEmpty()
                  && !ui->outputEdit->text().trimmed().isEmpty()
                  && !worker_->isRunning();
    ui->exportBtn->setEnabled(canExport);
}

void AudioMixPanel::stopPreview()
{
    bool wasActive = (previewPlayer_ != nullptr) || (previewStopTimer_ != nullptr);
    qInfo() << "AudioMixPanel::stopPreview 开始 wasActive=" << wasActive;
    if (previewStopTimer_) {
        previewStopTimer_->stop();
        delete previewStopTimer_;
        previewStopTimer_ = nullptr;
    }
    if (previewPlayer_) {
        auto* p = previewPlayer_;
        previewPlayer_ = nullptr;       // 先置空，防止 stateChanged 回调重入
        qInfo() << "AudioMixPanel::stopPreview previewPlayer_->stop() 前 state=" << p->state();
        p->stop();
        qInfo() << "AudioMixPanel::stopPreview previewPlayer_->stop() 完成";
        p->deleteLater();
    }
    if (wasActive && player_) {
        player_->pause();
        player_->setVolume(previewSavedVolume_);
        qInfo() << "AudioMixPanel::stopPreview player 已暂停, volume=" << previewSavedVolume_;
    }
    ui->previewBtn->setText("试播放选中区间");
    ui->previewBtn->setStyleSheet("");
    previewRegionStartUs_ = 0;
    qInfo() << "AudioMixPanel::stopPreview 完成";
    previewRegionDurationUs_ = 0;
    emit previewRegionChanged(0, 0);
}

// 进度条拖拽时同步新增音频播放位置
void AudioMixPanel::seekPreviewAudio(double videoSeconds)
{
    if (!previewPlayer_ || previewRegionDurationUs_ <= 0) return;

    int64_t videoUs = (int64_t)(videoSeconds * 1000000.0);
    // 计算音频在区间内的相对位置
    int64_t audioPosUs = videoUs - previewRegionStartUs_;

    // 如果 seek 到区间之前，音频从头开始
    if (audioPosUs < 0) audioPosUs = 0;
    // 如果 seek 到区间之后，停止预览
    if (audioPosUs >= previewRegionDurationUs_) {
        stopPreview();
        return;
    }

    previewPlayer_->setPosition(audioPosUs / 1000LL);

    // 重置停止定时器：剩余时长 = 区间总时长 - 当前音频位置
    if (previewStopTimer_) {
        int64_t remainUs = previewRegionDurationUs_ - audioPosUs;
        previewStopTimer_->start((int)(remainUs / 1000LL));
    }
}

// 手写 44 字节 WAV 文件头，无需第三方库
bool AudioMixPanel::writeWav(const QString& path, const QByteArray& pcm,
                              int sampleRate, int channels, int bitsPerSample)
{
    // WAV 头部 RIFF size 字段为 int32_t，PCM 数据超过 2GB 时溢出
    if (pcm.size() > INT32_MAX - 36) return false;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;

    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);

    int32_t dataSize  = pcm.size();
    int32_t fileSize  = dataSize + 36;
    int32_t byteRate  = sampleRate * channels * bitsPerSample / 8;
    int16_t blockAlign = (int16_t)(channels * bitsPerSample / 8);

    f.write("RIFF", 4);
    out << fileSize;
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    out << (int32_t)16;
    out << (int16_t)1;                  // PCM
    out << (int16_t)channels;
    out << sampleRate;
    out << byteRate;
    out << blockAlign;
    out << (int16_t)bitsPerSample;
    f.write("data", 4);
    out << dataSize;
    f.write(pcm);

    return true;
}

// 将选中行的参数（除文件列表）填入编辑控件，进入编辑模式
void AudioMixPanel::loadRegionIntoControls(int row)
{
    const AudioMixRegion& r = regions_[row];
    stopPreview();  // 切换行时停止预览，防止状态不一致
    editingRow_ = row;

    // 阻断信号防止填充过程触发 onUpdateSelectedRegion 形成反馈
    ui->videoStartMinSpin->blockSignals(true);
    ui->videoStartSecSpin->blockSignals(true);
    ui->audioDurMinSpin->blockSignals(true);
    ui->audioDurSecSpin->blockSignals(true);
    ui->srcVolSlider->blockSignals(true);
    ui->mixVolSlider->blockSignals(true);

    int videoStartSec = (int)(r.videoStartUs / 1000000LL);
    ui->videoStartMinSpin->setValue(videoStartSec / 60);
    ui->videoStartSecSpin->setValue(videoStartSec % 60);
    int audioDurSec = (int)(r.audioDurationUs / 1000000LL);
    ui->audioDurMinSpin->setValue(audioDurSec / 60);
    ui->audioDurSecSpin->setValue(audioDurSec % 60);
    int srcPct = qRound(r.srcVol * 100);
    int mixPct = qRound(r.mixVol * 100);
    ui->srcVolSlider->setValue(srcPct);
    ui->mixVolSlider->setValue(mixPct);
    ui->srcVolLabel->setText(QString("源音频 %1%").arg(srcPct));
    ui->mixVolLabel->setText(QString("新增音频 %1%").arg(mixPct));

    ui->videoStartMinSpin->blockSignals(false);
    ui->videoStartSecSpin->blockSignals(false);
    ui->audioDurMinSpin->blockSignals(false);
    ui->audioDurSecSpin->blockSignals(false);
    ui->srcVolSlider->blockSignals(false);
    ui->mixVolSlider->blockSignals(false);

    // 切换到本地音频页（录音条目参数同样在此页编辑）
    if (ui->addStack->currentIndex() != 0)
        switchToLocalMode();

    ui->addRegionBtn->setText("确认更新");
}

// 控件变化时实时将参数回写到选中行
void AudioMixPanel::onUpdateSelectedRegion()
{
    if (editingRow_ < 0 || editingRow_ >= regions_.size()) return;

    AudioMixRegion& r = regions_[editingRow_];
    r.videoStartUs = (int64_t)(ui->videoStartMinSpin->value() * 60 + ui->videoStartSecSpin->value()) * 1000000LL;

    int durSec = ui->audioDurMinSpin->value() * 60 + ui->audioDurSecSpin->value();
    if (durSec > 0)
        r.audioDurationUs = (int64_t)durSec * 1000000LL;

    r.srcVol = ui->srcVolSlider->value() / 100.0f;
    r.mixVol = ui->mixVolSlider->value() / 100.0f;

    updateTableRow(editingRow_);
}

// 仅刷新指定行的数值列（col2-5），不重建整张表（保留选中与复选状态）
void AudioMixPanel::updateTableRow(int row)
{
    if (row < 0 || row >= ui->regionsTable->rowCount()) return;
    const AudioMixRegion& r = regions_[row];
    ui->regionsTable->item(row, 2)->setText(
        QTime(0, 0).addSecs(r.videoStartUs / 1000000LL).toString("HH:mm:ss"));
    ui->regionsTable->item(row, 3)->setText(
        QTime(0, 0).addSecs(r.audioDurationUs / 1000000LL).toString("HH:mm:ss"));
    ui->regionsTable->item(row, 4)->setText(
        QString("%1%").arg(qRound(r.srcVol * 100)));
    ui->regionsTable->item(row, 5)->setText(
        QString("%1%").arg(qRound(r.mixVol * 100)));
}

// 每 100ms 被 recordLevelTimer_ 触发：刷新已录时长和输入电平
void AudioMixPanel::onRecordLevel()
{
    const QByteArray& data = recordBuffer_.data();
    int totalBytes = data.size();
    int bytesPerSec = recordSampleRate_ * recordChannels_ * (recordSampleBits_ / 8);

    // 前几次打印日志，确认数据是否在增长
    if (recordLevelLogCount_ < 30) {
        recordLevelLogCount_++;
        qInfo() << "AudioMixPanel::onRecordLevel totalBytes=" << totalBytes
                << "sampleBits=" << recordSampleBits_
                << "bytesPerSec=" << bytesPerSec
                << "audioInput state=" << (audioInput_ ? (int)audioInput_->state() : -1)
                << "error=" << (audioInput_ ? (int)audioInput_->error() : -1);
    }

    // 已录时长（秒）
    if (bytesPerSec > 0) {
        int elapsedSec = totalBytes / bytesPerSec;
        ui->recStatusLabel->setText(QString("录制中… 已录 %1:%2")
            .arg(elapsedSec / 60, 2, 10, QChar('0'))
            .arg(elapsedSec % 60, 2, 10, QChar('0')));
    }

    // 仅支持 16-bit 的 RMS 计算
    if (recordSampleBits_ != 16 || totalBytes < 4) {
        ui->recLevelBar->setValue(0);
        return;
    }

    // 取最近 100ms 的数据计算 RMS
    int frameBytes  = recordChannels_ * 2;  // 每帧字节数（16-bit × 声道）
    int windowBytes = (bytesPerSec / 10) & ~(frameBytes - 1);  // 对齐到帧边界
    if (windowBytes < frameBytes) { ui->recLevelBar->setValue(0); return; }

    int startByte   = qMax(0, totalBytes - windowBytes) & ~(frameBytes - 1);
    int sampleCount = (totalBytes - startByte) / 2;  // int16 样本数
    if (sampleCount <= 0) { ui->recLevelBar->setValue(0); return; }

    const auto* samples = reinterpret_cast<const qint16*>(data.constData() + startByte);
    double sumSq = 0;
    for (int i = 0; i < sampleCount; ++i)
        sumSq += (double)samples[i] * samples[i];
    double rms = std::sqrt(sumSq / sampleCount);
    // 对数刻度：-40dB → 0%，0dB → 100%（麦克风信号弱，线性刻度几乎看不到）
    double db = 20.0 * std::log10(rms / 32767.0 + 1e-10);
    int level = qBound(0, (int)((db + 40.0) / 40.0 * 100.0), 100);
    ui->recLevelBar->setValue(level);

    if (recordLevelLogCount_ <= 30 && recordLevelLogCount_ > 0) {
        qInfo() << "AudioMixPanel::onRecordLevel windowBytes=" << windowBytes
                << "sampleCount=" << sampleCount
                << "rms=" << rms << "db=" << db << "level=" << level;
    }
}

// 探测音频/视频文件时长（微秒），失败返回 0
int64_t AudioMixPanel::probeDurationUs(const QString& path, int64_t offsetUs)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return 0;
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return 0;
    }
    int64_t dur = ctx->duration;
    avformat_close_input(&ctx);
    return (dur > offsetUs) ? (dur - offsetUs) : 0;
}
