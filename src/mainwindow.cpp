#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "videorenderer.h"
#include "playercontroller.h"
#include <QFileDialog>

// 构造函数：setupUi 完成所有控件创建和布局，此处只做指针绑定、初始值和信号连接。
// renderer_ 直接取 ui->videoWidget（promoted VideoRenderer），无需手动 setCentralWidget。
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);

    renderer_ = ui->videoWidget;
    player_   = new PlayerController(renderer_, this);

    player_->setVolume(ui->volumeSlider->value() / 100.0f);  // 与滑块初始值 80 保持一致

    connect(ui->actionOpen,     &QAction::triggered,    this, &MainWindow::onOpenFile);
    connect(ui->playPauseBtn,   &QPushButton::clicked,  this, &MainWindow::onPlayPause);
    connect(ui->progressSlider, &QSlider::sliderMoved,  this, &MainWindow::onSeekSliderMoved);
    connect(ui->volumeSlider,   &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);

    connect(player_, &PlayerController::durationChanged,  this, &MainWindow::onDurationChanged);
    connect(player_, &PlayerController::positionChanged,  this, &MainWindow::onPositionChanged);
    connect(player_, &PlayerController::playbackFinished, this, &MainWindow::onPlaybackFinished);
}

// 析构函数：删除 ui，Qt 负责销毁 player_ 和 renderer_（均以 this 为 parent）。
MainWindow::~MainWindow() { delete ui; }

// 打开文件对话框，选中文件后停止当前播放并立即开始新文件。
void MainWindow::onOpenFile() {
    QString path = QFileDialog::getOpenFileName(
        this, "打开文件", "",
        "视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)");
    if (path.isEmpty()) return;
    player_->stop();
    if (player_->open(path)) {
        ui->playPauseBtn->setText("⏸");
        player_->play();
    }
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
