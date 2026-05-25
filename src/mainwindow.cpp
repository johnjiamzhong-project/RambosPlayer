#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "videorenderer.h"
#include "playercontroller.h"
#include "filterpanel.h"
#include "streamcontroller.h"
#include "streamconfigdialog.h"
#include "timeline.h"
#include "thumbnailextractor.h"
#include "exportworker.h"
#include <QDockWidget>
#include <QStyleOptionSlider>
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
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onAbout);

    rebuildRecentMenu();
}

// 析构函数：必须严格按顺序销毁，防止各组件通过裸指针互相访问已释放的对象。
// 1. filterDock_（含 filterPanel_）持有 player_ 裸指针，必须在 delete player_ 之前先删；
//    否则 Qt 基类析构时才删 Dock，彼时 player_ 已释放，FilterPanel slot 触发即 UAF。
// 2. trimDock_（含 timeline_）同理，须在 delete player_ 之前删除。
// 3. player_->stop() 调用 renderer_->stopRendering()，renderer_ 是 ui 内的 promoted widget，
//    必须先 delete player_ 再 delete ui，否则 renderer_ 已析构，player_ 析构时访问触发 UAF。
MainWindow::~MainWindow() {
    // clearRestreamPacketQueues 先让 DemuxThread 解除对队列的引用（abort + clear 裸指针），
    // 必须在 streamCtrl_->stop() 销毁 FrameQueue 对象之前调用，否则触发 UAF 崩溃。
    player_->clearRestreamPacketQueues();
    streamCtrl_->stop();
    delete streamCtrl_;
    delete thumbExtractor_;
    delete exportWorker_;

    // filterDock_ 和 trimDock_ 作为 addDockWidget 子控件默认由 QMainWindow 基类析构删除，
    // 但它们内部持有 player_ 裸指针，必须在 delete player_ 之前手动提前删除。
    delete filterDock_;  filterDock_  = nullptr;
    delete trimDock_;    trimDock_    = nullptr;

    delete player_;
    player_ = nullptr;
    delete ui;
}

// 打开文件并开始播放，同时更新最近文件记录。供对话框和最近文件菜单共用。
// 若剪辑模式已开启，自动提取缩略图；若有预配置推流目标，自动启动推流。
void MainWindow::openFile(const QString& path) {
    player_->stop();
    if (player_->open(path)) {
        qInfo() << "MainWindow: opened" << path;
        currentFile_ = path;
        updateRecentFiles(path);
        // 有预配置推流目标时，在 play() 前先注册推流队列，
        // 确保 DemuxThread 启动后首个关键帧即进入推流通道，避免漏等整个 GOP
        if (!pendingDests_.isEmpty()) {
            startStreaming(pendingDests_);
            pendingDests_.clear();
        }

        player_->play();

        // 播放成功后，根据实际状态更新按钮（避免状态不同步）
        if (player_->isPlaying()) {
            ui->playPauseBtn->setIcon(QIcon(":/icons/pause.svg"));
        } else {
            ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));
        }

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

// 播放/暂停切换，同步更新按钮图标。
// 推流进行中时：暂停→断开 restream 队列防止超前写入；恢复→重连并 seek 对齐。
void MainWindow::onPlayPause() {
    if (player_->isPlaying()) {
        player_->pause();
        ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));
        if (streamCtrl_->isStreaming()) {
            player_->clearRestreamPacketQueues();
            qInfo() << "MainWindow: restream disconnected on pause";
        }
    } else {
        if (streamCtrl_->isStreaming())
            reconnectStreaming();
        player_->play();
        ui->playPauseBtn->setIcon(QIcon(":/icons/pause.svg"));
    }
}

