#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "videorenderer.h"
#include "playercontroller.h"
#include "filterpanel.h"
#include "streamcontroller.h"
#include "timeline.h"
#include "thumbnailextractor.h"
#include "exportworker.h"
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>
#include <QInputDialog>
#include <QMessageBox>
#include <QCoreApplication>
#include <QStatusBar>
#include <QDebug>

// 构造函数：setupUi 完成所有控件创建和布局，此处只做指针绑定、初始值和信号连接。
// renderer_ 直接取 ui->videoWidget（promoted VideoRenderer），无需手动 setCentralWidget。
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);

    // 播放按钮：使用 SVG 图标，清除文字确保图标居中
    ui->playPauseBtn->setText("");
    ui->playPauseBtn->setIconSize(QSize(24, 24));
    ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));

    renderer_ = ui->videoWidget;
    renderer_->installEventFilter(this);
    renderer_->setFocusPolicy(Qt::StrongFocus);

    // 控制栏控件不抢焦点：鼠标点击后焦点仍留在 renderer_，
    // 保证进度条拖拽 / 点击后方向键快进快退依然有效。
    ui->progressSlider->setFocusPolicy(Qt::NoFocus);
    ui->volumeSlider->setFocusPolicy(Qt::NoFocus);
    ui->playPauseBtn->setFocusPolicy(Qt::NoFocus);
    player_   = new PlayerController(renderer_);
    streamCtrl_ = new StreamController();  // 不挂 parent，由析构函数手动控制销毁顺序

    // 创建滤镜面板，挂在右侧 Dock
    filterPanel_ = new FilterPanel(player_);
    filterDock_  = new QDockWidget("滤镜编辑器", this);
    filterDock_->setWidget(filterPanel_);
    filterDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    filterDock_->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, filterDock_);

    // 创建剪辑模式组件：Timeline + ThumbnailExtractor + ExportWorker
    timeline_       = new Timeline();
    thumbExtractor_ = new ThumbnailExtractor();
    exportWorker_   = new ExportWorker();

    // 剪辑时间轴 Dock，挂在底部
    trimDock_ = new QDockWidget("视频剪辑", this);
    trimDock_->setWidget(timeline_);
    trimDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    trimDock_->setVisible(false);
    addDockWidget(Qt::BottomDockWidgetArea, trimDock_);

    // 读取持久化配置，应用到菜单和播放器
    {
        QSettings s("RambosPlayer", "RambosPlayer");
        bool hwOn = s.value("hwAccelEnabled", true).toBool();
        ui->actionHwAccel->setChecked(hwOn);
        player_->setHwAccelEnabled(hwOn);

        int vol = s.value("volume", 80).toInt();
        ui->volumeSlider->setValue(vol);
        player_->setVolume(vol / 100.0f);
    }

    connect(ui->actionOpen,     &QAction::triggered,    this, &MainWindow::onOpenFile);
    connect(ui->playPauseBtn,   &QPushButton::clicked,  this, &MainWindow::onPlayPause);
    connect(ui->progressSlider, &QSlider::sliderMoved,    this, &MainWindow::onSeekSliderMoved);
    connect(ui->progressSlider, &QSlider::sliderReleased, this, [this]{ renderer_->setFocus(); });
    ui->progressSlider->installEventFilter(this);
    ui->volumeSlider->installEventFilter(this);
    connect(ui->volumeSlider,   &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);

    connect(player_, &PlayerController::durationChanged,  this, &MainWindow::onDurationChanged);
    connect(player_, &PlayerController::positionChanged,  this, &MainWindow::onPositionChanged);
    connect(player_, &PlayerController::playbackFinished, this, &MainWindow::onPlaybackFinished);
    connect(ui->actionClearRecent,  &QAction::triggered,  this, &MainWindow::onClearRecent);
    connect(ui->actionHwAccel,      &QAction::toggled,    this, &MainWindow::onHwAccelToggled);
    connect(ui->actionFilterPanel,  &QAction::toggled,    this, &MainWindow::onFilterPanelToggled);
    connect(ui->actionStream,      &QAction::triggered,  this, &MainWindow::onStreamStart);
    connect(streamCtrl_, &StreamController::errorOccurred, this, [](const QString& msg) {
        QMessageBox::warning(nullptr, "推流错误", msg);
    });
    connect(filterDock_, &QDockWidget::visibilityChanged, ui->actionFilterPanel, &QAction::setChecked);

    // 剪辑模式信号连接
    connect(ui->actionTrimMode, &QAction::toggled,          this, &MainWindow::onTrimModeToggled);
    connect(ui->actionExport,  &QAction::triggered,         this, &MainWindow::onExportTriggered);
    connect(trimDock_,         &QDockWidget::visibilityChanged, ui->actionTrimMode, &QAction::setChecked);
    connect(thumbExtractor_,   &ThumbnailExtractor::thumbnailReady,  this, [this](const QImage& img) {
        timeline_->addThumbnail(img);
    });
    connect(thumbExtractor_,   &ThumbnailExtractor::thumbnailsReady, this, &MainWindow::onThumbnailsReady);
    connect(thumbExtractor_,   &ThumbnailExtractor::errorOccurred,   this, [](const QString& msg) {
        QMessageBox::warning(nullptr, "缩略图错误", msg);
    });
    connect(timeline_, &Timeline::trimPointChanged, this, [this](int64_t inUs, int64_t outUs) {
        ui->actionExport->setEnabled(true);
        int inSec  = static_cast<int>(inUs / 1000000);
        int outSec = static_cast<int>(outUs / 1000000);
        int durSec = outSec - inSec;
        statusBar()->showMessage(
            QString("入口 %1:%2 → 出口 %3:%4  时长 %5秒")
                .arg(inSec / 60, 2, 10, QChar('0')).arg(inSec % 60, 2, 10, QChar('0'))
                .arg(outSec / 60, 2, 10, QChar('0')).arg(outSec % 60, 2, 10, QChar('0'))
                .arg(durSec));
    });
    connect(exportWorker_, &ExportWorker::progressed, this, &MainWindow::onExportProgress);
    connect(exportWorker_, &ExportWorker::exportFinished, this, &MainWindow::onExportFinished);
    connect(exportWorker_, &ExportWorker::errorOccurred, this, [](const QString& msg) {
        QMessageBox::warning(nullptr, "导出错误", msg);
    });

    rebuildRecentMenu();
}

