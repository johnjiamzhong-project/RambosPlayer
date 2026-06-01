#pragma once
#include <QWidget>
#include <QMediaPlayer>

QT_BEGIN_NAMESPACE
class QLabel;
class QSlider;
class QPushButton;
QT_END_NAMESPACE

// AudioPreviewWindow：简洁的音频试听弹窗。
// 无边框窗口，自定义标题栏显示文件名和关闭按钮，
// 中间是可拖动的播放进度条，底部是后退/播放暂停/前进控制。
// 通过 QMediaPlayer 解码播放，定时器刷新进度。
class AudioPreviewWindow : public QWidget {
    Q_OBJECT
public:
    explicit AudioPreviewWindow(const QString& filePath, QWidget* parent = nullptr);
    ~AudioPreviewWindow() override;

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onPlayPause();
    void onBackward();
    void onForward();
    void onPositionChanged(qint64 pos);
    void onDurationChanged(qint64 dur);
    void onStateChanged(QMediaPlayer::State state);
    void onSliderPressed();
    void onSliderReleased();

private:
    QString formatTime(qint64 ms) const;

    QMediaPlayer*  player_       = nullptr;
    QLabel*        titleLabel_   = nullptr;
    QLabel*        timeLabel_    = nullptr;
    QSlider*       progressSlider_ = nullptr;
    QPushButton*   playPauseBtn_ = nullptr;
    bool           sliderDragging_ = false;  // 用户拖动进度条时不更新位置
    bool           moving_       = false;    // 窗口拖动
    QPoint         moveOffset_;              // 拖动偏移
};
