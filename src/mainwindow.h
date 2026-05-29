#pragma once
#include <QMainWindow>
#include <QList>
#include <QPair>
#include <QElapsedTimer>
#include "streamcontroller.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class VideoRenderer;
class PlayerController;
class FilterPanel;
class StreamController;
class Timeline;
class ThumbnailExtractor;
class ExportWorker;
class BrowseClipper;
class QDockWidget;

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
    void onFilterPanelToggled(bool checked);
    void onStreamStart();
    void onTrimModeToggled(bool checked);
    void onBrowseClipToggled(bool checked);
    void onSegmentClipTriggered();
    void onExportTriggered();
    void onThumbnailsReady(const QList<QImage>& images);
    void onExportProgress(int64_t currentPts, int64_t totalPts);
    void onExportFinished(bool ok);
    // startNextBatchExport 已移除 — 批量导出改用 ExportWorker::runBatch() 内部循环
    void onAbout();

private:
    void openFile(const QString& path);                             // 打开文件并更新最近记录
    void updateRecentFiles(const QString& path);                    // 写入 QSettings，刷新菜单
    void rebuildRecentMenu();                                       // 用 QSettings 重建最近文件菜单条目
    void startStreaming(const QList<StreamDestination>& dests);     // 启动推流管线（需文件已打开）
    void reconnectStreaming();                                       // 恢复播放时重新接入推流
    void prepareMpegTsSeek(double seconds);                          // 通知低延迟推流丢弃 seek 预滚输出

    Ui::MainWindow* ui;
    VideoRenderer*    renderer_;    // 指向 ui->videoWidget（promoted），不拥有所有权
    PlayerController* player_;      // 持有并控制完整播放流水线
    QDockWidget*     filterDock_  = nullptr; // 滤镜面板的 Dock 容器
    FilterPanel*     filterPanel_ = nullptr; // 滤镜调参面板
    StreamController*   streamCtrl_    = nullptr; // 推流控制器
    QDockWidget*        trimDock_      = nullptr; // 剪辑时间轴 Dock 容器
    Timeline*           timeline_      = nullptr; // 剪辑时间轴控件
    ThumbnailExtractor* thumbExtractor_ = nullptr; // 缩略图异步提取
    ExportWorker*       exportWorker_  = nullptr; // 无损剪切导出线程
    BrowseClipper*      browseClipper_ = nullptr; // 浏览剪切控制器
    // 批量导出状态
    int     batchExportIndex_ = 0;  // 当前导出到第几个（UI 进度显示用）
    int     batchExportTotal_ = 0;  // 总区间数
    QStringList batchExportLog_;    // 日志行缓存，全部完成后写入
    QElapsedTimer exportTimer_;     // 导出耗时计时器

    void writeExportLog();          // 写入导出记录到文件
    QString             currentFile_;              // 当前打开的文件路径
    int64_t duration_ = 0;          // 当前文件总时长（毫秒），进度条换算用
    int64_t currentPos_ = 0;        // 当前播放位置（毫秒），键盘快进/快退用
    bool isFullscreen_ = false;     // 全屏状态标志
    bool switchingClipMode_ = false; // 剪辑模式切换中，防止 trimDock_ visibility 循环触发
    QList<StreamDestination> pendingDests_;        // 文件打开前预配置的推流目标，openFile 后自动启动
    QList<StreamDestination> activeDests_;         // 当前正在推流的目标列表（暂停恢复时需要阻塞标志）
    double streamAlignSec_ = 0.0;                 // 推流 seek 对齐的起始秒数，用于计算截止时长

    static constexpr int MaxRecentFiles = 10;   // 最近文件列表最大条数
    static QString formatTime(int64_t ms);      // 毫秒 → "MM:SS" 字符串
};