// 析构函数：必须先 delete player_ 再 delete ui。
// player_->stop() 会调用 renderer_->stopRendering()，renderer_ 是 ui 内的 promoted widget，
// 若先 delete ui 则 renderer_ 已销毁，player_ 析构时再访问它会触发 UAF 崩溃。
MainWindow::~MainWindow() {
    streamCtrl_->stop();
    delete streamCtrl_;
    delete thumbExtractor_;
    delete exportWorker_;
    delete player_;
    player_ = nullptr;
    delete ui;
}

// 打开文件并开始播放，同时更新最近文件记录。供对话框和最近文件菜单共用。
// 若剪辑模式已开启，自动提取缩略图。
void MainWindow::openFile(const QString& path) {
    player_->stop();
    if (player_->open(path)) {
        qInfo() << "MainWindow: opened" << path;
        currentFile_ = path;
        updateRecentFiles(path);
        ui->playPauseBtn->setIcon(QIcon(":/icons/pause.svg"));
        player_->play();

        // 剪辑模式下自动提取缩略图
        if (trimDock_->isVisible())
            thumbExtractor_->extract(path);
    }
}

// 打开文件对话框，起始目录从 QSettings 读取上次路径。
void MainWindow::onOpenFile() {
    QSettings s("RambosPlayer", "RambosPlayer");
    QString lastDir = s.value("lastDir").toString();
    QString path = QFileDialog::getOpenFileName(
        this, "打开文件", lastDir,
        "视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)");
    if (path.isEmpty()) return;
    openFile(path);
}

// 将 path 写入最近文件列表（置顶、去重、截断至 MaxRecentFiles），保存目录，刷新菜单。
void MainWindow::updateRecentFiles(const QString& path) {
    QSettings s("RambosPlayer", "RambosPlayer");
    s.setValue("lastDir", QFileInfo(path).absolutePath());

    QStringList files = s.value("recentFiles").toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > MaxRecentFiles)
        files.removeLast();
    s.setValue("recentFiles", files);

    rebuildRecentMenu();
}

// 重建最近文件子菜单：先删除 actionClearRecent 之前的动态条目，再按列表顺序插入新条目。
void MainWindow::rebuildRecentMenu() {
    QSettings s("RambosPlayer", "RambosPlayer");
    QStringList files = s.value("recentFiles").toStringList();

    // 删除所有动态文件条目（跳过静态的 actionClearRecent）
    for (QAction* a : ui->menuRecentFiles->actions()) {
        if (a == ui->actionClearRecent) continue;
        ui->menuRecentFiles->removeAction(a);
        delete a;
    }

    // 在 actionClearRecent 之前按顺序插入新条目
    for (int i = 0; i < files.size(); ++i) {
        QString label = QString("&%1  %2").arg(i + 1)
                            .arg(QFileInfo(files[i]).fileName());
        QAction* a = new QAction(label, ui->menuRecentFiles);
        a->setToolTip(files[i]);   // 悬停时显示完整路径
        connect(a, &QAction::triggered, this, [this, path = files[i]]{ openFile(path); });
        ui->menuRecentFiles->insertAction(ui->actionClearRecent, a);
    }

    bool hasFiles = !files.isEmpty();
    ui->menuRecentFiles->setEnabled(hasFiles);
    ui->actionClearRecent->setEnabled(hasFiles);
}

