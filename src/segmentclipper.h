#pragma once
#include <QDialog>
#include <QList>
#include <QPair>

class QTextEdit;
class QListWidget;
class QLabel;
class QPushButton;
class Timeline;

// SegmentClipper：多段剪切输入对话框。
// 用户逐行输入时间区间（MM:SS - MM:SS 或 HH:MM:SS - HH:MM:SS），
// 验证通过后填充 Timeline 底部导轨。
class SegmentClipper : public QDialog {
    Q_OBJECT
public:
    explicit SegmentClipper(Timeline* timeline, int64_t durationUs,
                            QWidget* parent = nullptr);

private slots:
    void onValidate();
    void onAccept();

private:
    // 解析一行文本为起止时间（微秒），成功返回 true
    bool parseLine(const QString& line, int64_t& startUs, int64_t& endUs,
                   QString& errorMsg) const;
    void refreshList();

    Timeline* timeline_;
    int64_t durationUs_;

    QTextEdit*   inputEdit_;
    QListWidget* listWidget_;
    QLabel*      statusLabel_;
    QPushButton* validateBtn_;
    QPushButton* okBtn_;

    struct SegmentInfo {
        int64_t startUs;
        int64_t endUs;
        QString originalText;
        bool    valid;
        QString errorMsg;
    };
    QList<SegmentInfo> parsedSegments_;
    bool validated_ = false;
};
