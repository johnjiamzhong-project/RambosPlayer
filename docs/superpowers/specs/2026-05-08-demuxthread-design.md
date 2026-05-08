# DemuxThread 设计文档

**日期：** 2026-05-08
**范围：** Task 4 — `src/demuxthread.h/.cpp` + `tests/tst_demuxthread.cpp`

---

## 职责

DemuxThread 是一个 `QThread` 子类，负责：

1. 用 `avformat_open_input` / `avformat_find_stream_info` 打开媒体文件
2. 在 `run()` 循环里调 `av_read_frame`，按流索引将 `AVPacket*` 分发到视频包队列或音频包队列
3. 响应外部的 `stop()` 和 `seek()` 请求，线程安全地退出或跳转

DemuxThread **不负责**解码，也不拥有解码器。

---

## 公共接口

```cpp
class DemuxThread : public QThread {
public:
    ~DemuxThread() override;

    bool open(const QString& path,
              FrameQueue<AVPacket*>* videoQueue,
              FrameQueue<AVPacket*>* audioQueue);
    void stop();
    void seek(double seconds);   // 秒，原子存储，run() 下次循环生效

    int64_t duration() const;            // 单位：微秒（AV_TIME_BASE）
    int videoStreamIdx() const;
    int audioStreamIdx() const;
    AVFormatContext* formatContext() const;  // PlayerController 用于取 codecpar/time_base

signals:
    void finished();

protected:
    void run() override;
};
```

**说明：**
- `formatContext()` 暴露裸指针，生命周期由 `DemuxThread` 管理（析构时 `avformat_close_input`）。调用方不得持有超过 `DemuxThread` 生命周期的引用。
- `duration()` 返回 `fmtCtx_->duration`（微秒），调用方除以 1000 转毫秒。

---

## 线程生命周期与停止机制

### 正常退出（EOF）

```
run() 循环
  av_read_frame() 返回 < 0  →  break
  av_packet_free(&pkt)
  emit finished()
```

### 外部 stop()

```
stop()
  abort_ = true
  videoQueue_->abort()    // wakeAll notFull_ + notEmpty_
  audioQueue_->abort()    // 解锁任何阻塞在 push/tryPop 的线程

run() 下次循环顶部 while (!abort_)  →  退出
```

**push 阻塞场景：** 若 `run()` 正阻塞在 `videoQueue_->push(p)`（队列满），`queue->abort()` 会通过 `notFull_.wakeAll()` 解锁，`push` 内部判断 `aborted_` 后直接 return，循环继续到 `!abort_` 检查后退出。

**`av_read_frame` 阻塞：** 本阶段仅处理本地文件，`av_read_frame` 不会长时间阻塞，无需 `interrupt_callback`。网络流阶段再补。

---

## Seek 设计

### 目标传递

```cpp
std::atomic<double> seekTarget_{-1.0};

void seek(double seconds) {
    seekTarget_.store(seconds, std::memory_order_relaxed);
}
```

`run()` 在每次循环顶部调 `handleSeek()`：

```cpp
void handleSeek() {
    double target = seekTarget_.exchange(-1.0, std::memory_order_relaxed);
    if (target < 0.0) return;

    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);

    // 清空队列并释放残留包（必须 free，不能只 clear）
    AVPacket* p;
    while (videoQueue_->tryPop(p, 0)) av_packet_free(&p);
    while (audioQueue_->tryPop(p, 0)) av_packet_free(&p);
}
```

**多次快速 seek：** `exchange` 保证只取最新目标，中间值丢弃，行为正确。

**seek 延迟：** seek 请求在 `run()` 下次循环顶部才执行。若此时正阻塞在 `push`（队列满），延迟数十 ms，用户体感可接受。不做主动解锁，留 TODO 注释。

**seek 精度：** 使用 `AVSEEK_FLAG_BACKWARD`，定位到目标前最近关键帧（H.264 通常间隔 1–2 秒）。精确逐帧 seek 留到后续阶段。

### 解码器 flush（由 PlayerController 负责）

`PlayerController::seek()` 在调 `demux_.seek()` 的同时调 `videoDec_.flush()` 和 `audioDec_.flush()`，由解码线程在循环顶部执行 `avcodec_flush_buffers()`，消除 B 帧缓冲花屏。

---

## 内存管理

| 位置 | 职责 |
|------|------|
| `run()` 分配的 `pkt` | `av_packet_alloc` / `av_packet_free`，整个 run 期间复用 |
| clone 进队列的包 | `av_packet_clone` 分配，消费方（解码线程或 handleSeek）`av_packet_free` |
| seek 时队列残留 | `handleSeek()` 里 pop+free（不能用 `FrameQueue::clear()`，会泄漏） |
| stop 后队列残留 | `PlayerController::stopAllThreads()` 负责在线程退出后 pop+free |
| `fmtCtx_` | `DemuxThread::~DemuxThread()` 里 `avformat_close_input` |

---

## 测试策略

使用真实文件 `tests/data/sample.mp4`（2 秒 320×240 H.264+AAC，用 ffmpeg 生成）。

| 测试用例 | 验证内容 |
|----------|----------|
| `open_validFile_returnsTrue` | open 成功，`duration() > 0` |
| `open_invalidFile_returnsFalse` | 不存在的路径返回 false |
| `run_populatesQueues` | `start()` 后 300 ms，视频队列和音频队列均有包；结束后 pop+free 清理 |

Seek / stop 的集成行为在 Task 8（PlayerController）中覆盖，不在此测。

---

## 已知局限与后续

| 局限 | 说明 |
|------|------|
| seek 时 push 阻塞延迟 | 接受，网络流 / 精确 seek 阶段再优化 |
| `av_read_frame` 网络阻塞 | 本地文件无问题，网络流需加 `interrupt_callback` |
| seek 精度 | 关键帧级，精确 seek 留后续 |
