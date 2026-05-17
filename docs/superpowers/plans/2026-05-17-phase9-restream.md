# Phase 9 重构计划 — 推流内容来自播放器解码管线

**Goal:** 将推流源从录屏（CaptureThread）改为播放器解码帧，支持音视频双流，
协议支持 RTMP（在线平台 / 本地 SRS）和 SRT（局域网直连），支持多路同时推流。

**Architecture:**

```
VideoDecodeThread → videoFrameQueue  → VideoRenderer（不变）
                  ↘ restreamVideoQ  → VideoEncodeThread（H.264）─┐
                                       (fan-out: clone per mux)   ├→ MuxThread[0] → rtmp://...
AudioDecodeThread → QAudioOutput（不变）                          ├→ MuxThread[1] → srt://:9000
                  ↘ restreamAudioQ → AudioEncodeThread（AAC）  ──┘ → MuxThread[2] → record.flv
                                       (fan-out: clone per mux)
```

**Tech Stack:** C++17, Qt 5.14.2, FFmpeg 4.x (libavformat/avcodec/swresample/swscale)

---

## 文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/capturethread.h` | **删除** | 录屏方案废弃 |
| `src/capturethread.cpp` | **删除** | 录屏方案废弃 |
| `src/framequeue.h` | 修改 | 新增 `tryPush()`（非阻塞入队） |
| `src/videodecodethread.h/.cpp` | 修改 | 加 `setRestreamVideoQueue()` |
| `src/audiodecodethread.h/.cpp` | 修改 | 加 `setRestreamAudioQueue()` |
| `src/audioencodethread.h` | **新建** | AAC 编码线程声明 |
| `src/audioencodethread.cpp` | **新建** | SwrContext + AVAudioFifo + AAC 编码 |
| `src/encodethread.h/.cpp` | 修改 | 支持多路输出队列（fan-out） |
| `src/muxthread.h/.cpp` | 修改 | 音视频双流、SRT 支持、PTS 归零 |
| `src/streamcontroller.h/.cpp` | 修改 | 删 CaptureThread，管理多路 MuxThread |
| `src/mainwindow.h/.cpp` | 修改 | 多目标 UI，SRT 端口可配置 |
| `CMakeLists.txt` | 修改 | 移除 capturethread，加 audioencodethread |

---

## Task 1 — 删除 CaptureThread，更新构建

- [ ] 删除 `src/capturethread.h`、`src/capturethread.cpp`
- [ ] `CMakeLists.txt` 移除 `capturethread.h/.cpp`，添加 `audioencodethread.h/.cpp`
- [ ] 编译通过（此时 StreamController 里的引用会报错，Task 6 修复）

---

## Task 2 — FrameQueue 加 tryPush()

**File:** `src/framequeue.h`

在现有 `push()` 之后添加非阻塞入队方法。队列满或已 abort 时立即返回 false，
不阻塞调用者（解码线程）。推流慢时自动丢帧，保证播放器本身不受影响。

```cpp
// 非阻塞入队：队列满或已 abort 时立即返回 false
bool tryPush(T val) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (aborted_ || static_cast<int>(queue_.size()) >= capacity_)
        return false;
    queue_.push(val);
    cond_.notify_one();
    return true;
}
```

- [ ] 在 `framequeue.h` 中添加 `tryPush()`
- [ ] 编译通过

---

## Task 3 — VideoDecodeThread 分叉

**Files:** `src/videodecodethread.h/.cpp`

新增可选的转推队列指针。`run()` 每帧推入主队列后，若 `restreamVideoQueue_` 不为空，
`av_frame_clone` 一份用 `tryPush` 推入；`tryPush` 失败（队列满）时直接 `av_frame_free`，不阻塞。

**接口变更（h 文件）：**
```cpp
// 设置转推视频帧队列，nullptr 表示不转推
void setRestreamVideoQueue(FrameQueue<AVFrame*>* q);

private:
FrameQueue<AVFrame*>* restreamVideoQueue_ = nullptr; // 转推分叉队列
```

**run() 中在 push 到主队列之后添加：**
```cpp
if (restreamVideoQueue_) {
    AVFrame* copy = av_frame_clone(frame);
    if (!restreamVideoQueue_->tryPush(copy))
        av_frame_free(&copy);  // 队列满，丢帧
}
```

注意：硬解路径已在 `av_hwframe_transfer_data` 后下载到 CPU 帧再 push，
restreamVideoQueue_ 拿到的帧一定是 CPU 帧，无需额外处理。

- [ ] 修改 `videodecodethread.h` 添加接口声明和成员变量
- [ ] 修改 `videodecodethread.cpp` 实现 setter 和 run() 分叉逻辑
- [ ] 编译通过

---

## Task 4 — AudioDecodeThread 分叉