// 硬件加速开关：传递到 PlayerController，持久化到 QSettings，下次 open() 时生效。
void MainWindow::onHwAccelToggled(bool checked) {
    player_->setHwAccelEnabled(checked);
    QSettings("RambosPlayer", "RambosPlayer").setValue("hwAccelEnabled", checked);
}

// 滤镜面板显示/隐藏切换
void MainWindow::onFilterPanelToggled(bool checked) {
    filterDock_->setVisible(checked);
}

// 清除最近文件列表并刷新菜单。
void MainWindow::onClearRecent() {
    QSettings("RambosPlayer", "RambosPlayer").remove("recentFiles");
    rebuildRecentMenu();
}

// 播放/暂停切换，同步更新按钮文字。
void MainWindow::onPlayPause() {
    if (player_->isPlaying()) {
        player_->pause();
        ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));
    } else {
        player_->play();
        ui->playPauseBtn->setIcon(QIcon(":/icons/pause.svg"));
    }
}

// 进度条拖拽时触发 seek，value 范围 0–1000 映射到 0–duration 秒。
void MainWindow::onSeekSliderMoved(int value) {
    if (duration_ <= 0) return;
    double seconds = (double)value / 1000.0 * duration_ / 1000.0;
    player_->seek(seconds);
}

// 音量滑块变化，value 范围 0–100 映射到 0.0–1.0，并持久化到 QSettings。
void MainWindow::onVolumeChanged(int value) {
    player_->setVolume(value / 100.0f);
    QSettings("RambosPlayer", "RambosPlayer").setValue("volume", value);
}

// 收到文件时长后设置标签右半部分，进度条最大值固定 1000 无需更改。
void MainWindow::onDurationChanged(int64_t ms) {
    duration_ = ms;
    ui->timeLabel->setText("00:00 / " + formatTime(ms));
}

// 每 100ms 更新进度条和时间标签；用户正在拖拽时跳过进度条更新，避免跳动。
void MainWindow::onPositionChanged(int64_t ms) {
    currentPos_ = ms;
    if (!ui->progressSlider->isSliderDown() && duration_ > 0)
        ui->progressSlider->setValue((int)((double)ms / duration_ * 1000));
    ui->timeLabel->setText(formatTime(ms) + " / " + formatTime(duration_));
}

// 播放结束后复位按钮为播放状态。
void MainWindow::onPlaybackFinished() {
    ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));
}

// 拦截进度条/音量条的鼠标点击，实现点哪跳哪而非 pageStep 步进。
// 同时拦截 VideoRenderer 的键盘事件，实现方向键快进/快退与空格暂停。
bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto* slider = qobject_cast<QSlider*>(obj);
        if (slider) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                int val = QStyle::sliderValueFromPosition(
                    slider->minimum(), slider->maximum(),
                    me->x(), slider->width());
                slider->setValue(val);
                if (slider == ui->progressSlider)
                    onSeekSliderMoved(val);
                return true;
            }
        }
    }
    if (event->type() == QEvent::KeyPress && obj == renderer_) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->isAutoRepeat()) return false;  // 忽略按键自动重复，避免多次 seek 叠加
        if (ke->key() == Qt::Key_Space) {
            qInfo() << "MainWindow: Space pressed, toggle play/pause";
            onPlayPause();
            return true;
        }
        if (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right) {
            if (duration_ <= 0) {
                qInfo() << "MainWindow: arrow key ignored, no media loaded";
                return true;
            }
            double offset = (ke->key() == Qt::Key_Left) ? -10.0 : 10.0;
            double newSec = qBound(0.0, currentPos_ / 1000.0 + offset, duration_ / 1000.0);
            qInfo() << "MainWindow:" << (ke->key() == Qt::Key_Left ? "Left" : "Right")
                    << "arrow, seek from" << currentPos_ / 1000.0 << "s to" << newSec << "s"
                    << "(offset =" << offset << "s)";
            player_->seek(newSec);
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// 空格暂停/恢复，左右方向键快退/快进 10 秒。
// 仅当焦点不在 VideoRenderer 时作为降级路径；正常情况由 eventFilter 拦截。
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    if (event->key() == Qt::Key_Space) {
        qInfo() << "MainWindow::keyPressEvent Space";
        onPlayPause();
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        if (duration_ <= 0) {
            qInfo() << "MainWindow::keyPressEvent arrow ignored, no media loaded";
            return;
        }
        double offset = (event->key() == Qt::Key_Left) ? -10.0 : 10.0;
        double newSec = qBound(0.0, currentPos_ / 1000.0 + offset, duration_ / 1000.0);
        qInfo() << "MainWindow::keyPressEvent" << (event->key() == Qt::Key_Left ? "Left" : "Right")
                << "seek to" << newSec;
        player_->seek(newSec);
    } else {
        QMainWindow::keyPressEvent(event);
    }
}

