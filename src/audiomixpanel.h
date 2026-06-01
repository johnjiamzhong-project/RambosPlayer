#pragma once
#include <QWidget>
#include <QList>
#include <QBuffer>
#include <QTime>
#include <cstdint>

QT_BEGIN_NAMESPACE
namespace Ui { class AudioMixPanel; }
QT_END_NAMESPACE

class PlayerController;
class Timeline;
class AudioMixWorker;
class QAudioInput;
class QMediaPlayer;

// 一条音频混合区间的完整参数
struct AudioMixRegion {
    QString  sourcePath;              // 音频文件路径（录音为临时 WAV）
    QString  displayName;             // 列表显示名
    int64_t  videoStartUs    = 0;     // 贴入视频的时间（秒精度取整，单位微秒）
    int64_t  audioOffsetUs   = 0;     // 从音频文件的哪个位置开始（微秒，默认 0）
    int64_t  audioDurationUs = 0;     // 使用多少时长（微秒，探测或用户输入，>0）
    float    srcVol  = 1.0f;          // 该区间源视频音量（0.0–1.0）
    float    mixVol  = 0.0f;          // 新增音频音量（srcVol+mixVol=1.0）
    bool     isRecorded = false;      // true = 录音临时文件，面板关闭时删除
};

// AudioMixPanel：音频混合面板。
// 支持添加本地音频和实时录入音频，每段可设置贴入视频时间点、音量混合比例。
// 通过 AudioMixWorker 执行 FFmpeg amix 滤镜图混音，输出视频流直通复制的新视频文件。
// 使用 setSourceFile() 绑定源视频；始终从原始源混合避免多代质量损失。
class AudioMixPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioMixPanel(QWidget* parent = nullptr);
    ~AudioMixPanel() override;

    void setPlayerController(PlayerController* pc);
    void setTimeline(Timeline* tl);
    void setSourceFile(const QString& path);  // 由 MainWindow 在文件打开时调用
    void switchToLocalMode();                 // 菜单"本地音频"触发
    void switchToRecordMode();                // 菜单"实时录入"触发

signals:
    void sourceFileSelected(const QString& path);  // 用户在面板内选择了源视频，请求 MainWindow 打开

private slots:
    void onOpenSource();
    void onBrowseAudio();
    void onSrcVolChanged(int value);
    void onMixVolChanged(int value);
    void onAddRegion();
    void onRemoveRegion();
    void onListen();
    void onPreview();
    void onBrowseOutput();
    void onExport();
    void onRecStartStop();
    void onProgressed(int percent);
    void onWorkFinished(bool ok);
    void onError(const QString& msg);

private:
    void rebuildTable();
    void updateTimeline();
    void updateExportEnabled();
    void addRegionToList(const AudioMixRegion& r);
    void stopPreview();
    bool writeWav(const QString& path, const QByteArray& pcm,
                  int sampleRate, int channels, int bitsPerSample);
    static int64_t probeDurationUs(const QString& path, int64_t offsetUs = 0);

    Ui::AudioMixPanel* ui;
    PlayerController*  player_        = nullptr;
    Timeline*          timeline_      = nullptr;
    AudioMixWorker*    worker_        = nullptr;
    QAudioInput*       audioInput_    = nullptr;
    QMediaPlayer*      previewPlayer_ = nullptr;
    QBuffer            recordBuffer_;

    QString               sourceFile_;          // 始终指向原始源视频
    QList<AudioMixRegion> regions_;             // 已添加的区间（累积，导出从原始源重混）
    float                 prevVolume_ = 1.0f;         // 录音前保存的播放器音量
    float                 previewSavedVolume_ = 1.0f; // 试播放前保存的播放器音量
    int64_t  recordVideoStartUs_ = 0;           // 本次录音在视频中的起始时间
    bool     recording_          = false;       // 录音进行中标志
};
