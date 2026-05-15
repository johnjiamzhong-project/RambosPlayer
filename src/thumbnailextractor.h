#ifndef THUMBNAILEXTRACTOR_H
#define THUMBNAILEXTRACTOR_H

#include <QThread>
#include <QImage>
#include <QList>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// ThumbnailExtractor：异步提取视频关键帧缩略图。
// extract() 后自动启动线程，完成时通过 thumbnailsReady 信号返回 QImage 列表。
// 每个缩略图对应视频时长均匀分布在 [duration/(N+1), 2*duration/(N+1), ...] 位置。
class ThumbnailExtractor : public QThread {
    Q_OBJECT
public:
    explicit ThumbnailExtractor(QObject* parent = nullptr);
    ~ThumbnailExtractor() override;

    void extract(const QString& path, int count = 8);

signals:
    void thumbnailReady(const QImage& image);      // 逐张送达，UI 即时更新
    void thumbnailsReady(const QList<QImage>& images); // 全部完成
    void errorOccurred(const QString& msg);

protected:
    void run() override;

private:
    QImage frameToImage(AVFrame* frame, AVCodecContext* codecCtx, int w, int h);

    QString  path_;
    int      count_;
};

#endif // THUMBNAILEXTRACTOR_H