// 双击切换全屏/窗口模式。
void MainWindow::mouseDoubleClickEvent(QMouseEvent*) {
    isFullscreen_ = !isFullscreen_;
    isFullscreen_ ? showFullScreen() : showNormal();
}

// 推流设置对话框：输入源和目标 URL，启动/停止推流。
// 启动中时再次点击变为停止。默认保存至 exe 同目录。
void MainWindow::onStreamStart() {
    if (streamCtrl_->isStreaming()) {
        streamCtrl_->stop();
        ui->actionStream->setText("推流(&S)...");
        return;
    }

    QString source = QInputDialog::getText(this, "推流源",
        "采集源 (desktop / 摄像头设备名):",
        QLineEdit::Normal, "desktop");
    if (source.isEmpty()) return;

    QString defaultPath = QCoreApplication::applicationDirPath() + "/record.flv";
    QString url = QInputDialog::getText(this, "推流目标",
        "RTMP URL 或本地 .flv 路径:",
        QLineEdit::Normal, defaultPath);
    if (url.isEmpty()) return;

    if (streamCtrl_->start(source, url)) {
        ui->actionStream->setText("停止推流");
    } else {
        statusBar()->showMessage("推流启动失败，详见弹窗提示", 5000);
    }
}

// 剪辑模式切换：显示/隐藏时间轴 Dock，启动缩略图提取。
void MainWindow::onTrimModeToggled(bool checked) {
    trimDock_->setVisible(checked);
    if (checked && !currentFile_.isEmpty()) {
        timeline_->setDuration(duration_ * 1000);  // ms → us，先画刻度
        thumbExtractor_->extract(currentFile_);
        ui->actionExport->setEnabled(false);
    }
}

// 缩略图全部提取完成：确保时长已设置（第一张来时就设了，这里做兜底）。
void MainWindow::onThumbnailsReady(const QList<QImage>&) {
    timeline_->setDuration(duration_ * 1000);
}

// 导出片段：弹出保存对话框，启动 ExportWorker。记住上次导出路径。
void MainWindow::onExportTriggered() {
    QSettings s("RambosPlayer", "RambosPlayer");
    QString lastExportDir = s.value("lastExportDir",
                                     QFileInfo(currentFile_).absolutePath()).toString();
    QString defaultPath = lastExportDir + "/clip_" + QFileInfo(currentFile_).completeBaseName() + ".mp4";

    QString outPath = QFileDialog::getSaveFileName(this, "导出剪辑片段", defaultPath,
        "MP4 (*.mp4);;所有文件 (*)");
    if (outPath.isEmpty()) return;

    s.setValue("lastExportDir", QFileInfo(outPath).absolutePath());

    int64_t inUs  = timeline_->inPts();
    int64_t outUs = timeline_->outPts();
    qInfo() << "导出范围:" << inUs / 1000000.0 << "s –" << outUs / 1000000.0 << "s";

    statusBar()->showMessage("正在导出...");
    ui->actionExport->setEnabled(false);
    exportWorker_->run(currentFile_, outPath, inUs, outUs);
}

// 导出进度更新。
void MainWindow::onExportProgress(int64_t currentPts, int64_t totalPts) {
    if (totalPts > 0) {
        int pct = static_cast<int>(currentPts * 100 / totalPts);
        statusBar()->showMessage(QString("导出中... %1%").arg(pct));
    }
}

// 导出完成/失败处理。
void MainWindow::onExportFinished(bool ok) {
    statusBar()->showMessage(ok ? "导出完成" : "导出失败", 5000);
    ui->actionExport->setEnabled(true);
}

// 毫秒转 "MM:SS" 字符串，用于时间标签显示。
QString MainWindow::formatTime(int64_t ms) {
    int s = (int)(ms / 1000);
    return QString("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}
