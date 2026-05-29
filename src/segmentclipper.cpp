#include "segmentclipper.h"
#include "timeline.h"
#include "timeutil.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QListWidget>
#include <QPushButton>
#include <QMessageBox>
#include <QRegularExpression>
#include <QFont>

// ─────────────────────────────────────────────
// 内部工具：时间字符串 → 微秒
// 支持 MM:SS 和 HH:MM:SS 两种格式
// ─────────────────────────────────────────────
static bool parseTimeString(const QString& token, int64_t& us, QString& err)
{
    // 尝试 MM:SS 或 H:MM:SS
    static const QRegularExpression re1("^(\\d+):(\\d{2})$");
    // 尝试 HH:MM:SS 或 H:MM:SS
    static const QRegularExpression re2("^(\\d+):(\\d{2}):(\\d{2})$");

    QRegularExpressionMatch m;
    m = re2.match(token.trimmed());
    if (m.hasMatch()) {
        int h = m.captured(1).toInt();
        int mn = m.captured(2).toInt();
        int s = m.captured(3).toInt();
        if (mn >= 60 || s >= 60) {
            err = QString("时间超出范围: %1").arg(token.trimmed());
            return false;
        }
        us = (static_cast<int64_t>(h) * 3600 + mn * 60 + s) * 1000000;
        return true;
    }

    m = re1.match(token.trimmed());
    if (m.hasMatch()) {
        int mn = m.captured(1).toInt();
        int s = m.captured(2).toInt();
        if (mn >= 60 && m.captured(1).length() > 2) {
            // "120:30" 这种可能是 2小时0分30秒，当作 HH:MM 处理
        }
        if (s >= 60) {
            err = QString("秒数超出范围: %1").arg(token.trimmed());
            return false;
        }
        us = (static_cast<int64_t>(mn) * 60 + s) * 1000000;
        return true;
    }

    err = QString("无法识别的时间格式: %1  （应为 MM:SS 或 HH:MM:SS）").arg(token.trimmed());
    return false;
}

// ─────────────────────────────────────────────
// 构造函数：构建对话框 UI
// ─────────────────────────────────────────────
SegmentClipper::SegmentClipper(Timeline* timeline, int64_t durationUs,
                               QWidget* parent)
    : QDialog(parent), timeline_(timeline), durationUs_(durationUs)
{
    setWindowTitle("多段剪切");
    resize(520, 440);

    auto* mainLayout = new QVBoxLayout(this);

    // 说明标签
    auto* hint = new QLabel(
        QString("逐行输入时间区间，格式：MM:SS - MM:SS 或 HH:MM:SS - HH:MM:SS\n"
                "视频总时长: %1  每行一个区间，空行自动跳过")
            .arg(usToLabel(durationUs)));
    hint->setWordWrap(true);
    mainLayout->addWidget(hint);

    // 输入区域
    inputEdit_ = new QTextEdit(this);
    inputEdit_->setPlaceholderText(
        "00:10 - 00:25\n"
        "01:30 - 02:00\n"
        "1:05:00 - 1:10:30");
    inputEdit_->setMinimumHeight(100);
    QFont monoFont("Consolas", 10);
    monoFont.setStyleHint(QFont::Monospace);
    inputEdit_->setFont(monoFont);
    mainLayout->addWidget(inputEdit_);

    // 验证结果列表
    listWidget_ = new QListWidget(this);
    listWidget_->setMinimumHeight(120);
    mainLayout->addWidget(listWidget_);

    // 状态栏
    statusLabel_ = new QLabel("", this);
    mainLayout->addWidget(statusLabel_);

    // 按钮行
    auto* btnLayout = new QHBoxLayout();
    validateBtn_ = new QPushButton("验证", this);
    okBtn_ = new QPushButton("确定", this);
    okBtn_->setEnabled(false);
    auto* cancelBtn = new QPushButton("取消", this);

    btnLayout->addWidget(validateBtn_);
    btnLayout->addStretch();
    btnLayout->addWidget(okBtn_);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    // 信号连接
    connect(validateBtn_, &QPushButton::clicked, this, &SegmentClipper::onValidate);
    connect(okBtn_, &QPushButton::clicked, this, &SegmentClipper::onAccept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    // 文本变化时重置验证状态，避免用户修改后不点验证直接点确定
    connect(inputEdit_, &QTextEdit::textChanged, this, [this]() {
        validated_ = false;
        okBtn_->setEnabled(false);
    });
}

// ─────────────────────────────────────────────
// 解析单行文本 → 起止微秒
// ─────────────────────────────────────────────
bool SegmentClipper::parseLine(const QString& line, int64_t& startUs,
                               int64_t& endUs, QString& errorMsg) const
{
    // 分隔符：- → , 两侧可选空格
    static const QRegularExpression sep("[\\-→,，]");
    QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        errorMsg = "空行";
        return false;
    }

    QStringList parts = trimmed.split(sep, Qt::SkipEmptyParts);
    if (parts.size() != 2) {
        errorMsg = QString("无法解析: \"%1\"  （需要两个时间，用 - → 或 , 分隔）").arg(trimmed);
        return false;
    }

    int64_t s = 0, e = 0;
    QString err1, err2;
    if (!parseTimeString(parts[0], s, err1)) {
        errorMsg = err1;
        return false;
    }
    if (!parseTimeString(parts[1], e, err2)) {
        errorMsg = err2;
        return false;
    }
    if (s >= e) {
        errorMsg = QString("起始时间 (%1) 必须早于结束时间 (%2)")
                       .arg(usToLabel(s)).arg(usToLabel(e));
        return false;
    }
    if (e > durationUs_) {
        errorMsg = QString("结束时间 (%1) 超出视频总时长 (%2)")
                       .arg(usToLabel(e)).arg(usToLabel(durationUs_));
        return false;
    }

    startUs = s;
    endUs   = e;
    return true;
}

