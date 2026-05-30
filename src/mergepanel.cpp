#include "mergepanel.h"
#include "ui_mergepanel.h"
#include "mergeworker.h"
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QDateTime>
#include <QListWidgetItem>

MergePanel::MergePanel(QWidget* parent)
    : QWidget(parent), ui(new Ui::MergePanel)
{
    ui->setupUi(this);
    setAcceptDrops(true);

    worker_ = new MergeWorker(this);
    connect(worker_, &MergeWorker::progressed,    this, &MergePanel::onProgressed);
    connect(worker_, &MergeWorker::mergeFinished, this, &MergePanel::onMergeFinished);
    connect(worker_, &MergeWorker::errorOccurred, this, &MergePanel::onError);

    connect(ui->modeCombo,    QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MergePanel::onModeChanged);
    connect(ui->addFileBtn,   &QPushButton::clicked, this, &MergePanel::onAddFile);
    connect(ui->removeFileBtn,&QPushButton::clicked, this, &MergePanel::onRemoveFile);
    connect(ui->clearBtn,     &QPushButton::clicked, this, &MergePanel::onClearFiles);
    connect(ui->fileList,     &QListWidget::itemSelectionChanged,
            this, &MergePanel::onFileSelectionChanged);
    connect(ui->volumeSlider, &QSlider::valueChanged, this, &MergePanel::onVolumeChanged);
    connect(ui->browseBtn,    &QPushButton::clicked, this, &MergePanel::onBrowseOutput);
    connect(ui->startBtn,     &QPushButton::clicked, this, &MergePanel::onStart);
    connect(ui->cancelBtn,    &QPushButton::clicked, this, &MergePanel::onCancel);

    onModeChanged(0);
}

MergePanel::~MergePanel()
{
    if (worker_->isRunning()) {
        worker_->requestInterruption();
        worker_->wait(3000);
    }
    delete ui;
}

// 接受包含文件 URL 的拖拽操作
void MergePanel::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

// 拖入文件：将有效文件路径添加到列表
void MergePanel::dropEvent(QDropEvent* e)
{
    for (const QUrl& url : e->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        QString path = url.toLocalFile();
        if (!QFileInfo::exists(path)) continue;
        ui->fileList->addItem(path);
        volumes_[ui->fileList->count() - 1] = 1.0;
    }
    updateStartEnabled();
}

// 模式切换：调整提示文字、音量组显隐
void MergePanel::onModeChanged(int index)
{
    // 0=拼接视频 1=音频混音 2=音视频合流
    bool isMux = (index == 2);

    ui->volumeGroup->setVisible(false);  // 当前版本不用音量调节

    switch (index) {
    case 0:
        ui->hintLabel->setText("拖入或添加 2+ 个视频文件（相同参数→无损，不同参数→重编码）");
        break;
    case 1:
        ui->hintLabel->setText("拖入或添加 2+ 个音频文件，首尾相接合成一个文件");
        break;
    case 2:
        ui->hintLabel->setText("第一个是视频，其余是音频（按顺序拼接后替换原声）");
        break;
    }


    updateStartEnabled();
}

void MergePanel::onAddFile()
{
    int mode = ui->modeCombo->currentIndex();
    QString filter;

    if (mode == 1) {
        filter = "音频文件 (*.mp3 *.aac *.wav *.flac *.m4a *.ogg *.wma);;视频文件 (*.mp4 *.mkv *.mov *.avi);;所有文件 (*.*)";
    } else if (mode == 2) {
        // 替换音频：列表为空时选视频，已有视频后选音频
        if (ui->fileList->count() == 0)
            filter = "视频文件 (*.mp4 *.mkv *.mov *.avi *.ts *.flv);;所有文件 (*.*)";
        else
            filter = "音频文件 (*.mp3 *.aac *.wav *.flac *.m4a *.ogg *.wma);;视频文件 (*.mp4 *.mkv *.mov *.avi);;所有文件 (*.*)";
    } else {
        filter = "视频文件 (*.mp4 *.mkv *.mov *.avi *.ts *.flv);;所有文件 (*.*)";
    }

    QStringList files = QFileDialog::getOpenFileNames(this, "选择文件", QString(), filter);
    for (const QString& f : files) {
        ui->fileList->addItem(f);
        volumes_[ui->fileList->count() - 1] = 1.0;
    }
    updateStartEnabled();
}

void MergePanel::onRemoveFile()
{
    for (QListWidgetItem* item : ui->fileList->selectedItems()) {
        int row = ui->fileList->row(item);
        volumes_.remove(row);
        delete ui->fileList->takeItem(row);
    }
    // 重建 volumes_ 索引（行号可能变化）
    QMap<int, double> rebuilt;
    for (int i = 0; i < ui->fileList->count(); ++i)
        rebuilt[i] = volumes_.value(i, 1.0);
    volumes_ = rebuilt;
    updateStartEnabled();
}

