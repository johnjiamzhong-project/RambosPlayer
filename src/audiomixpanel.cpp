#include "audiomixpanel.h"
#include "audiomixworker.h"
#include "audiopreviewwindow.h"
#include "playercontroller.h"
#include "timeline.h"
#include "ui_audiomixpanel.h"
#include "logger.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QAudioInput>
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
    connect(ui->browseAudioBtn,  &QPushButton::clicked, this, &AudioMixPanel::onBrowseAudio);
    connect(ui->srcVolSlider,    &QSlider::valueChanged, this, &AudioMixPanel::onSrcVolChanged);
    connect(ui->mixVolSlider,    &QSlider::valueChanged, this, &AudioMixPanel::onMixVolChanged);
    connect(ui->addRegionBtn,    &QPushButton::clicked, this, &AudioMixPanel::onAddRegion);
    connect(ui->removeRegionBtn, &QPushButton::clicked, this, &AudioMixPanel::onRemoveRegion);
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

    // 列表选中时启用相关按钮
    connect(ui->regionsTable, &QTableWidget::itemSelectionChanged, this, [this]{
        bool sel = !ui->regionsTable->selectedItems().isEmpty();
        ui->removeRegionBtn->setEnabled(sel);
        ui->listenBtn->setEnabled(sel);
        ui->previewBtn->setEnabled(sel && player_ != nullptr);
    });

    // 表格列宽：音频文件列拉伸填充，数值列固定像素宽
    ui->regionsTable->horizontalHeader()->setStretchLastSection(false);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    ui->regionsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    ui->regionsTable->setColumnWidth(1, 65);   // 贴入时间（HH:mm:ss）
    ui->regionsTable->setColumnWidth(2, 65);   // 时长
    ui->regionsTable->setColumnWidth(3, 55);   // 源音量
    ui->regionsTable->setColumnWidth(4, 55);   // 混音量
    // 鼠标追踪：启用后 hover 样式（整行高亮）才能生效
    ui->regionsTable->setMouseTracking(true);
}

AudioMixPanel::~AudioMixPanel()
{
    stopPreview();
    if (audioInput_) { audioInput_->stop(); delete audioInput_; }
    if (worker_->isRunning()) { worker_->requestInterruption(); worker_->wait(3000); }
    // 清理录音临时文件
    for (const auto& r : regions_) {
        if (r.isRecorded) QFile::remove(r.sourcePath);
    }
    delete ui;
}

void AudioMixPanel::setPlayerController(PlayerController* pc)
{
    player_ = pc;
}

