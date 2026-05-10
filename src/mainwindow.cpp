#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "videorenderer.h"
#include "playercontroller.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>

// 构造函数：setupUi 完成所有控件创建和布局，此处只做指针绑定、初始值和信号连接。
// renderer_ 直接取 ui->videoWidget（promoted VideoRenderer），无需手动 setCentralWidget。
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);

    renderer_ = ui->videoWidget;
    player_   = new PlayerController(renderer_);  // 不挂 parent，由析构函数手动控制销毁顺序

    player_->setVolume(ui->volumeSlider->value() / 100.0f);  // 与滑块初始值 80 保持一致

    connect(ui->actionOpen,     &QAction::triggered,    this, &MainWindow::onOpenFile);
    connect(ui->playPauseBtn,   &QPushButton::clicked,  this, &MainWindow::onPlayPause);
    connect(ui->progressSlider, &QSlider::sliderMoved, this, &MainWindow::onSeekSliderMoved);
    ui->progressSlider->installEventFilter(this);
    connect(ui->volumeSlider,   &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);

    connect(player_, &PlayerController::durationChanged,  this, &MainWindow::onDurationChanged);
    connect(player_, &PlayerController::positionChanged,  this, &MainWindow::onPositionChanged);
    connect(player_, &PlayerController::playbackFinished, this, &MainWindow::onPlaybackFinished);
    connect(ui->actionClearRecent, &QAction::triggered,   this, &MainWindow::onClearRecent);

    rebuildRecentMenu();
}

// 析构函数：必须先 delete player_ 再 delete ui。
// player_->stop() 会调用 renderer_->stopRendering()，renderer_ 是 ui 内的 promoted widget，
// 若先 delete ui 则 renderer_ 已销毁，player_ 析构时再访问它会触发 UAF 崩溃。
MainWindow::~MainWindow() {
    delete player_;   // stop() 在 renderer_ 还存活时执行
    player_ = nullptr;
    delete ui;        // VideoRenderer 随 ui 安全销毁，此时 pendingFrame_ 已为 null
}

// 打开文件并开始播放，同时更新最近文件记录。供对话框和最近文件菜单共用。
void MainWindow::openFile(const QString& path) {
    player_->stop();
    if (player_->open(path)) {
        updateRecentFiles(path);
        ui->playPauseBtn->setText("⏸");
        player_->play();
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

// 重建最近文件子菜单：先删除分隔线之前的动态条目，再按列表顺序插入新条目。
void MainWindow::rebuildRecentMenu() {
    QSettings s("RambosPlayer", "RambosPlayer");
    QStringList files = s.value("recentFiles").toStringList();

    // 找到菜单中的固定分隔线（动态条目与"清除记录"之间的边界）
    QAction* sep = nullptr;
    for (QAction* a : ui->menuRecentFiles->actions()) {
        if (a->isSeparator()) { sep = a; break; }
    }

    // 删除分隔线之前所有旧的动态文件条目
    for (QAction* a : ui->menuRecentFiles->actions()) {
        if (a == sep) break;
        ui->menuRecentFiles->removeAction(a);
        delete a;
    }

    // 在分隔线之前按顺序插入新条目
    for (int i = 0; i < files.size(); ++i) {
        QString label = QString("&%1  %2").arg(i + 1)
                            .arg(QFileInfo(files[i]).fileName());
        QAction* a = new QAction(label, ui->menuRecentFiles);
        a->setToolTip(files[i]);   // 悬停时显示完整路径
        connect(a, &QAction::triggered, this, [this, path = files[i]]{ openFile(path); });
        if (sep)
            ui->menuRecentFiles->insertAction(sep, a);
        else
            ui->menuRecentFiles->addAction(a);
    }

    bool hasFiles = !files.isEmpty();
    ui->menuRecentFiles->setEnabled(hasFiles);
    ui->actionClearRecent->setEnabled(hasFiles);
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
        ui->playPauseBtn->setText("▶");
    } else {
        player_->play();
        ui->playPauseBtn->setText("⏸");
    }
}

// 进度条拖拽时触发 seek，value 范围 0–1000 映射到 0–duration 秒。
void MainWindow::onSeekSliderMoved(int value) {
    if (duration_ <= 0) return;
    double seconds = (double)value / 1000.0 * duration_ / 1000.0;
    player_->seek(seconds);
}

// 音量滑块变化，value 范围 0–100 映射到 0.0–1.0。
void MainWindow::onVolumeChanged(int value) {
    player_->setVolume(value / 100.0f);
}

// 收到文件时长后设置标签右半部分，进度条最大值固定 1000 无需更改。
void MainWindow::onDurationChanged(int64_t ms) {
    duration_ = ms;
    ui->timeLabel->setText("00:00 / " + formatTime(ms));
}

// 每 100ms 更新进度条和时间标签；用户正在拖拽时跳过进度条更新，避免跳动。
void MainWindow::onPositionChanged(int64_t ms) {
    if (!ui->progressSlider->isSliderDown() && duration_ > 0)
        ui->progressSlider->setValue((int)((double)ms / duration_ * 1000));
    ui->timeLabel->setText(formatTime(ms) + " / " + formatTime(duration_));
}

// 播放结束后复位按钮为播放状态。
void MainWindow::onPlaybackFinished() {
    ui->playPauseBtn->setText("▶");
}

// 拦截进度条鼠标按下：用 sliderValueFromPosition 算出精确点击位置并立即 seek。
// 不拦截其他控件，也不消费事件（返回 false），让 Qt 照常处理后续拖拽逻辑。
bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == ui->progressSlider && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            int val = QStyle::sliderValueFromPosition(
                ui->progressSlider->minimum(),
                ui->progressSlider->maximum(),
                me->x(), ui->progressSlider->width());
            ui->progressSlider->setValue(val);
            onSeekSliderMoved(val);
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// 双击切换全屏/窗口模式。
void MainWindow::mouseDoubleClickEvent(QMouseEvent*) {
    isFullscreen_ = !isFullscreen_;
    isFullscreen_ ? showFullScreen() : showNormal();
}

// 毫秒转 "MM:SS" 字符串，用于时间标签显示。
QString MainWindow::formatTime(int64_t ms) {
    int s = (int)(ms / 1000);
    return QString("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}