**Files:** `src/audiodecodethread.h/.cpp`

在 `swr_convert` 之前 clone 原始解码帧推入转推队列。本地播放路径不变。
音频帧格式（fltp/s16p 等）由源文件决定，AudioEncodeThread 内部自行转换。

**接口变更（h 文件）：**
```cpp
// 设置转推音频帧队列，nullptr 表示不转推
void setRestreamAudioQueue(FrameQueue<AVFrame*>* q);

private:
FrameQueue<AVFrame*>* restreamAudioQueue_ = nullptr; // 转推分叉队列
```

**run() 中 swr_convert 之前添加：**
```cpp
if (restreamAudioQueue_) {
    AVFrame* copy = av_frame_clone(decodedFrame);
    if (!restreamAudioQueue_->tryPush(copy))
        av_frame_free(&copy);
}
```

- [ ] 修改 `audiodecodethread.h` 添加接口声明和成员变量
- [ ] 修改 `audiodecodethread.cpp` 实现 setter 和 run() 分叉逻辑
- [ ] 编译通过

---

## Task 5 — AudioEncodeThread（新建）

**Files:** `src/audioencodethread.h`、`src/audioencodethread.cpp`

AAC 编码线程。内部维护 SwrContext（任意格式 → fltp 44100 stereo）和
AVAudioFifo（缓冲采样，每次取 1024 samples 喂编码器）。
支持多路输出队列（fan-out），每个编码包 `av_packet_clone` 后分别推入。

**接口：**
```cpp
class AudioEncodeThread : public QThread {
    Q_OBJECT
public:
    ~AudioEncodeThread() override;

    bool init(int sampleRate, int channels, int bitrate = 128000);
    void stop();
    void addOutputQueue(FrameQueue<AVPacket*>* q); // fan-out：添加输出目标
    void clearOutputQueues();
    void setInputQueue(FrameQueue<AVFrame*>* q);
    AVCodecContext* codecContext() const { return codecCtx_; }

protected:
    void run() override;

private:
    // 核心流程：inputQueue → swr → fifo → 编码 → fan-out 到 outputQueues_
    AVCodecContext*              codecCtx_     = nullptr;
    SwrContext*                  swrCtx_       = nullptr;
    AVAudioFifo*                 fifo_         = nullptr;
    AVFrame*                     swrFrame_     = nullptr; // swr 输出帧（复用）
    FrameQueue<AVFrame*>*        inputQueue_   = nullptr;
    std::vector<FrameQueue<AVPacket*>*> outputQueues_;
    std::atomic<bool>            abort_{false};
    int64_t                      nextPts_      = 0;       // 编码帧计数，用于 pts 生成
};
```

**init() 关键步骤：**
1. `avcodec_find_encoder(AV_CODEC_ID_AAC)` — FFmpeg 内置 AAC，vcpkg 默认包含，不需要额外安装
2. 设置 `sample_fmt = AV_SAMPLE_FMT_FLTP`，`sample_rate`，`channel_layout`
3. 初始化 `SwrContext`：输入格式在第一帧到来时动态确定（lazy init）
4. `av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, channels, 1)`

**run() 关键步骤：**
```
取帧 → swr_convert 到 fltp → av_audio_fifo_write
while fifo 中 samples >= codecCtx_->frame_size(1024):
    av_audio_fifo_read(1024 samples) → 编码 → fan-out clone 到各 outputQueues_
```

**PTS 处理：** 用 `nextPts_` 计数，每帧 pts = nextPts_，nextPts_ += frame_size。
时间基为 `{1, sample_rate}`，MuxThread 写包前 rescale 到 stream->time_base。

- [ ] 创建 `src/audioencodethread.h`，写好类声明和成员注释
- [ ] 创建 `src/audioencodethread.cpp`，实现 init / run / stop
- [ ] 编译通过

---

## Task 6 — EncodeThread（VideoEncodeThread）支持 fan-out

**Files:** `src/encodethread.h/.cpp`

将单路 `setOutputQueue()` 改为多路 fan-out，每个编码包 clone 后推入所有输出队列。

**接口变更：**
```cpp
void addOutputQueue(FrameQueue<AVPacket*>* q);   // 替换 setOutputQueue
void clearOutputQueues();

private:
std::vector<FrameQueue<AVPacket*>*> outputQueues_; // 替换 outputQueue_
```

**run() 中推包改为：**
```cpp
for (auto* q : outputQueues_) {
    AVPacket* copy = av_packet_clone(pkt);
    q->push(copy);
}
av_packet_unref(pkt);
```

- [ ] 修改 `encodethread.h` 替换 outputQueue_ 为 vector
- [ ] 修改 `encodethread.cpp` 适配 fan-out 推包
- [ ] 编译通过

---