void AudioMixPanel::setTimeline(Timeline* tl)
{
    timeline_ = tl;
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

void AudioMixPanel::onBrowseAudio()
{
    QString path = QFileDialog::getOpenFileName(this, "选择音频文件", QString(),
        "音频文件 (*.mp3 *.aac *.wav *.flac *.ogg *.m4a);;所有文件 (*)");
    if (!path.isEmpty())
        ui->audioFileEdit->setText(path);
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
}

void AudioMixPanel::onMixVolChanged(int value)
{
    ui->mixVolLabel->setText(QString("新增音频 %1%").arg(value));
    int srcVal = 100 - value;
    ui->srcVolSlider->blockSignals(true);
    ui->srcVolSlider->setValue(srcVal);
    ui->srcVolSlider->blockSignals(false);
    ui->srcVolLabel->setText(QString("源音频 %1%").arg(srcVal));
}

// 将本地音频添加到区间列表
void AudioMixPanel::onAddRegion()
{
    QString audioPath = ui->audioFileEdit->text().trimmed();
    if (audioPath.isEmpty()) {
        QMessageBox::warning(this, "未指定音频", "请先选择音频文件。");
        return;
    }
    if (!QFileInfo::exists(audioPath)) {
        QMessageBox::warning(this, "文件不存在", "指定的音频文件不存在：\n" + audioPath);
        return;
    }

    AudioMixRegion r;
    r.sourcePath    = audioPath;
    r.displayName   = QFileInfo(audioPath).fileName();
    r.videoStartUs  = (int64_t)ui->videoStartSpin->value() * 1000000LL;
    r.audioOffsetUs = 0;
    r.srcVol        = ui->srcVolSlider->value() / 100.0f;
    r.mixVol        = ui->mixVolSlider->value() / 100.0f;
    r.isRecorded    = false;

    int userDurSec = ui->audioDurationSpin->value();
    if (userDurSec > 0) {
        r.audioDurationUs = (int64_t)userDurSec * 1000000LL;
    } else {
        r.audioDurationUs = probeDurationUs(audioPath);
        if (r.audioDurationUs <= 0) {
            QMessageBox::warning(this, "无法读取时长", "无法读取音频文件时长，请手动输入使用时长。");
            return;
        }
    }

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
    updateTimeline();
    updateExportEnabled();
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
void AudioMixPanel::onPreview()
{
    int row = ui->regionsTable->currentRow();
    if (row < 0 || row >= regions_.size() || !player_) return;
    const AudioMixRegion& r = regions_[row];

    stopPreview();

    previewSavedVolume_ = player_->volume();  // 保存试播放前的实际音量
    player_->seek(r.videoStartUs / 1e6);
    player_->setVolume(r.srcVol);
    player_->play();

    previewPlayer_ = new QMediaPlayer(this);
    previewPlayer_->setMedia(QUrl::fromLocalFile(r.sourcePath));
    previewPlayer_->setPosition(r.audioOffsetUs / 1000LL);
    previewPlayer_->setVolume(qRound(r.mixVol * 100));
    connect(previewPlayer_, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State state){
        if (state == QMediaPlayer::StoppedState) stopPreview();
    });
    previewPlayer_->play();

    // 到区间结束时停止
    int playMs = (int)(r.audioDurationUs / 1000LL);
    if (playMs > 0)
        QTimer::singleShot(playMs, this, [this]{ stopPreview(); });
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
    if (regions_.isEmpty()) {
        QMessageBox::information(this, "无音频区间", "请先添加至少一段音频。");
        return;
    }
    QString output = ui->outputEdit->text().trimmed();
    if (output.isEmpty()) {
        QMessageBox::warning(this, "未指定输出路径", "请指定输出文件路径。");
        return;
    }

    AudioMixTask task;
    task.originalVideoPath = sourceFile_;
    task.regions           = regions_;
    task.outputPath        = output;

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

        recordVideoStartUs_ = (int64_t)ui->recVideoStartSpin->value() * 1000000LL;
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

        QAudioDeviceInfo devInfo(QAudioDeviceInfo::defaultInputDevice());
        if (!devInfo.isFormatSupported(fmt)) {
            fmt = devInfo.nearestFormat(fmt);
            qWarning() << "AudioMixPanel: 请求格式不支持，使用近似格式";
        }

        audioInput_ = new QAudioInput(fmt, this);
        recordBuffer_.setData(QByteArray());
        recordBuffer_.open(QIODevice::WriteOnly);
        audioInput_->start(&recordBuffer_);

        recording_ = true;
        ui->recStartStopBtn->setText("停止录入");
        ui->recStatusLabel->setText("录制中… 播放视频同时录入麦克风，完成后点击停止");
    } else {
        // 停止录音
        if (audioInput_) {
            audioInput_->stop();
            delete audioInput_;
            audioInput_ = nullptr;
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

            if (writeWav(tempPath, pcmData, 44100, 2, 16)) {
                AudioMixRegion r;
                r.sourcePath     = tempPath;
                r.isRecorded     = true;
                r.videoStartUs   = recordVideoStartUs_;
                r.audioOffsetUs  = 0;
                // PCM 时长：先乘后除保留精度，避免整数截断到整秒
                r.audioDurationUs = pcmData.size() * 1000000LL / (44100LL * 2 * 2);
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
    updateTimeline();
    updateExportEnabled();
}

void AudioMixPanel::rebuildTable()
{
    ui->regionsTable->setRowCount(0);
    for (const auto& r : regions_) {
        int row = ui->regionsTable->rowCount();
        ui->regionsTable->insertRow(row);

        auto makeItem = [](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };

        // 音频文件列：tooltip 显示完整路径，鼠标悬停可查看
        auto* nameItem = makeItem(r.displayName);
        nameItem->setToolTip(r.sourcePath);
        ui->regionsTable->setItem(row, 0, nameItem);
        ui->regionsTable->setItem(row, 1, makeItem(
            QTime(0, 0).addSecs(r.videoStartUs / 1000000LL).toString("HH:mm:ss")));
        ui->regionsTable->setItem(row, 2, makeItem(
            QTime(0, 0).addSecs(r.audioDurationUs / 1000000LL).toString("HH:mm:ss")));
        ui->regionsTable->setItem(row, 3, makeItem(
            QString("%1%").arg(qRound(r.srcVol * 100))));
        ui->regionsTable->setItem(row, 4, makeItem(
            QString("%1%").arg(qRound(r.mixVol * 100))));
    }
}

void AudioMixPanel::updateTimeline()
{
    if (!timeline_) return;
    QList<QPair<int64_t, int64_t>> pairs;
    for (const auto& r : regions_)
        pairs.append({r.videoStartUs, r.videoStartUs + r.audioDurationUs});
    timeline_->setAudioRegions(pairs);
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
    if (previewPlayer_) {
        previewPlayer_->stop();
        previewPlayer_->deleteLater();
        previewPlayer_ = nullptr;
    }
    if (player_) {
        player_->pause();
        player_->setVolume(previewSavedVolume_);
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
