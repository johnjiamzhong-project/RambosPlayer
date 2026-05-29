#ifndef EXPORTWORKER_H
#define EXPORTWORKER_H

#include <QThread>
#include <QString>
#include <QList>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// 单个导出片段
struct ExportSegment {
    QString outputPath;
    int64_t inPts;   // 微秒
    int64_t outPts;  // 微秒
};

// ExportWorker：帧精确重编码导出线程。
// 单段导出: run(inputPath, outputPath, inPts, outPts)
// 批量导出: runBatch(inputPath, segments) — 一次打开源文件，多段复用解码器
// 视频：H.264（libx264 CRF 17 / NVENC CQP 18），音频：AAC 192kbps。
class ExportWorker : public QThread {
    Q_OBJECT
public:
    explicit ExportWorker(QObject* parent = nullptr);
    ~ExportWorker() override;

    // 单段导出
    void run(const QString& inputPath, const QString& outputPath,
             int64_t inPts, int64_t outPts);
    // 批量导出：一次运行处理多段，复用输入文件和解码器
    void runBatch(const QString& inputPath, const QList<ExportSegment>& segments);

signals:
    void progressed(int64_t currentPts, int64_t totalPts);
    void segmentCompleted(int index);     // 批量模式：单段完成
    void exportFinished(bool ok);
    void errorOccurred(const QString& msg);

protected:
    void run() override;

private:
    // 处理单个片段（创建输出+编码器 → seek → 编码 → flush → 关闭输出）
    bool processSegment(class AVFormatContext* inCtx,
                        int videoStreamIdx, class AVCodecContext* vDecCtx,
                        int audioStreamIdx, class AVCodecContext* aDecCtx,
                        int srcWidth, int srcHeight,
                        AVRational videoTimeBase, AVRational audioTimeBase,
                        AVRational frameRate,
                        class SwsContext* swsCtx, class AVFrame* swsFrame,
                        struct AVBufferRef* hwDeviceCtx,
                        const QString& outPath, int64_t inPts, int64_t outPts,
                        int segIdx, int totalSegs, int64_t& outFrameCount);

    // 单段模式参数
    QString  inputPath_;
    QString  outputPath_;
    int64_t  inPts_;
    int64_t  outPts_;

    // 批量模式参数
    QList<ExportSegment> batchSegments_;
    bool isBatch_ = false;
};

#endif // EXPORTWORKER_H