void MergePanel::onClearFiles()
{
    ui->fileList->clear();
    volumes_.clear();
    updateStartEnabled();
}

// 选中文件变化：更新音量滑块显示该文件的当前音量（仅混音模式）
void MergePanel::onFileSelectionChanged()
{
    if (ui->modeCombo->currentIndex() != 1) return;
    auto selected = ui->fileList->selectedItems();
    if (selected.isEmpty()) return;
    int row = ui->fileList->row(selected.first());
    int vol = static_cast<int>(volumes_.value(row, 1.0) * 100);
    ui->volumeSlider->blockSignals(true);
    ui->volumeSlider->setValue(vol);
    ui->volumeSlider->blockSignals(false);
    ui->volumeLabel->setText(QString("%1%").arg(vol));
}

// 音量滑块变化：更新选中文件的音量权重
void MergePanel::onVolumeChanged(int value)
{
    ui->volumeLabel->setText(QString("%1%").arg(value));
    auto selected = ui->fileList->selectedItems();
    if (selected.isEmpty()) return;
    int row = ui->fileList->row(selected.first());
    volumes_[row] = value / 100.0;
}

void MergePanel::onBrowseOutput()
{
    int mode = ui->modeCombo->currentIndex();
    QString filter;
    QString defaultName;
    if (mode == 1) {
        filter      = "音频文件 (*.aac *.mp3 *.m4a);;所有文件 (*.*)";
        defaultName = "mixed_audio.aac";
    } else {
        filter      = "视频文件 (*.mp4 *.mkv *.mov);;所有文件 (*.*)";
        defaultName = (mode == 0) ? "concat_output.mp4" : "muxed_output.mp4";
    }

    QString path = QFileDialog::getSaveFileName(this, "输出文件", defaultName, filter);
    if (!path.isEmpty()) ui->outputEdit->setText(path);
    updateStartEnabled();
}

void MergePanel::onStart()
{
    if (worker_->isRunning()) return;

    MergeWorker::Task task;
    switch (ui->modeCombo->currentIndex()) {
    case 0: task.mode = MergeWorker::Mode::ConcatVideo; break;
    case 1: task.mode = MergeWorker::Mode::MixAudio;    break;
    case 2: task.mode = MergeWorker::Mode::MuxAV;       break;
    }

    task.inputFiles = filePaths();
    task.outputFile = ui->outputEdit->text().trimmed();

    if (task.mode == MergeWorker::Mode::MixAudio) {
        for (int i = 0; i < task.inputFiles.size(); ++i)
            task.volumes.append(volumeFor(i));
    }

    appendLog(QString("[%1] 开始：%2 → %3")
              .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
              .arg(task.inputFiles.join(" + "))
              .arg(task.outputFile));

    worker_->prepare(task);
    worker_->start();
    setRunning(true);
}

void MergePanel::onCancel()
{
    if (!worker_->isRunning()) return;
    appendLog("正在取消...");
    worker_->requestInterruption();
    worker_->wait(3000);
    setRunning(false);
    appendLog("已取消");
}

void MergePanel::onProgressed(int percent)
{
    ui->progressBar->setValue(percent);
}

void MergePanel::onMergeFinished(bool ok)
{
    setRunning(false);
    ui->progressBar->setValue(ok ? 100 : ui->progressBar->value());
    appendLog(ok ? "完成！" : "失败，请查看错误信息");
}

void MergePanel::onError(const QString& msg)
{
    appendLog("[错误] " + msg);
}

// ===== 私有辅助 =====

void MergePanel::updateStartEnabled()
{
    int n    = ui->fileList->count();
    bool hasOut = !ui->outputEdit->text().trimmed().isEmpty();
    int minFiles = (ui->modeCombo->currentIndex() == 2) ? 2 : 2;
    ui->startBtn->setEnabled(n >= minFiles && hasOut && !worker_->isRunning());
}

void MergePanel::setRunning(bool running)
{
    ui->modeCombo->setEnabled(!running);
    ui->fileList->setEnabled(!running);
    ui->addFileBtn->setEnabled(!running);
    ui->removeFileBtn->setEnabled(!running);
    ui->clearBtn->setEnabled(!running);
    ui->volumeGroup->setEnabled(!running);
    ui->outputEdit->setEnabled(!running);
    ui->browseBtn->setEnabled(!running);
    ui->startBtn->setEnabled(!running);
    ui->cancelBtn->setEnabled(running);
    if (!running) updateStartEnabled();
}

void MergePanel::appendLog(const QString& msg)
{
    ui->logEdit->append(msg);
}

QStringList MergePanel::filePaths() const
{
    QStringList list;
    for (int i = 0; i < ui->fileList->count(); ++i)
        list << ui->fileList->item(i)->text();
    return list;
}

double MergePanel::volumeFor(int row) const
{
    return volumes_.value(row, 1.0);
}
