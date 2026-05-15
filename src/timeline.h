#ifndef TIMELINE_H
#define TIMELINE_H

#include <QWidget>
#include <QImage>
#include <QList>

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
    void    setTrimRange(int64_t inUs, int64_t outUs);

signals:
    void trimPointChanged(int64_t inPts, int64_t outPts);

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

    QList<QImage> thumbnails_;
    int64_t duration_     = 0;
    int64_t inPts_        = 0;
    int64_t outPts_       = 0;
    int     handleDrag_   = -1;  // -1=无, 0=入口, 1=出口

    // 轨道区域（像素坐标），resizeEvent 时更新
    int     trackLeft_    = 10;
    int     trackRight_   = 10;
    int     rulerHeight_  = 20;
    int     trackTop_     = 24;
    int     trackHeight_  = 60;

    static constexpr int kHandleWidth  = 8;
    static constexpr int kHandleHeight = 20;
    static constexpr int kMinTrimUs    = 100000;   // 最短剪辑长度 0.1 秒
};

#endif // TIMELINE_H
