#include "timeline.h"
#include "timeutil.h"
#include <QPainter>
#include <QMouseEvent>
#include <QtMath>
#include <QFontMetrics>
#include <QDebug>

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
    if (durationUs <= 0)
        return;
    duration_ = durationUs;
    if (outPts_ == 0)  // 首次设置才初始化出口到末尾
        outPts_ = durationUs;
    qInfo() << "Timeline::setDuration" << durationUs / 1000000.0 << "s";
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

// 将时间戳（微秒）映射到轨道水平像素坐标。
// range 提升为 int64_t 后再乘，避免 pts * range 在除法前溢出 int。
int Timeline::ptsToX(int64_t pts) const
{
    if (duration_ <= 0)
        return trackLeft_;
    int64_t range = trackRight_ - trackLeft_;
    return trackLeft_ + static_cast<int>(pts * range / duration_);
}

// 将轨道水平像素坐标反算为时间戳（微秒），结果钳位到 [0, duration_]。
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

// 返回指定把手（入口/出口）的鼠标响应矩形区域。
// 区域覆盖整条竖线高度，方便鼠标精确抓取。
QRect Timeline::handleRect(bool isInHandle) const
{
    int x = ptsToX(isInHandle ? inPts_ : outPts_);
    // 点击区域覆盖整条竖线（从轨道顶部到底部把手），方便鼠标抓取
    return QRect(x - kHandleWidth / 2, trackTop_,
                 kHandleWidth, trackHeight_ + kHandleHeight);
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

// usToLabel 由 timeutil.h 提供（inline），通过 #include 使用即可
// 本文件不再定义静态 usToLabel，避免重复定义

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

// 绘制整个时间轴控件：背景、轨道底色、选中高亮、刻度尺、缩略图、
// 把手（自由剪辑模式）和底部导轨（多段区间/浏览剪切模式）。
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

    // 把手可见时绘制选中区域高亮和把手（自由剪辑模式）
    if (handlesVisible_) {
        int selLeft  = ptsToX(inPts_);
        int selRight = ptsToX(outPts_);
        if (selRight > selLeft) {
            p.fillRect(QRect(selLeft, trackTop_, selRight - selLeft, trackHeight_),
                       QColor(60, 120, 200, 80));
        }
    }

    // 轨道边框
    p.setPen(QPen(QColor(80, 80, 85), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(trackRect);

    drawRuler(p);
    drawThumbnails(p);
    if (handlesVisible_)
        drawHandles(p);
    drawBottomBar(p);
}

// 鼠标按下：判断是否点中入口或出口把手，记录拖拽目标并切换光标。
// 非自由剪辑模式（把手隐藏时）透传给父类。
void Timeline::mousePressEvent(QMouseEvent* ev)
{
    if (!handlesVisible_) { QWidget::mousePressEvent(ev); return; }
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

// 鼠标移动：拖拽中更新入口/出口时间点并发射 trimPointChanged；
// 未拖拽时做悬停检测，悬停到把手区域时切换为水平缩放光标。
void Timeline::mouseMoveEvent(QMouseEvent* ev)
{
    if (!handlesVisible_) { QWidget::mouseMoveEvent(ev); return; }
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

// 鼠标释放：结束拖拽，恢复箭头光标，打印最终剪辑点到日志。
void Timeline::mouseReleaseEvent(QMouseEvent*)
{
    if (handleDrag_ >= 0)
        qInfo() << "Timeline: trim" << (handleDrag_ == 0 ? "in" : "out")
                << "set to" << (handleDrag_ == 0 ? inPts_ : outPts_) / 1000000.0 << "s"
                << "range=" << (outPts_ - inPts_) / 1000000.0 << "s";
    handleDrag_ = -1;
    setCursor(Qt::ArrowCursor);
}

// 控件大小变化时更新右侧轨道边界坐标。
void Timeline::resizeEvent(QResizeEvent*)
{
    trackRight_ = width() - 10;
}

// ---- 底部导轨（多段区间） ----

// 向底部导轨添加区间（微秒），插入后按起始时间排序。
// 与已有区间重叠时拒绝并返回 false，由调用方决定是否弹出合并对话框。
bool Timeline::addSegment(int64_t startUs, int64_t endUs)
{
    if (startUs >= endUs || startUs < 0 || endUs > duration_)
        return false;
    // 拒绝与已有区间重叠
    for (const auto& seg : segments_) {
        if (startUs < seg.second && endUs > seg.first)
            return false;
    }
    segments_.append(qMakePair(startUs, endUs));
    std::sort(segments_.begin(), segments_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    update();
    emit segmentsChanged(segments_.size());
    return true;
}

// 按精确起止时间移除单个区间，找不到则返回 false。
// 注意：mergeSegment 会改变区间边界，之后用原始参数调用此方法会找不到。
// 边界已漂移的场景请用 removeSegmentAt(index) 替代。
bool Timeline::removeSegment(int64_t startUs, int64_t endUs)
{
    auto it = std::find_if(segments_.begin(), segments_.end(),
        [&](const auto& seg) { return seg.first == startUs && seg.second == endUs; });
    if (it != segments_.end()) {
        segments_.erase(it);
        update();
        emit segmentsChanged(segments_.size());
        return true;
    }
    return false;
}

// 按索引移除区间，不受边界漂移影响。
// 调用方需确保 index 有效（通常由 dialog 中勾选状态推导）。
bool Timeline::removeSegmentAt(int index)
{
    if (index < 0 || index >= segments_.size())
        return false;
    segments_.erase(segments_.begin() + index);
    update();
    emit segmentsChanged(segments_.size());
    return true;
}

// 将新区间合并到已有的重叠区间中（扩展边界），合并后调用 mergeAdjacent() 消除连锁重叠。
// 若与所有已有区间均不重叠则返回 false（不新增区间）。
bool Timeline::mergeSegment(int64_t startUs, int64_t endUs)
{
    if (startUs >= endUs || startUs < 0 || endUs > duration_)
        return false;
    for (auto& seg : segments_) {
        if (startUs < seg.second && endUs > seg.first) {
            // 扩展已有区间覆盖两者
            seg.first  = qMin(seg.first, startUs);
            seg.second = qMax(seg.second, endUs);
            // 合并后可能与后续区间再次重叠（如 A(0-10) B(5-15) C(12-20)
            // 合并 A(0-15) 后与 C 重叠，需递归合并
            mergeAdjacent();
            update();
            emit segmentsChanged(segments_.size());
            return true;
        }
    }
    return false;
}

// 合并相邻或重叠的区间（排序后遍历合并）。
// 仅合并真正重叠的区间（A 的结束 > B 的起始），
// 首尾恰好相接的区间（如 0-10 和 10-20）保持独立，不合并。
void Timeline::mergeAdjacent()
{
    if (segments_.size() < 2) return;
    std::sort(segments_.begin(), segments_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    QList<QPair<int64_t, int64_t>> merged;
    merged.append(segments_.first());
    for (int i = 1; i < segments_.size(); ++i) {
        auto& last = merged.last();
        if (segments_[i].first < last.second) {
            // 真正重叠（非相邻），合并
            last.second = qMax(last.second, segments_[i].second);
        } else {
            merged.append(segments_[i]);
        }
    }
    segments_ = merged;
}

// 清空所有底部导轨区间并触发重绘。
void Timeline::clearSegments()
{
    segments_.clear();
    update();
    emit segmentsChanged(0);
}

// 返回当前所有已标记区间的副本（按起始时间排序）。
QList<QPair<int64_t, int64_t>> Timeline::segments() const
{
    return segments_;
}

// 控制底部导轨（多段区间显示层）的可见性，并同步调整控件最小高度。
void Timeline::setBottomBarVisible(bool visible)
{
    bottomBarVisible_ = visible;
    setMinimumHeight(visible ? 140 : 110);
    update();
}

// 返回底部导轨当前是否可见。
bool Timeline::isBottomBarVisible() const
{
    return bottomBarVisible_;
}

// 控制剪辑把手（入口/出口竖线与三角）的可见性。
// 自由剪辑模式显示，浏览剪切/多段剪切模式隐藏，隐藏时把手坐标保留不变。
void Timeline::setHandlesVisible(bool visible)
{
    handlesVisible_ = visible;
    update();
}

// 设置待定入点（浏览剪切首次空格时调用），底部导轨立即绘制绿色竖线标记。
void Timeline::setPendingInPoint(int64_t pts)
{
    pendingInPts_ = pts;
    update();
}

// 清除待定入点标记（区间确认后或模式退出时调用），底部导轨绿色竖线消失。
void Timeline::clearPendingInPoint()
{
    pendingInPts_ = -1;
    update();
}

// 由 AudioMixPanel 调用，设置音频混合区间（微秒对 {start, end}）
void Timeline::setAudioRegions(const QList<QPair<int64_t, int64_t>>& regions)
{
    audioRegions_ = regions;
    update();
}

// 绘制底部导轨：显示所有已标记区间的色块 + 时间标签
void Timeline::drawBottomBar(QPainter& p)
{
    bool hasContent = !segments_.isEmpty() || pendingInPts_ >= 0 || !audioRegions_.isEmpty();
    if (!bottomBarVisible_ || !hasContent)
        return;

    int barTop = trackTop_ + trackHeight_ + kHandleHeight + 6;
    int barH   = kBottomBarHeight;

    // 底部导轨背景
    p.fillRect(trackLeft_, barTop, trackRight_ - trackLeft_, barH, QColor(35, 35, 38));
    p.setPen(QPen(QColor(70, 70, 75), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(trackLeft_, barTop, trackRight_ - trackLeft_, barH);

    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);

    for (const auto& seg : segments_) {
        int x1 = ptsToX(seg.first);
        int x2 = ptsToX(seg.second);
        if (x2 <= x1) continue;

        // 区间色块
        p.fillRect(x1, barTop + 3, x2 - x1, barH - 6, QColor(80, 160, 80, 140));
        p.setPen(QPen(QColor(80, 200, 80), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(x1, barTop + 3, x2 - x1, barH - 6);

        // 时间标签
        QString label = usToLabel(seg.first) + " - " + usToLabel(seg.second);
        p.setPen(QColor(200, 200, 200));
        int tw = QFontMetrics(f).horizontalAdvance(label);
        int tx = x1 + (x2 - x1 - tw) / 2;
        if (tx < trackLeft_) tx = trackLeft_;
        if (tx + tw > trackRight_) tx = trackRight_ - tw;
        p.drawText(tx, barTop + barH - 6, label);
    }

    // 待定入点标记：绿色竖线 + 三角 + 时间标签
    if (pendingInPts_ >= 0) {
        int x = ptsToX(pendingInPts_);
        if (x >= trackLeft_ && x <= trackRight_) {
            QColor green(0, 220, 80);
            p.setPen(QPen(green, 2));
            p.drawLine(x, barTop, x, barTop + barH);

            // 小三角
            QPoint tri[3] = {
                QPoint(x, barTop),
                QPoint(x - 5, barTop - 4),
                QPoint(x + 5, barTop - 4)
            };
            p.setBrush(green);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri, 3);

            // 时间标签
            p.setPen(green);
            p.setFont(f);
            QString label = usToLabel(pendingInPts_);
            int tw = QFontMetrics(f).horizontalAdvance(label);
            int tx = qBound(trackLeft_, x - tw / 2, trackRight_ - tw);
            p.drawText(tx, barTop + barH - 4, label);
        }
    }

    // 音频混合区间行（紫色，在绿色剪切区间行下方 2px 处）
    if (!audioRegions_.isEmpty()) {
        int aTop = barTop + barH + 2;
        int aH   = kAudioBarHeight;

        p.fillRect(trackLeft_, aTop, trackRight_ - trackLeft_, aH, QColor(28, 25, 45));
        p.setPen(QPen(QColor(80, 65, 110), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(trackLeft_, aTop, trackRight_ - trackLeft_, aH);

        QFont af = p.font();
        af.setPixelSize(9);
        p.setFont(af);

        for (const auto& ar : audioRegions_) {
            int x1 = ptsToX(ar.first);
            int x2 = ptsToX(ar.second);
            if (x2 <= x1) continue;

            p.fillRect(x1, aTop + 2, x2 - x1, aH - 4, QColor(120, 75, 200, 160));
            p.setPen(QPen(QColor(160, 110, 240), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(x1, aTop + 2, x2 - x1, aH - 4);

            // 时间标签（起始时间）
            QString lbl = usToLabel(ar.first);
            p.setPen(QColor(210, 195, 240));
            int tw = QFontMetrics(af).horizontalAdvance(lbl);
            int tx = x1 + 2;
            if (tx + tw > trackRight_) tx = qMax(trackLeft_, trackRight_ - tw);
            p.drawText(tx, aTop + aH - 4, lbl);
        }
    }
}
