#include "browseclipper.h"
#include "timeutil.h"
#include "playercontroller.h"
#include "timeline.h"
#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QMainWindow>
#include <QDebug>

// 构造函数：绑定播放控制器与时间轴，不做其他初始化（状态由 start() 重置）。
BrowseClipper::BrowseClipper(PlayerController* player, Timeline* timeline, QObject* parent)
    : QObject(parent), player_(player), timeline_(timeline)
{
}

// 进入浏览剪切模式：清空本次会话的区间列表，显示底部导轨，自动播放视频，
// 并在状态栏提示用户按空格标记入点。
void BrowseClipper::start()
{
    active_ = true;
    state_ = Idle;
    markedSegments_.clear();
    timeline_->setBottomBarVisible(true);

    if (player_->isOpened()) {
        player_->play();
        QMainWindow* mw = qobject_cast<QMainWindow*>(parent());
        if (mw && mw->statusBar())
            mw->statusBar()->showMessage("浏览剪切模式 — 按空格标记入点 (视频持续播放)");
    }
}

// 退出浏览剪切模式：清除待定入点标记，若有已标记区间则弹出确认对话框，
// 用户可勾选要保留的区间；全部丢弃则从时间轴移除所有本次会话区间。
// 最终发射 finished() 信号通知主窗口更新菜单状态。
void BrowseClipper::stop()
{
    active_ = false;
    state_ = Idle;
    timeline_->clearPendingInPoint();

    if (markedSegments_.isEmpty()) {
        timeline_->setBottomBarVisible(false);
        QMainWindow* mw = qobject_cast<QMainWindow*>(parent());
        if (mw && mw->statusBar())
            mw->statusBar()->showMessage("", 3000);
        emit finished();
        return;
    }

    // 弹出确认对话框：列出所有已标记区间，可勾选保留
    QDialog dlg(qobject_cast<QWidget*>(parent()));
    dlg.setWindowTitle("浏览剪切 — 确认区间");
    dlg.resize(400, 250);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QString("共标记 %1 个区间，勾选要保留的段：").arg(markedSegments_.size())));

    auto* list = new QListWidget(&dlg);
    for (const auto& seg : markedSegments_) {
        auto* item = new QListWidgetItem(
            QString("%1 → %2  (%3秒)")
                .arg(usToLabel(seg.first))
                .arg(usToLabel(seg.second))
                .arg((seg.second - seg.first) / 1000000));
        item->setCheckState(Qt::Checked);
        list->addItem(item);
    }
    layout->addWidget(list);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btns->button(QDialogButtonBox::Ok)->setText("确认");
    btns->button(QDialogButtonBox::Cancel)->setText("全部丢弃");
    layout->addWidget(btns);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        // 移除未勾选的浏览区间，已勾选的保留在 timeline 中（不动，避免重叠检测误杀）
        for (int i = 0; i < list->count() && i < markedSegments_.size(); ++i) {
            if (list->item(i)->checkState() == Qt::Unchecked) {
                timeline_->removeSegment(markedSegments_[i].first, markedSegments_[i].second);
            }
        }
        if (timeline_->segments().isEmpty())
            timeline_->setBottomBarVisible(false);
    } else {
        // 全部丢弃：移除所有本次浏览会话标记的区间
        for (const auto& seg : markedSegments_)
            timeline_->removeSegment(seg.first, seg.second);
        if (timeline_->segments().isEmpty())
            timeline_->setBottomBarVisible(false);
    }

    QMainWindow* mw = qobject_cast<QMainWindow*>(parent());
    if (mw && mw->statusBar())
        mw->statusBar()->showMessage("已退出浏览剪切模式", 3000);

    emit finished();
}

// 返回当前是否处于浏览剪切激活状态。
bool BrowseClipper::isActive() const
{
    return active_;
}

// 响应空格键：状态为 Idle 时标记入点（底部导轨显示绿色竖线），
// 状态为 Marking 时标记出点并提交区间；出点若不晚于入点则重置状态并提示。
// 区间重叠时弹出三选框（合并/丢弃/取消）。
void BrowseClipper::markPoint()
{
    if (!active_ || !player_->isOpened())
        return;

    double posSec = player_->currentPositionSeconds();
    int64_t pts = static_cast<int64_t>(posSec * 1000000);

    QMainWindow* mw = qobject_cast<QMainWindow*>(parent());
    QString label = usToLabel(pts);

    if (state_ == Idle) {
        // 标记入点 → 底部导轨立刻显示绿色竖线标记
        inPts_ = pts;
        state_ = Marking;
        timeline_->setPendingInPoint(inPts_);
        if (mw && mw->statusBar())
            mw->statusBar()->showMessage(
                QString("▶ 入点: %1  |  按空格标记出点...").arg(label));
    } else {
        // 标记出点，追加区间
        if (pts <= inPts_) {
            // 出点不晚于入点：重置状态，清除待定入点标记，让用户重新标记
            state_ = Idle;
            timeline_->clearPendingInPoint();
            if (mw && mw->statusBar())
                mw->statusBar()->showMessage(
                    QString("⚠ 出点 (%1) 必须在入点 (%2) 之后，请重新标记").arg(label).arg(usToLabel(inPts_)), 3000);
            return;
        }
        timeline_->clearPendingInPoint();
        if (timeline_->addSegment(inPts_, pts)) {
            markedSegments_.append(qMakePair(inPts_, pts));
            if (mw && mw->statusBar())
                mw->statusBar()->showMessage(
                    QString("✓ 区间 %1 → %2  已标记 (%3秒)  |  按空格继续标记")
                        .arg(usToLabel(inPts_))
                        .arg(label)
                        .arg((pts - inPts_) / 1000000));
        } else {
            // 重叠：自定义按钮询问合并/丢弃/取消
            QMessageBox box(qobject_cast<QWidget*>(parent()));
            box.setWindowTitle("区间重叠");
            box.setText(QString("区间 %1 → %2 与已有区间重叠，请选择：")
                            .arg(usToLabel(inPts_)).arg(label));
            auto* btnMerge  = box.addButton("合并到已有区间", QMessageBox::YesRole);
            auto* btnDiscard = box.addButton("丢弃此段", QMessageBox::NoRole);
            auto* btnCancel = box.addButton("不标记此段", QMessageBox::RejectRole);
            box.setDefaultButton(btnMerge);
            box.exec();

            if (box.clickedButton() == btnMerge) {
                timeline_->mergeSegment(inPts_, pts);
                // 合并后边界已被扩展，不再追加原始区间到 markedSegments_，
                // 否则退出对话框时 removeSegment(原始边界) 会因边界不匹配而删除失败。
                // 合并段在退出对话框中不可单独勾选（始终保留）。
                if (mw && mw->statusBar())
                    mw->statusBar()->showMessage(
                        QString("⊕ 区间已合并到已有区间 (%1 秒)")
                            .arg((pts - inPts_) / 1000000));
            } else if (box.clickedButton() == btnDiscard) {
                if (mw && mw->statusBar())
                    mw->statusBar()->showMessage(
                        QString("✕ 已丢弃区间 %1 → %2").arg(usToLabel(inPts_)).arg(label), 3000);
            }
            // 不标记此段 → 不追加，状态重置，等同于取消标记
        }
        state_ = Idle;
    }
}
