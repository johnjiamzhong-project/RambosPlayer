#include "timeline.h"
#include <QPainter>
#include <QMouseEvent>
#include <QtMath>
#include <QFontMetrics>

Timeline::Timeline(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    outPts_ = duration_;
}

void Timeline::setDuration(int64_t durationUs)
{
    duration_ = durationUs;
    if (outPts_ == 0)  // 首次设置才初始化出口到末尾
        outPts_ = durationUs;
    update();
}

void Timeline::setThumbnails(const QList<QImage>& images)
{
    thumbnails_ = images;
    update();
}

void Timeline::addThumbnail(const QImage& image)
{
    thumbnails_.append(image);
    update();
}

int64_t Timeline::inPts() const  { return inPts_; }
int64_t Timeline::outPts() const { return outPts_; }

void Timeline::setTrimRange(int64_t inUs, int64_t outUs)
{
    inPts_  = qBound<int64_t>(0, inUs, duration_);
    outPts_ = qBound<int64_t>(0, outUs, duration_);
    update();
}

int Timeline::ptsToX(int64_t pts) const
{
    if (duration_ <= 0)
        return trackLeft_;
    int range = trackRight_ - trackLeft_;
    return trackLeft_ + static_cast<int>(pts * range / duration_);
}

int64_t Timeline::xToPts(int x) const
{
    if (duration_ <= 0)
        return 0;
    int range = trackRight_ - trackLeft_;
    if (range <= 0)
        return 0;
    int64_t pts = static_cast<int64_t>(x - trackLeft_) * duration_ / range;
    return qBound<int64_t>(0, pts, duration_);
}

QRect Timeline::handleRect(bool isInHandle) const
{
    int x = ptsToX(isInHandle ? inPts_ : outPts_);
    return QRect(x - kHandleWidth / 2, trackTop_ + trackHeight_,
                 kHandleWidth, kHandleHeight);
}

// 绘制时间刻度尺（自适应刻度间距）
void Timeline::drawRuler(QPainter& p)
{
    if (duration_ <= 0)
        return;

    p.save();
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(QColor(200, 200, 200));

    int    range     = trackRight_ - trackLeft_;
    double seconds   = duration_ / 1000000.0;
    double tickStep  = 5.0;        // 默认 5 秒一格
    int    minTicks  = 4;
    int    maxTicks  = 20;

    // 自适应刻度间距
    while (seconds / tickStep > maxTicks)  tickStep *= 2;
    while (seconds / tickStep < minTicks && tickStep > 1.0) tickStep /= 2;

    int    fontH     = QFontMetrics(p.font()).height();
    double tickUs    = tickStep * 1000000.0;
    double firstTick = 0;

    for (double us = firstTick; us <= duration_ + tickUs / 2; us += tickUs) {
        int x = ptsToX(static_cast<int64_t>(us));
        if (x < trackLeft_ || x > trackRight_)
            continue;

        p.drawLine(x, rulerHeight_ - 4, x, rulerHeight_);

        int mins = static_cast<int>(us / 60000000);
        int secs = static_cast<int>(us / 1000000) % 60;
        QString label = QString("%1:%2")
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));
        int tw = QFontMetrics(p.font()).horizontalAdvance(label);
        p.drawText(x - tw / 2, rulerHeight_ - 6 - fontH / 2, label);
    }

    p.restore();
}