## Task 7 — MuxThread 重构（音视频双流 + SRT + PTS 归零）

**Files:** `src/muxthread.h/.cpp`

MuxThread 从单视频流升级为音视频双流，同时支持 SRT 协议和 PTS 归零。

**接口变更：**
```cpp
bool init(const QString& url, int width, int height, AVRational videoTimeBase,
          int sampleRate, int channels,
          const uint8_t* videoExtradata = nullptr, int videExtradataSize = 0,
          const uint8_t* audioExtradata = nullptr, int audioExtradataSize = 0);

void setVideoInputQueue(FrameQueue<AVPacket*>* q);
void setAudioInputQueue(FrameQueue<AVPacket*>* q);

private:
AVStream*               aStream_       = nullptr; // 音频流
FrameQueue<AVPacket*>*  audioQueue_    = nullptr;
int64_t                 videoPtsBase_  = AV_NOPTS_VALUE; // PTS 归零基准
int64_t                 audioPtsBase_  = AV_NOPTS_VALUE;
```

**协议识别（init 内）：**
```cpp
// SRT：url 前缀 srt://，格式用 mpegts 或 flv 均可，这里用 mpegts
bool isSrt  = url.startsWith("srt://");
bool isRtmp = url.startsWith("rtmp://");
const char* fmt = (isSrt && !isRtmp) ? "mpegts" : "flv";
avformat_alloc_output_context2(&fmtCtx_, nullptr, fmt, nullptr);
```

**SRT listener 模式：** url 格式为 `srt://:9000`，FFmpeg 通过 libsrt 自动处理 listen/accept，
`avio_open` 时会阻塞直到有客户端连接。

> **依赖说明：** SRT 需要 FFmpeg 编译时含 libsrt。若当前 vcpkg 包不支持，
> 运行 `.\vcpkg install ffmpeg[srt]` 后重新 cmake。
> 若 libsrt 不可用，SRT 选项在 UI 上仍显示，但 `avio_open` 会报错并通过 `errorOccurred` 信号通知 UI。

**PTS 归零（run() 写包前）：**
```cpp
// 记录第一帧 pts 作为基准，后续每包减去基准值
if (videoPtsBase_ == AV_NOPTS_VALUE) videoPtsBase_ = pkt->pts;
pkt->pts -= videoPtsBase_;
pkt->dts -= videoPtsBase_;
```

**run() 双流交错：** 用 `tryPop` 轮询两个队列，`av_interleaved_write_frame` 自动处理交错。

- [ ] 修改 `muxthread.h` 添加音频流相关成员和接口
- [ ] 修改 `muxthread.cpp` 实现双流 init、SRT 识别、PTS 归零、双队列轮询
- [ ] 编译通过

---

## Task 8 — StreamController 重构

**Files:** `src/streamcontroller.h/.cpp`

删除 CaptureThread，改为接收来自 PlayerController 的解码帧队列。
支持多路 MuxThread（vector），动态添加推流目标。

**接口变更：**
```cpp
struct StreamDestination {
    enum Type { Rtmp, Srt, LocalFile };
    Type    type;
    QString url;  // RTMP: 完整 URL；SRT: "srt://:port"；LocalFile: 文件路径
};

bool start(const QList<StreamDestination>& destinations,
           int width, int height, int fps,
           int sampleRate, int channels,
           int videoBitrate = 2000000, int audioBitrate = 128000);
void stop();

FrameQueue<AVFrame*>* videoSourceQueue() { return &restreamVideoQ_; }
FrameQueue<AVFrame*>* audioSourceQueue() { return &restreamAudioQ_; }

private:
FrameQueue<AVFrame*>   restreamVideoQ_{30};
FrameQueue<AVFrame*>   restreamAudioQ_{60};
VideoEncodeThread      videoEncode_;
AudioEncodeThread      audioEncode_;
std::vector<std::unique_ptr<MuxThread>> muxThreads_;
```

**start() 流程：**
1. `videoEncode_.init(width, height, fps, videoBitrate)`
2. `audioEncode_.init(sampleRate, channels, audioBitrate)`
3. 遍历 destinations，每个创建一个 MuxThread，init 后加入 vector
4. 把每个 MuxThread 的视频/音频输入队列注册到 videoEncode_/audioEncode_ 的 fan-out
5. 逆序启动：先启动所有 MuxThread，再启动 audioEncode_，最后启动 videoEncode_

**stop() 流程：**
1. 通知 PlayerController 断开分叉（由 MainWindow 协调）
2. videoEncode_.stop() + wait
3. audioEncode_.stop() + wait
4. 所有 MuxThread stop() + wait
5. 清空队列，reset

- [ ] 修改 `streamcontroller.h`，删除 CaptureThread 相关，添加新接口
- [ ] 修改 `streamcontroller.cpp` 实现新的 start/stop
- [ ] 编译通过

