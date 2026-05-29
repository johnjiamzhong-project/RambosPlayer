#ifndef TIMELINE_H
#define TIMELINE_H

#include <QWidget>
#include <QImage>
#include <QList>
#include <QPair>

// Timeline：视频剪辑时间轴控件。
// 上部绘制时间刻度尺，中部绘制缩略图轨道，左右各一个可拖拽的剪辑把手。
// 拖拽把手时发射 trimPointChanged(inPts, outPts) 信号（单位：微秒）。
class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(QWidget* parent = nullptr);

    void setDuration(int64_t durationUs);
    void setThumbnails(const QList<QImage>& images);
    void addThumbnail(const QImage& image);   // 逐张追加，即时刷新

    int64_t inPts() const;
    int64_t outPts() const;
    int64_t duration() const { return duration_; }
    void    setTrimRange(int64_t inUs, int64_t outUs);

    // 底部导轨 — 多段剪切区间管理
    bool addSegment(int64_t startUs, int64_t endUs);  // 返回 false 表示重叠被拒绝
    bool removeSegment(int64_t startUs, int64_t endUs); // 按起始+结束时间精确匹配移除
    bool removeSegmentAt(int index);                    // 按索引移除，不受边界漂移影响
    bool mergeSegment(int64_t startUs, int64_t endUs); // 合并重叠区间，返回 true 表示合并成功
    void clearSegments();
    QList<QPair<int64_t, int64_t>> segments() const;
    void setBottomBarVisible(bool visible);
    bool isBottomBarVisible() const;

    // 待定入点标记（浏览剪切：首次空格立刻显示，二次空格确认区间后清除）
    void setPendingInPoint(int64_t pts);
    void clearPendingInPoint();

    // 把手显隐控制（自由剪辑显示，浏览剪切/多段剪切隐藏）
    void setHandlesVisible(bool visible);
    bool areHandlesVisible() const { return handlesVisible_; }

signals:
    void trimPointChanged(int64_t inPts, int64_t outPts);
    void segmentsChanged(int count);  // 底部导轨区间数变化时发出

protected:
    void paintEvent(QPaintEvent*) override;                                                                                                                                                                                                                            
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    int     ptsToX(int64_t pts) const;
    int64_t xToPts(int x) const;
    QRect   handleRect(bool isInHandle) const;
    void    drawRuler(QPainter& p);
    void    drawThumbnails(QPainter& p);
    void    drawHandles(QPainter& p);
    void    drawBottomBar(QPainter& p);
    void    mergeAdjacent();

    QList<QImage> thumbnails_;          // 均匀分布在轨道上的视频缩略图列表
    int64_t duration_     = 0;          // 视频总时长（微秒），0 表示未加载
    int64_t inPts_        = 0;          // 入口剪辑点（微秒）
    int64_t outPts_       = 0;          // 出口剪辑点（微秒）
    int     handleDrag_   = -1;         // -1=无, 0=入口, 1=出口

    // 轨道区域（像素坐标），resizeEvent 时更新
    int     trackLeft_    = 10;
    int     trackRight_   = 10;
    int     rulerHeight_  = 20;
    int     trackTop_     = 24;
    int     trackHeight_  = 60;

    // 底部导轨（多段区间显示）
    QList<QPair<int64_t, int64_t>> segments_;    // 已标记区间列表（按 startUs 排序）
    int64_t pendingInPts_    = -1;                // 待定入点（-1=无），浏览剪切首次空格设置
    bool   bottomBarVisible_ = false;             // 底部导轨是否可见
    bool   handlesVisible_   = true;              // 把手可见（自由剪辑显示，其他模式隐藏）
    static constexpr int kBottomBarHeight = 24;   // 底部导轨高度

    static constexpr int kHandleWidth  = 8;
    static constexpr int kHandleHeight = 20;
    static constexpr int kMinTrimUs    = 100000;   // 最短剪辑长度 0.1 秒
};

#endif // TIMELINE_H
