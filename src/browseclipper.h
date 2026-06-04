#pragma once
#include <QObject>
#include <QList>
#include <QPair>

class PlayerController;
class Timeline;

// BrowseClipper：浏览剪辑控制器。
// 播放中按空格标记入点/出点，自动追加区间到底部导轨；
// 退出时弹出确认对话框，用户选择保留的区间段。
class BrowseClipper : public QObject {
    Q_OBJECT
public:
    explicit BrowseClipper(PlayerController* player, Timeline* timeline, QObject* parent = nullptr);

    void start();           // 进入浏览剪辑模式，自动播放
    void stop(bool showDialog = true);  // 退出浏览剪辑模式；showDialog=true 弹出确认对话框
    bool isActive() const;
    void markPoint();       // 空格键：标记入点或出点

signals:
    void finished();        // 退出浏览模式（含取消）

private:
    enum State { Idle, Marking };
    PlayerController* player_;
    Timeline* timeline_;
    State state_ = Idle;
    bool active_ = false;
    int64_t inPts_ = 0;     // 当前入点（微秒）
    // segments_ 记录本次会话标记的区间，用于退出时确认对话框
    QList<QPair<int64_t, int64_t>> markedSegments_;
};