// ─────────────────────────────────────────────
// "验证"按钮：解析所有非空行，更新列表显示
// ─────────────────────────────────────────────
void SegmentClipper::onValidate()
{
    parsedSegments_.clear();
    listWidget_->clear();

    QStringList lines = inputEdit_->toPlainText().split('\n');
    int validCount = 0;
    int errorCount = 0;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;  // 跳过空行

        SegmentInfo info;
        info.originalText = trimmed;

        int64_t s = 0, e = 0;
        QString err;
        if (parseLine(trimmed, s, e, err)) {
            info.valid = true;
            info.startUs = s;
            info.endUs   = e;
            // 检查与已解析的区间是否重叠
            for (const auto& existing : parsedSegments_) {
                if (existing.valid && s < existing.endUs && e > existing.startUs) {
                    info.valid = false;
                    info.errorMsg = QString("与已有区间 %1 → %2 重叠")
                                        .arg(usToLabel(existing.startUs))
                                        .arg(usToLabel(existing.endUs));
                    break;
                }
            }
        } else {
            info.valid = false;
            info.errorMsg = err;
        }

        parsedSegments_.append(info);
        if (info.valid)
            validCount++;
        else
            errorCount++;
    }

    refreshList();

    bool allValid = (errorCount == 0 && validCount > 0);
    okBtn_->setEnabled(allValid);
    validated_ = true;

    if (allValid) {
        statusLabel_->setText(QString("✓ 全部 %1 个区间验证通过").arg(validCount));
        statusLabel_->setStyleSheet("color: green;");
    } else {
        statusLabel_->setText(QString("✗ %1 个错误，%2 个通过").arg(errorCount).arg(validCount));
        statusLabel_->setStyleSheet("color: red;");
    }
}

// ─────────────────────────────────────────────
// 刷新验证结果列表的显示
// ─────────────────────────────────────────────
void SegmentClipper::refreshList()
{
    listWidget_->clear();
    for (const auto& info : parsedSegments_) {
        QString text;
        if (info.valid) {
            int64_t dur = info.endUs - info.startUs;
            QString durStr;
            int totalSec = static_cast<int>(dur / 1000000);
            if (totalSec >= 60)
                durStr = QString("%1分%2秒").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QChar('0'));
            else
                durStr = QString("%1秒").arg(totalSec);
            text = QString("✓ %1 → %2  (%3)")
                       .arg(usToLabel(info.startUs))
                       .arg(usToLabel(info.endUs))
                       .arg(durStr);
        } else {
            text = QString("✗ %1").arg(info.errorMsg);
        }

        auto* item = new QListWidgetItem(text);
        if (info.valid)
            item->setForeground(QColor(0, 160, 0));
        else
            item->setForeground(QColor(200, 50, 50));
        listWidget_->addItem(item);
    }
}

// ─────────────────────────────────────────────
// "确定"按钮：填充 Timeline 底部导轨
// ─────────────────────────────────────────────
void SegmentClipper::onAccept()
{
    if (!validated_) {
        onValidate();
        if (!okBtn_->isEnabled())
            return;
    }

    // 清空现有区间
    timeline_->clearSegments();
    timeline_->setBottomBarVisible(true);

    int added = 0;
    for (const auto& info : parsedSegments_) {
        if (info.valid) {
            timeline_->addSegment(info.startUs, info.endUs);
            added++;
        }
    }

    statusLabel_->setText(QString("已添加 %1 个区间到底部导轨").arg(added));
    QDialog::accept();
}
