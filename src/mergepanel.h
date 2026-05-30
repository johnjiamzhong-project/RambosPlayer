#pragma once
#include <QWidget>
#include <QVector>
#include <QMap>

QT_BEGIN_NAMESPACE
namespace Ui { class MergePanel; }
QT_END_NAMESPACE

class MergeWorker;

// MergePanel：合并/混音面板。
// 支持三种模式（拼接视频 / 音频混音 / 音视频合流）的文件输入和任务调度。
// 文件列表支持外部文件拖入和内部排序；混音模式下可对每路文件单独设音量。
// 进度和日志通过 MergeWorker 的信号实时更新，运行期间禁用所有输入控件。
class MergePanel : public QWidget {
    Q_OBJECT
public:
    explicit MergePanel(QWidget* parent = nullptr);
    ~MergePanel() override;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onModeChanged(int index);
    void onAddFile();
    void onRemoveFile();
    void onClearFiles();
    void onFileSelectionChanged();
    void onVolumeChanged(int value);
    void onBrowseOutput();
    void onStart();
    void onCancel();
    void onProgressed(int percent);
    void onMergeFinished(bool ok);
    void onError(const QString& msg);

private:
    void updateStartEnabled();
    void setRunning(bool running);
    void appendLog(const QString& msg);
    QStringList filePaths() const;      // 按列表顺序返回所有文件路径
    double volumeFor(int row) const;    // 第 row 行的音量权重（默认 1.0）

    Ui::MergePanel* ui;
    MergeWorker*    worker_  = nullptr;

    QMap<int, double> volumes_;   // row → 音量权重（仅混音模式有意义）
};