// 推流恢复：重新连接 restream 队列。
// 不做 seek：暂停期间 DemuxThread 被满的播放队列阻塞，文件读取位置漂移极小，
// seek 反而会触发 pre-target 帧跳过，导致播放管线长时间无帧输出、画面冻结。
void MainWindow::reconnectStreaming() {
    qInfo() << "MainWindow: reconnectStreaming local=" << streamCtrl_->recorders().size()
            << "remote=" << streamCtrl_->videoMuxQueues().size();
    // 先清掉 DemuxThread 中已有的录制器/队列，防止 startStreaming 后立即 resume 导致重复添加
    player_->clearRestreamPacketQueues();
    // clearRestreamPacketQueues 内部调用了 q->abort()，使队列进入 aborted 状态。
    // abort 后 tryPush 直接 return false（不检查容量），导致重连后 sentinel 推不进去，
    // MuxThread 的 waitingForStart 永远不能清除，所有包被丢弃。
    // 必须在重新注册到 DemuxThread 前 reset，清除 aborted 标志并清空残留包。
    for (const auto& q : streamCtrl_->videoMuxQueues()) q->reset();
    for (const auto& q : streamCtrl_->audioMuxQueues()) q->reset();
    // HttpFlvServer 的队列同样需要 reset，否则 abort 后 tryPush 永远返回 false
    for (const auto& srv : streamCtrl_->httpFlvServers()) {
        srv->videoQueue()->reset();
        srv->audioQueue()->reset();
    }
    streamCtrl_->setWaitingForStart(true);
    // 直接推 sentinel 到 restream 队列（重置 PTS + 触发关键帧门控），
    // 必须在 add* 之前，否则 DemuxThread 可能抢先推入帧，sentinel 被排到后面。
    for (const auto& srv : streamCtrl_->httpFlvServers()) {
        srv->videoQueue()->tryPush(nullptr);
        srv->audioQueue()->tryPush(nullptr);
    }
    for (int i = 0; i < (int)streamCtrl_->videoMuxQueues().size(); ++i) {
        streamCtrl_->videoMuxQueues()[i]->tryPush(nullptr);
        streamCtrl_->audioMuxQueues()[i]->tryPush(nullptr);
    }
    int localIdx = 0, remoteIdx = 0, httpFlvIdx = 0;
    for (const auto& dest : activeDests_) {
        if (dest.type == StreamDestination::LocalFile) {
            player_->addLocalRecorder(streamCtrl_->recorders()[localIdx++].get());
        } else if (dest.type == StreamDestination::HttpFlv) {
            auto* srv = streamCtrl_->httpFlvServers()[httpFlvIdx++].get();
            player_->addRestreamVideoPacketQueue(srv->videoQueue());
            player_->addRestreamAudioPacketQueue(srv->audioQueue());
        } else {
            player_->addRestreamVideoPacketQueue(
                streamCtrl_->videoMuxQueues()[remoteIdx].get());
            player_->addRestreamAudioPacketQueue(
                streamCtrl_->audioMuxQueues()[remoteIdx].get());
            player_->addMuxThread(streamCtrl_->muxThreads()[remoteIdx].get());
            remoteIdx++;
        }
    }
    double alignSec = player_->currentPositionSeconds();
    if (alignSec < 0.0) alignSec = currentPos_ / 1000.0;
    streamAlignSec_ = alignSec;
    qInfo() << "MainWindow: restream reconnected, align=" << alignSec << "s";
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

// 播放结束：复位按钮，若正在推流则自动停止。
// playbackFinished 由 QTimer::singleShot(500ms) 延迟发出，切换文件时新文件可能已开始播放，
// 此时忽略旧文件的结束事件，避免把按钮图标覆盖回 play。
void MainWindow::onPlaybackFinished() {
    if (player_->isPlaying()) return;
    ui->playPauseBtn->setIcon(QIcon(":/icons/play.svg"));
    if (streamCtrl_->isStreaming()) {
        double stopSec = player_->currentPositionSeconds();
        if (stopSec < 0.0) stopSec = currentPos_ / 1000.0;
        streamCtrl_->setStreamStopDuration(stopSec - streamAlignSec_);
        player_->clearRestreamPacketQueues();
        streamCtrl_->stop();
        activeDests_.clear();
        ui->actionStream->setText("推流(&S)...");
        statusBar()->showMessage("播放结束，推流已自动停止", 4000);
    }
}

// 拦截进度条/音量条的鼠标点击，实现点哪跳哪而非 pageStep 步进。
// 同时拦截 VideoRenderer 的键盘事件，实现方向键快进/快退与空格暂停。
bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto* slider = qobject_cast<QSlider*>(obj);
        if (slider) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QStyleOptionSlider opt;
                opt.initFrom(slider);
                opt.minimum = slider->minimum();
                opt.maximum = slider->maximum();
                opt.sliderPosition = slider->sliderPosition();
                opt.sliderValue = slider->value();
                QRect handleRect = slider->style()->subControlRect(
                    QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, slider);
                if (handleRect.contains(me->pos()))
                    return false;
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

// 推流配置对话框入口：
// - 正在推流时：停止推流。
// - 文件已打开：弹配置对话框，确认后立即启动推流管线。
// - 尚未打开文件：弹配置对话框，确认后保存为预配置目标，打开文件时自动启动。
void MainWindow::onStreamStart() {
    if (streamCtrl_->isStreaming()) {
        double stopSec = player_->currentPositionSeconds();
        if (stopSec < 0.0) stopSec = currentPos_ / 1000.0;
        streamCtrl_->setStreamStopDuration(stopSec - streamAlignSec_);
        player_->clearRestreamPacketQueues();
        streamCtrl_->stop();
        activeDests_.clear();
        ui->actionStream->setText("推流(&S)...");
        return;
    }

    // 取消预配置（已配置但还没开文件，再次点击则取消）
    if (!pendingDests_.isEmpty()) {
        pendingDests_.clear();
        ui->actionStream->setText("推流(&S)...");
        statusBar()->showMessage("推流预配置已取消", 3000);
        return;
    }

    StreamConfigDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QList<StreamDestination> dests = dlg.destinations();
    if (dests.isEmpty()) return;

    if (player_->isOpened()) {
        // 文件已打开，立即启动
        startStreaming(dests);
    } else {
        // 尚未打开文件，保存预配置，打开文件后自动启动
        pendingDests_ = dests;
        ui->actionStream->setText("取消预配置");
        statusBar()->showMessage(
            QString("推流已预配置 %1 路目标，打开文件后自动启动").arg(dests.size()), 0);
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

// 启动推流管线（-c copy 直通模式）：
// 直接从 PlayerController 取源文件 AVCodecParameters，无需重编码。
// MuxThread 启动后，将其输入队列注册到 DemuxThread 分叉列表，
// DemuxThread 此后自动把每个原始 packet clone 一份推入推流队列。
void MainWindow::startStreaming(const QList<StreamDestination>& dests) {
    AVCodecParameters* vpar = player_->videoCodecPar();
    AVCodecParameters* apar = player_->audioCodecPar();
    AVRational vtb = player_->videoStreamTimeBase();
    AVRational atb = player_->audioStreamTimeBase();

    if (!vpar) {
        QMessageBox::warning(this, "推流", "无法读取视频流参数，请确认文件有效。");
        return;
    }

    if (!streamCtrl_->start(dests, vpar, vtb, apar, atb)) {
        statusBar()->showMessage("推流启动失败，详见弹窗提示", 5000);
        return;
    }

    activeDests_ = dests;  // 保存目标列表，暂停恢复时用于区分本地/远程

    // 直接推 sentinel 到 restream 队列（重置 PTS + 触发关键帧门控）
    for (const auto& srv : streamCtrl_->httpFlvServers()) {
        srv->videoQueue()->tryPush(nullptr);
        srv->audioQueue()->tryPush(nullptr);
    }
    for (int i = 0; i < (int)streamCtrl_->videoMuxQueues().size(); ++i) {
        streamCtrl_->videoMuxQueues()[i]->tryPush(nullptr);
        streamCtrl_->audioMuxQueues()[i]->tryPush(nullptr);
    }
    // 根据目标类型分别注册到 DemuxThread
    int localIdx = 0, remoteIdx = 0, httpFlvIdx = 0;
    for (const auto& dest : dests) {
        if (dest.type == StreamDestination::LocalFile) {
            player_->addLocalRecorder(streamCtrl_->recorders()[localIdx++].get());
        } else if (dest.type == StreamDestination::HttpFlv) {
            auto* srv = streamCtrl_->httpFlvServers()[httpFlvIdx++].get();
            player_->addRestreamVideoPacketQueue(srv->videoQueue());
            player_->addRestreamAudioPacketQueue(srv->audioQueue());
        } else {
            player_->addRestreamVideoPacketQueue(
                streamCtrl_->videoMuxQueues()[remoteIdx].get());
            player_->addRestreamAudioPacketQueue(
                streamCtrl_->audioMuxQueues()[remoteIdx].get());
            player_->addMuxThread(streamCtrl_->muxThreads()[remoteIdx].get());
            remoteIdx++;
        }
    }

    // 推流启动对齐：记录起始位置用于停止时的时长计算。
    // 不做 seek：DemuxThread 已在目标位置附近，seek 会触发 pre-target 帧跳过导致延迟。
    // HttpFlvServer/MuxThread 首个收到的帧自动设置 PTS base，无需 sentinel。
    double alignSec = player_->currentPositionSeconds();
    if (alignSec < 0.0) alignSec = currentPos_ / 1000.0;
    streamAlignSec_ = alignSec;
    qInfo() << "MainWindow: stream started, align=" << alignSec
            << "s (audioClock=" << player_->currentPositionSeconds() << ")";

    ui->actionStream->setText("停止推流");
    statusBar()->showMessage(
        QString("推流中（直通）→ %1 路目标  %2x%3")
            .arg(dests.size()).arg(vpar->width).arg(vpar->height), 0);
}

// 毫秒转 "MM:SS" 字符串，用于时间标签显示。
QString MainWindow::formatTime(int64_t ms) {
    int s = (int)(ms / 1000);
    return QString("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

// 显示"关于"对话框：版本信息、快捷键列表、GitHub 主页链接。
// 使用 open()（非阻塞）而非 exec()，避免嵌套事件循环导致 Qt slot 对象 UAF 崩溃。
void MainWindow::onAbout() {
    auto* dlg = new QMessageBox(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("关于 RambosPlayer");
    dlg->setTextFormat(Qt::RichText);
    dlg->setText(
        "<b>RambosPlayer v1.0.0</b><br>"
        "基于 FFmpeg + Qt 的多媒体播放器<br><br>"
        "<b>基本操作</b><br>"
        "<table cellspacing='4'>"
        "<tr><td><b>Ctrl+O</b></td><td>打开文件</td></tr>"
        "<tr><td><b>空格</b></td><td>播放 / 暂停</td></tr>"
        "<tr><td><b>← / →</b></td><td>快退 / 快进 5 秒</td></tr>"
        "<tr><td><b>双击视频</b></td><td>切换全屏</td></tr>"
        "<tr><td><b>Esc</b></td><td>退出全屏</td></tr>"
        "<tr><td><b>Ctrl+Shift+S</b></td><td>推流设置（HTTP-FLV / SRT / 本地录制）</td></tr>"
        "<tr><td><b>Ctrl+T</b></td><td>剪辑模式</td></tr>"
        "<tr><td><b>Ctrl+E</b></td><td>导出片段</td></tr>"
        "</table><br>"
        "<a href='https://github.com/johnjiamzhong-project/RambosPlayer'>"
        "https://github.com/johnjiamzhong-project/RambosPlayer</a>"
    );
    dlg->setStandardButtons(QMessageBox::Ok);
    dlg->open();
}
