#pragma once
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class FilterPanel; }
QT_END_NAMESPACE

class QDockWidget;
class PlayerController;

// FilterPanel 是滤镜调参面板（QDockWidget），所有控件在 .ui 中绘制。
// 滑块变化时通过 PlayerController 转发到 VideoDecodeThread 的 atomic 参数；
// 滤镜重建在解码线程中自动完成。
class FilterPanel : public QWidget {
    Q_OBJECT
public:
    explicit FilterPanel(PlayerController* player, QWidget* parent = nullptr);
    ~FilterPanel() override;

private slots:
    void onEnabledChanged(int state);
    void onBrightnessChanged(int value);
    void onContrastChanged(int value);
    void onSaturationChanged(int value);
    void onBrowseWatermark();

private:
    Ui::FilterPanel* ui;
    PlayerController* player_;
};
