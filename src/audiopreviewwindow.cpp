#include "audiopreviewwindow.h"

#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QFileInfo>
#include <QIcon>

// 构造函数：创建无边框弹窗，布局标题栏 + 进度条 + 控制按钮
AudioPreviewWindow::AudioPreviewWindow(const QString& filePath, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setFixedSize(380, 130);
    setStyleSheet(
        "AudioPreviewWindow { background-color: #2d2d30; }"
        "QLabel { color: #e0e0e0; background: transparent; }"
        "QPushButton { background: #3e3e42; color: #e0e0e0; border: 1px solid #555;"
        "  border-radius: 4px; padding: 4px 10px; min-height: 22px; }"
        "QPushButton:hover { background: #505050; }"
        "QPushButton:pressed { background: #094771; }"
        "QSlider::groove:horizontal { background: #3e3e42; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #0078d4; width: 12px; height: 12px;"
        "  margin: -4px 0; border-radius: 6px; }"
        "QSlider::sub-page:horizontal { background: #0078d4; border-radius: 2px; }"
        "QSlider::add-page:horizontal { background: #3e3e42; border-radius: 2px; }"
    );

    // --- 标题栏 ---
    auto* titleBar = new QHBoxLayout();
    titleBar->setContentsMargins(8, 4, 8, 0);
    titleLabel_ = new QLabel(QFileInfo(filePath).fileName());
    titleLabel_->setStyleSheet("font-size: 13px; font-weight: bold; color: #e0e0e0;");
    titleBar->addWidget(titleLabel_, 1);

    auto* closeBtn = new QPushButton();
    closeBtn->setFixedSize(24, 24);
    closeBtn->setIcon(QIcon(":/icons/close_small.svg"));
    closeBtn->setIconSize(QSize(14, 14));
    closeBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; }"
        "QPushButton:hover { background: #c42b1c; border-radius: 3px; }");
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleBar->addWidget(closeBtn);

    // --- 进度条 + 时间标签 ---
    progressSlider_ = new QSlider(Qt::Horizontal);
    progressSlider_->setRange(0, 0);
    progressSlider_->installEventFilter(this);
    connect(progressSlider_, &QSlider::sliderPressed,  this, &AudioPreviewWindow::onSliderPressed);
    connect(progressSlider_, &QSlider::sliderReleased, this, &AudioPreviewWindow::onSliderReleased);

    timeLabel_ = new QLabel("00:00 / 00:00");
    timeLabel_->setStyleSheet("font-size: 11px; color: #999; background: transparent;");
    timeLabel_->setAlignment(Qt::AlignCenter);

    // --- 控制按钮（使用 SVG 图标）---
    auto* ctrlLayout = new QHBoxLayout();
    ctrlLayout->setContentsMargins(0, 0, 0, 0);
    ctrlLayout->setSpacing(8);

    auto* backBtn = new QPushButton();
    backBtn->setFixedSize(36, 28);
    backBtn->setIcon(QIcon(":/icons/skip_backward.svg"));
    backBtn->setIconSize(QSize(18, 18));
    connect(backBtn, &QPushButton::clicked, this, &AudioPreviewWindow::onBackward);

    playPauseBtn_ = new QPushButton();
    playPauseBtn_->setFixedSize(36, 28);
    playPauseBtn_->setIcon(QIcon(":/icons/play.svg"));
    playPauseBtn_->setIconSize(QSize(18, 18));
    connect(playPauseBtn_, &QPushButton::clicked, this, &AudioPreviewWindow::onPlayPause);

    auto* fwdBtn = new QPushButton();
    fwdBtn->setFixedSize(36, 28);
    fwdBtn->setIcon(QIcon(":/icons/skip_forward.svg"));
    fwdBtn->setIconSize(QSize(18, 18));
    connect(fwdBtn, &QPushButton::clicked, this, &AudioPreviewWindow::onForward);

    ctrlLayout->addStretch();
    ctrlLayout->addWidget(backBtn);
    ctrlLayout->addWidget(playPauseBtn_);
    ctrlLayout->addWidget(fwdBtn);
    ctrlLayout->addStretch();

    // --- 主布局 ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 8);
    mainLayout->setSpacing(6);
    mainLayout->addLayout(titleBar);
    mainLayout->addWidget(progressSlider_);
    mainLayout->addWidget(timeLabel_);
    mainLayout->addLayout(ctrlLayout);

    // --- 播放器 ---
    player_ = new QMediaPlayer(this);
    player_->setMedia(QUrl::fromLocalFile(filePath));
    connect(player_, &QMediaPlayer::positionChanged, this, &AudioPreviewWindow::onPositionChanged);
    connect(player_, &QMediaPlayer::durationChanged, this, &AudioPreviewWindow::onDurationChanged);
    connect(player_, &QMediaPlayer::stateChanged,    this, &AudioPreviewWindow::onStateChanged);

    // 窗口关闭时停止播放
    connect(this, &QWidget::destroyed, player_, &QMediaPlayer::stop);
}