---

## Task 9 — PlayerController 暴露分叉接口

**Files:** `src/playercontroller.h/.cpp`

**新增接口：**
```cpp
void setRestreamVideoQueue(FrameQueue<AVFrame*>* q); // 转推给 VideoDecodeThread
void setRestreamAudioQueue(FrameQueue<AVFrame*>* q); // 转推给 AudioDecodeThread
int  videoWidth()   const;
int  videoHeight()  const;
int  videoFps()     const;  // 返回整数帧率，足够用于编码器 init
int  audioSampleRate() const;
int  audioChannels()   const;
```

内部透传给 `VideoDecodeThread` 和 `AudioDecodeThread`。
stop() 时自动将两个队列设为 nullptr。

- [ ] 修改 `playercontroller.h` 添加接口声明
- [ ] 修改 `playercontroller.cpp` 实现，透传给对应 decode thread
- [ ] 编译通过

---

## Task 10 — MainWindow UI 重构

**Files:** `src/mainwindow.h/.cpp`（以及 `.ui` 若需要新控件）

**推流配置对话框：**

用 `QDialog` 替换原来的两次 `QInputDialog`。对话框包含三个可勾选的目标场景，
用户可**单选或多选**，每个场景勾选后显示对应的配置项。至少选一项才能点"开始推流"。

```
┌──────────────────────────────────────────┐
│  推流配置                                  │
├──────────────────────────────────────────┤
│ ☑ 直播平台 (RTMP)                         │
│   URL: [rtmp://a.rtmp.twitch.tv/live/key]│
│                                          │
│ ☑ 局域网设备 (SRT)                        │
│   端口: [9000]  （平板用 VLC 拉流）        │
│                                          │
│ ☐ 本地录制                                │
│   路径: [C:/record.flv]  [浏览...]        │
├──────────────────────────────────────────┤
│  [开始推流]                    [取消]      │
└──────────────────────────────────────────┘
```

**实现要点：**
- 三个 `QGroupBox`（带 `setCheckable(true)`），勾选时内部控件可用，取消勾选时置灰
- 直播平台：`QLineEdit` 输入 RTMP URL
- 局域网设备：`QSpinBox` 输入端口（默认 9000，范围 1024–65535），URL 自动拼为 `srt://:port`
- 本地录制：`QLineEdit` + `QPushButton`（浏览），`QFileDialog::getSaveFileName` 选 `.flv` 路径
- "开始推流"按钮：至少一项勾选才可点击（`QDialogButtonBox` 的 OK 按钮动态 `setEnabled`）
- 上次的配置通过 `QSettings` 记忆，下次打开对话框自动填入

**StreamDestination 结构调整：**
```cpp
struct StreamDestination {
    enum Type { Rtmp, Srt, LocalFile };
    Type    type;
    QString url;     // RTMP: 完整 URL；SRT: srt://:port；LocalFile: 文件路径
};
```

**onStreamStart() 流程：**
```cpp
// 启动
if (!player_->isPlaying()) { warn("请先播放视频"); return; }
// 弹配置对话框，用户勾选并填写参数
// 收集勾选的 StreamDestination 列表（1~3 项）
// 从 player_ 读视频参数
player_->setRestreamVideoQueue(streamCtrl_->videoSourceQueue());
player_->setRestreamAudioQueue(streamCtrl_->audioSourceQueue());
streamCtrl_->start(destinations, w, h, fps, sr, ch);
ui->actionStream->setText("停止推流");

// 停止
player_->setRestreamVideoQueue(nullptr);
player_->setRestreamAudioQueue(nullptr);
streamCtrl_->stop();
ui->actionStream->setText("推流(&S)...");
```

- [ ] 实现推流配置 `QDialog`（三个可勾选 `QGroupBox`，至少一项才能确认）
- [ ] `QSettings` 记忆上次配置
- [ ] 修改 `onStreamStart()` 连接新的 StreamController 接口
- [ ] 编译通过，端对端手动验证（单选 / 多选 各跑一遍）

---

## 端对端验收清单

| 场景 | 预期 |
|------|------|
| 未播放视频时点推流 | 弹提示，不启动 |
| 推到本地 .flv 文件 | 文件有音视频，A/V 同步 |
| 推到本地 SRS（RTMP） | 平板 VLC 拉流正常，有声音 |
| SRT 局域网直连平板 | 平板 VLC 连接后正常播放，有声音 |
| 同时推两路目标 | 两个目标均正常接收 |
| 推流中 seek | 推流有短暂花帧后恢复，播放器不卡 |
| 推流中关闭播放器 | 无崩溃，MuxThread 正常写 trailer |
| 推流中视频播放结束 | 推流自动停止 |
