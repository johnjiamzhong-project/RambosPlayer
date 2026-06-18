#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <atomic>
#include "framequeue.h"
#include "avsync.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// VideoRenderer 是视频渲染组件，继承 QWidget。
// 由 1ms QTimer 驱动 onTimer()，每次从 FrameQueue<AVFrame*> 取一帧，
// 查询 AVSync::videoDelay() 决定等待或丢弃，然后用 sws_scale 将
// YUV420P 转换为 RGB32 写入 QImage，并调用 update() 触发 paintEvent()。
// paintEvent() 用 QPainter 保持宽高比居中绘制，背景填充黑色。
class VideoRenderer : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderer(QWidget* parent = nullptr);
    ~VideoRenderer() override;

    void init(int width, int height, AVRational timeBase,
              AVSync* sync, FrameQueue<AVFrame*>* frameQueue);
    void startRendering();
    void stopRendering();
    void flushPendingFrame();   // seek 时清除残留的 pendingFrame_，防止旧帧卡住队列
    void renderOneFrame();      // seek while paused 时由 PlayerController 延迟调用，强制渲染一帧刷新画面

signals:
    // 落后直播源严重超出本地小队列能追赶的范围（见 kForceReconnectBehindSec）时发出。
    // PlayerController 据此触发 DemuxThread 强制重连，直接拿到服务端当前最新 GOP，
    // 而不是指望本地这几帧的追赶逻辑能处理任意大小的、堆在系统/服务端缓冲区里看不见的积压。
    void fellBehindLive(double behindSec);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTimer();

private:
    QImage currentFrame_;               // 当前待绘制的 RGB32 帧
    QMutex frameMutex_;                 // 保护 currentFrame_ 的读写
    QTimer* timer_ = nullptr;           // 1ms 定时器，驱动帧拉取
    AVSync* sync_ = nullptr;            // 音频主时钟，用于计算视频延迟
    FrameQueue<AVFrame*>* frameQueue_ = nullptr; // 来自 VideoDecodeThread 的帧队列
    SwsContext* swsCtx_ = nullptr;      // sws 上下文，srcFormat_ → RGB32
    AVPixelFormat srcFormat_ = AV_PIX_FMT_YUV420P; // 当前源帧格式，硬解时自动切换 NV12
    int srcW_ = 0, srcH_ = 0;          // 视频原始宽高，用于宽高比计算
    AVRational timeBase_{1, 1};         // 视频流时间基，用于 pts → 秒换算
    AVFrame* pendingFrame_ = nullptr;   // 未到渲染时间的帧，暂存避免推回队列阻塞主线程

    QElapsedTimer noFrameTimer_;        // 计量"队列连续空"的时长，用于打印无帧警告
    bool noFrameTimerStarted_ = false;  // noFrameTimer_ 是否已启动
    bool noFrameLogged_ = false;        // 避免同一段空窗重复打印
    int  dropCount_ = 0;                // 当前轮连续丢帧数，汇总后打印
    double lastRenderedPts_ = -1.0;     // 上次成功渲染的帧 PTS，用于检测 seek 后首帧
    bool rendering_ = false;            // startRendering/stopRendering 状态标志，onTimer() 据此提前返回

    // 纯视频直播节拍控制（无音频时钟时启用）。
    // SRS 以 GOP 为单位缓冲投递，8 帧会在 <10ms 内一次性到达，VideoRenderer
    // 若不加控制会瞬间全部渲完，然后冻屏 ~533ms 等下一个 GOP，视觉上就是卡顿。
    // 以第一帧挂钟时间为基准，按 (pts - startPts)*1000ms 决定每帧何时渲染。
    QElapsedTimer livePacingTimer_;     // 节拍基准挂钟（上一帧渲染时刻 start()，每帧重锚）
    double livePacingStartPts_ = -1.0; // 节拍基准 PTS（秒），-1 表示未初始化

    // 落后追赶阈值：本帧 PTS 相对节拍基准的滞后量（actualMs - expectedMs）超过此值时，
    // 判定为"长时间未被正常调度积压了大量帧"（例如窗口被最小化导致 GUI 线程的 1ms
    // 定时器被系统限流），需要丢弃队列中积压的旧帧直接跳到最新帧，而不是按 1x 速度
    // 把积压逐帧播完。复用 livePacingTimer_/livePacingStartPts_ 而非单独长期锚点，
    // 因为它们每帧都重新校准，不会像固定起点的锚点一样被编码帧率与 PTS timebase
    // 间的微小偏差（参考 docs/BUGFIX-LOG.md #047）长期累积出虚假的"落后"判定。
    static constexpr double kCatchUpBehindSec = 1.0;

    // 强制重连阈值：落后超过这个量级，说明积压已经远超本地队列（videoFrameQ_ 只有
    // 10 帧≈0.4s）能装的范围，靠本地"丢队列里的旧帧"这套追赶逻辑没法处理（追的是
    // 看不见的服务端/系统缓冲区积压），改为发出 fellBehindLive 信号请求整条流重连，
    // 直接拿 SRS 当前最新 GOP，干净地回到直播边缘。
    static constexpr double kForceReconnectBehindSec = 5.0;
    bool reconnectRequested_ = false; // 防止追赶期间（可能持续多个 1ms tick）重复发 fellBehindLive

    // 长期漂移检测：livePacingStartPts_ 每帧重新校准，结构上只能看到"相对上一帧"
    // 的滞后，看不到"每帧都只差一点点、但持续累积"的缓慢漂移（例如窗口被最小化后
    // 进程被系统降权，解码/渲染没有完全停摆，只是持续比实时慢一点）。这里用一个
    // 独立的、每隔 kDriftCheckWindowSec 重新校准一次的基准来测这种累积漂移；
    // 定期重新校准（而非像 #047 那样固定第一帧不变）是为了让编码帧率与 PTS
    // timebase 间约 1% 的固有偏差最多只在一个窗口内累积（几百毫秒），不会跨窗口
    // 累积出虚假判定，同时仍能在真实落后达到阈值前检测出来。
    QElapsedTimer driftAnchorTimer_;
    double driftAnchorPts_ = -1.0;
    static constexpr double kDriftCheckWindowSec = 10.0;

    // 诊断用看门狗：独立于 GUI 事件循环的后台线程，周期性检查 onTimer() 是否
    // 还在被正常调度。用来直接证明"窗口最小化/被遮挡导致 GUI 线程的 1ms 定时器
    // 停止调度"这一猜测，而不是只能从 pts 跳变间接推断。
    std::atomic<qint64> lastOnTimerEpochMs_{0}; // onTimer() 每次被调度时更新为当前挂钟时间（ms）
    QThread* watchdog_ = nullptr;               // 看门狗线程，startRendering() 创建，stopRendering() 销毁
    void startWatchdog();
    void stopWatchdog();
};