AudioPreviewWindow::~AudioPreviewWindow()
{
    player_->stop();
}

// --- 播放控制 ---

void AudioPreviewWindow::onPlayPause()
{
    if (player_->state() == QMediaPlayer::PlayingState)
        player_->pause();
    else
        player_->play();
}

void AudioPreviewWindow::onBackward()
{
    player_->setPosition(qMax(0LL, player_->position() - 5000));
}

void AudioPreviewWindow::onForward()
{
    player_->setPosition(qMin(player_->duration(), player_->position() + 5000));
}

// --- 进度条同步 ---

void AudioPreviewWindow::onPositionChanged(qint64 pos)
{
    if (!sliderDragging_) {
        progressSlider_->setValue(static_cast<int>(pos / 1000));
    }
    timeLabel_->setText(formatTime(pos) + " / " + formatTime(player_->duration()));
}

void AudioPreviewWindow::onDurationChanged(qint64 dur)
{
    progressSlider_->setRange(0, static_cast<int>(dur / 1000));
}

void AudioPreviewWindow::onStateChanged(QMediaPlayer::State state)
{
    if (state == QMediaPlayer::PlayingState)
        playPauseBtn_->setIcon(QIcon(":/icons/pause.svg"));
    else
        playPauseBtn_->setIcon(QIcon(":/icons/play.svg"));
}

void AudioPreviewWindow::onSliderPressed()
{
    sliderDragging_ = true;
}

void AudioPreviewWindow::onSliderReleased()
{
    sliderDragging_ = false;
    player_->setPosition(static_cast<qint64>(progressSlider_->value()) * 1000);
}

// --- 窗口拖动 ---

void AudioPreviewWindow::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        moving_ = true;
        moveOffset_ = ev->globalPos() - frameGeometry().topLeft();
        ev->accept();
    }
}

void AudioPreviewWindow::mouseMoveEvent(QMouseEvent* ev)
{
    if (moving_ && (ev->buttons() & Qt::LeftButton)) {
        move(ev->globalPos() - moveOffset_);
        ev->accept();
    }
}

void AudioPreviewWindow::mouseReleaseEvent(QMouseEvent* ev)
{
    moving_ = false;
    QWidget::mouseReleaseEvent(ev);
}

// 事件过滤器：拦截进度条上的鼠标点击，直接跳转到点击位置
bool AudioPreviewWindow::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == progressSlider_ && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton && progressSlider_->maximum() > 0) {
            // 根据点击 x 坐标计算目标值
            double ratio = static_cast<double>(me->x()) / progressSlider_->width();
            int target = static_cast<int>(ratio * progressSlider_->maximum());
            progressSlider_->setValue(target);
            player_->setPosition(static_cast<qint64>(target) * 1000);
            return true;  // 拦截，不再走默认的 page step 行为
        }
    }
    return QWidget::eventFilter(obj, ev);
}

// --- 工具函数 ---

QString AudioPreviewWindow::formatTime(qint64 ms) const
{
    int s = static_cast<int>(ms / 1000);
    return QString("%1:%2")
        .arg(s / 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}
