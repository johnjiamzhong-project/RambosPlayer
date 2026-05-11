#pragma once
#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class VideoRenderer;
class PlayerController;

// MainWindow 是播放器的主窗口，包含视频渲染区（VideoRenderer）和底部控制栏。
// 控制栏布局全部定义在 mainwindow.ui 中；本类只负责信号连接和交互逻辑。
// 双击窗口切换全屏；进度条拖拽触发 seek；播放结束自动复位按钮状态。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onOpenFile();
    void onPlayPause();
    void onSeekSliderMoved(int value);
    void onVolumeChanged(int value);
    void onDurationChanged(int64_t ms);
    void onPositionChanged(int64_t ms);
    void onPlaybackFinished();
    void onClearRecent();
    void onHwAccelToggled(bool checked);

private:
    void openFile(const QString& path);         // 打开文件并更新最近记录
    void updateRecentFiles(const QString& path); // 写入 QSettings，刷新菜单
    void rebuildRecentMenu();                    // 用 QSettings 重建最近文件菜单条目

    Ui::MainWindow* ui;
    VideoRenderer*    renderer_;    // 指向 ui->videoWidget（promoted），不拥有所有权
    PlayerController* player_;      // 持有并控制完整播放流水线
    int64_t duration_ = 0;          // 当前文件总时长（毫秒），进度条换算用
    bool isFullscreen_ = false;     // 全屏状态标志

    static constexpr int MaxRecentFiles = 10;   // 最近文件列表最大条数
    static QString formatTime(int64_t ms);      // 毫秒 → "MM:SS" 字符串
};
