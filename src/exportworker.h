#ifndef EXPORTWORKER_H
#define EXPORTWORKER_H

#include <QThread>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
}

// ExportWorker：无损剪切导出线程。
// run(inputPath, outputPath, inPts, outPts) 以 -c copy 方式透传数据流，
// 不重编码，速度只受磁盘 I/O 限制。进度通过 progressed(current, total) 信号上报。
class ExportWorker : public QThread {
    Q_OBJECT
public:
    explicit ExportWorker(QObject* parent = nullptr);
    ~ExportWorker() override;

    void run(const QString& inputPath, const QString& outputPath,
             int64_t inPts, int64_t outPts);

signals:
    void progressed(int64_t currentPts, int64_t totalPts);
    void exportFinished(bool ok);
    void errorOccurred(const QString& msg);

protected:
    void run() override;

private:
    QString  inputPath_;
    QString  outputPath_;
    int64_t  inPts_;
    int64_t  outPts_;
};

#endif // EXPORTWORKER_H