// 绘制缩略图轨道（均匀分布在轨道区域）
void Timeline::drawThumbnails(QPainter& p)
{
    if (thumbnails_.isEmpty())
        return;

    int range   = trackRight_ - trackLeft_;
    int count   = thumbnails_.size();
    int cellW   = range / count;
    int cellH   = trackHeight_;
    int drawW   = cellW - 2;

    if (drawW < 4)
        return;  // 太窄，不画

    for (int i = 0; i < count; ++i) {
        int x = trackLeft_ + i * cellW + 1;
        QRect cell(x, trackTop_, drawW, cellH);
        p.drawImage(cell, thumbnails_[i].scaled(drawW, cellH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        p.setPen(QColor(80, 80, 80));
        p.drawRect(cell);
    }
}

// 微秒 → "MM:SS" 或 "HH:MM:SS" 字符串
static QString usToLabel(int64_t us)
{
    int totalSec = static_cast<int>(us / 1000000);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

// 绘制入口/出口把手（三角形 + 竖线 + 时间标签）
void Timeline::drawHandles(QPainter& p)
{
    auto drawOne = [&](int64_t pts, bool isIn) {
        int x = ptsToX(pts);
        if (x < trackLeft_ || x > trackRight_)
            return;

        QColor color = isIn ? QColor(0, 200, 80) : QColor(220, 50, 50);
        p.setBrush(color);
        p.setPen(Qt::NoPen);

        // 竖线
        p.drawRect(x - 1, trackTop_, 2, trackHeight_ + kHandleHeight);

        // 三角形把手
        QPoint tri[3];
        int ty = trackTop_ + trackHeight_;
        if (isIn) {
            tri[0] = QPoint(x, ty + kHandleHeight);          // 左下
            tri[1] = QPoint(x + kHandleWidth, ty);           // 右上
            tri[2] = QPoint(x, ty);                          // 左上
        } else {
            tri[0] = QPoint(x, ty + kHandleHeight);          // 右下
            tri[1] = QPoint(x - kHandleWidth, ty);           // 左上
            tri[2] = QPoint(x, ty);                          // 右上
        }
        p.drawPolygon(tri, 3);

        // 时间标签（把手上方）
        p.setPen(color);
        QFont f = p.font();
        f.setPixelSize(10);
        p.setFont(f);
        QString label = usToLabel(pts);
        int tw = QFontMetrics(f).horizontalAdvance(label);
        int tx = qBound(trackLeft_, x - tw / 2, trackRight_ - tw);
        p.drawText(tx, trackTop_ - 2, label);
    };

    drawOne(inPts_, true);
    drawOne(outPts_, false);
}

void Timeline::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 背景
    p.fillRect(rect(), QColor(45, 45, 48));

    // 轨道底色
    QRect trackRect(trackLeft_, trackTop_, trackRight_ - trackLeft_, trackHeight_);
    p.fillRect(trackRect, QColor(30, 30, 32));

    // 无缩略图时显示提示文字
    if (thumbnails_.isEmpty() && duration_ > 0) {
        p.setPen(QColor(120, 120, 120));
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        p.drawText(trackRect, Qt::AlignCenter, "正在生成缩略图...");
    }

    // 已选中区域高亮
    int selLeft  = ptsToX(inPts_);
    int selRight = ptsToX(outPts_);
    if (selRight > selLeft) {
        p.fillRect(QRect(selLeft, trackTop_, selRight - selLeft, trackHeight_),
                   QColor(60, 120, 200, 80));
    }

    // 轨道边框
    p.setPen(QPen(QColor(80, 80, 85), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(trackRect);

    drawRuler(p);
    drawThumbnails(p);
    drawHandles(p);
}

void Timeline::mousePressEvent(QMouseEvent* ev)
{
    QRect inR  = handleRect(true);
    QRect outR = handleRect(false);

    if (inR.contains(ev->pos())) {
        handleDrag_ = 0;
        setCursor(Qt::SizeHorCursor);
    } else if (outR.contains(ev->pos())) {
        handleDrag_ = 1;
        setCursor(Qt::SizeHorCursor);
    }
}

void Timeline::mouseMoveEvent(QMouseEvent* ev)
{
    if (handleDrag_ < 0) {
        // 悬停检测
        QRect inR  = handleRect(true);
        QRect outR = handleRect(false);
        if (inR.contains(ev->pos()) || outR.contains(ev->pos()))
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(Qt::ArrowCursor);
        return;
    }

    int64_t newPts = xToPts(ev->x());

    if (handleDrag_ == 0) {
        // 入口把手：不能超过出口，留最小间隔
        inPts_ = qBound<int64_t>(0, newPts, outPts_ - kMinTrimUs);
    } else {
        // 出口把手：不能小于入口
        outPts_ = qBound<int64_t>(inPts_ + kMinTrimUs, newPts, duration_);
    }

    update();
    emit trimPointChanged(inPts_, outPts_);
}

void Timeline::mouseReleaseEvent(QMouseEvent*)
{
    handleDrag_ = -1;
    setCursor(Qt::ArrowCursor);
}

void Timeline::resizeEvent(QResizeEvent*)
{
    trackRight_ = width() - 10;
}
