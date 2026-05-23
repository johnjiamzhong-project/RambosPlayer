# 从零给 C++ 播放器加 RTMP 推流：四个坑和解法

## 背景

最近在用 C++17 + Qt + FFmpeg 从零写一个多媒体播放器（[RambosPlayer](https://github.com/rambos/RambosPlayer)），目标是覆盖 FFmpeg 全链路实战——解码、音视频同步、硬件加速、推流。本文记录实现 RTMP 推流功能时遇到的四个问题，以及每个问题的根因和修复方案。

推流管线的架构是 `-c copy` 直通模式：DemuxThread 读出的压缩包（H.264 + AAC）在推入播放队列的同时，`tryPush` 到推流队列，MuxThread 封装成 FLV 写出。不经过解码和重编码，播放和推流两条管线并行互不影响。

```
DemuxThread → videoPacketQueue → VideoDecodeThread → VideoRenderer（播放）
            → audioPacketQueue → AudioDecodeThread → QAudioOutput（播放）
            ↘ restreamVideoQ  → MuxThread → rtmp://...（推流）
            ↘ restreamAudioQ  → MuxThread
```

---

## 坑一：ffplay listen 模式反压卡顿

### 现象

最开始用 `ffplay -listen 1 rtmp://127.0.0.1:1935/live/test` 当本地接收端，推流几乎是静止的，帧率极低。

### 根因

`ffplay -listen` 不是真正的 RTMP 服务器，本质上是让 ffplay 监听 TCP 端口，推流端和 ffplay 直接 TCP 对接，中间没有任何缓冲层：

```
RambosPlayer → [直连 TCP] → ffplay
```

当 ffplay 处理一帧的时间超过推流帧间隔时，TCP 发送缓冲区满，`av_write_frame` 阻塞，整个 MuxThread 停转，进而 restream 队列满，DemuxThread 的 `tryPush` 开始丢包，推流卡死。

### 修复

换用 SRS（Simple Realtime Server）作为本地 RTMP 服务器。SRS 把推流端和播放端通过服务器缓冲完全解耦：

```
RambosPlayer →[RTMP推流]→ SRS [缓冲 + GOP cache] →[RTMP/HTTP-FLV拉流]→ ffplay
```

推流端只管往 SRS 写，写入速度不受任何下游消费者影响。

---

## 坑二：seek 之后录制文件花屏 10 秒

### 现象

推流到本地 FLV 文件，播放过程中拖动进度条 seek，录制文件的 seek 附近约 10 秒画面花屏。

### 根因一：GOP 重叠写入

H.264 中，P/B 帧依赖同 GOP 内的 I 帧才能解码。`av_seek_frame(AVSEEK_FLAG_BACKWARD)` 定位到目标时间点之前的最近关键帧（例如目标 15s，实际落点 12s）。

播放器需要从 12s 开始解码用于建立参考状态，但只显示 15s 之后的画面。而录制器挂载在 DemuxThread 层，无法区分"热身帧"和"真正要输出的帧"，12s–15s 的 GOP 重叠内容全部被写入了录制文件，接收端解码器遇到这段内容就会出错。

### 根因二：重编码 IDR 帧的 SPS/PPS 不兼容

早期方案试图"智能"处理：seek 后先跳过 GOP 重叠段，在 seek 目标帧处用 libx264 重新编码一个 IDR 帧写入，之后恢复 `-c copy`。

这个方案有一个不可调和的矛盾：

- IDR 帧由 libx264 编码 → SPS/PPS 来自 libx264
- 后续帧从源文件 copy → SPS/PPS 来自原始编码器

两段码流参数不兼容，解码器在 IDR 处用 libx264 参数重置状态，却收到原始编码器的 P 帧，解码失败，导致花屏。

### 修复：关键帧抑制期

放弃重编码，改为**等待源文件里真正的关键帧**：

1. seek 后进入抑制期，丢弃所有 PTS < targetSec 或非关键帧的包
2. 遇到第一个 `PTS >= targetSec && AV_PKT_FLAG_KEY` 的帧，退出抑制，从该帧起 `-c copy` 正常写入

由于 `av_seek_frame(BACKWARD)` 保证了关键帧之后的参考链完整，此帧写入后 H.264 码流全程自洽，不再需要任何重编码。

```cpp
// 视频抑制期逻辑（简化）
if (suppressVideoUntilSec_ >= 0) {
    double srcSec = pkt->pts * av_q2d(videoTimeBase_);
    bool isKey = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    if (srcSec < suppressVideoUntilSec_ || !isKey) {
        av_packet_free(&pkt); continue;
    }
    suppressVideoUntilSec_ = -1.0;
    suppressAudioUntilSec_ = srcSec; // 音频对齐到此关键帧
}
```

音频对齐：视频关键帧落点确定后，丢弃早于该 PTS 的音频包，保证 A/V 内容同步起点一致。

---

## 坑三：seek 之后推流时间轴断裂

### 现象

网络推流（RTMP）过程中 seek，ffplay 拉流端时间轴往回跳，画面出现卡顿或重置。

### 根因

MuxThread 收到 seek sentinel（`nullptr`）后，将 `videoPtsBase_` 重置为 `AV_NOPTS_VALUE`，下一个包的 PTS 成为新基准，输出从 0 重新计时。

对接收端而言，时间轴从 30s 突然跳回 0s，播放器触发重置。对 FLV 文件而言，`av_write_frame` 收到 DTS 倒退的帧会乱序或静默丢帧，文件损坏。

### 修复：segBase + accumPts 续接机制

sentinel 到来时不再归零，而是将上一段最后写出的 PTS 存为累积偏移：

```cpp
// sentinel 到来：保存上段末尾 PTS
videoAccumPts_ = videoLastOut_;
videoSegBase_  = AV_NOPTS_VALUE;

// 新包写入：从上段末尾续接
if (videoSegBase_ == AV_NOPTS_VALUE)
    videoSegBase_ = pkt->dts; // 首包 DTS 作段基准
pkt->pts = pkt->pts - videoSegBase_ + videoAccumPts_;
pkt->dts = pkt->dts - videoSegBase_ + videoAccumPts_;
```

对接收端完全透明，时间轴跨 seek 保持连续递增。

---

## 坑四：推流中关闭窗口 UAF 崩溃

### 现象

推流进行中直接关闭窗口，程序崩溃，异常码 `W32/0xC0000005`，崩溃地址 `0xFFFFFFFFFFFFFFFF`，定位在 `FrameQueue::abort()` 内的 `QMutexLocker lk(&mutex_)`。

### 根因

`~MainWindow()` 的析构顺序错误：

```cpp
// 原来的顺序（有问题）
MainWindow::~MainWindow() {
    streamCtrl_->stop();  // ← 销毁 videoMuxQueues_，FrameQueue 对象释放
    delete streamCtrl_;
    delete player_;       // ← DemuxThread 仍持有已释放队列的裸指针
}
```

`streamCtrl_->stop()` 内部 `clear()` 了 `unique_ptr` 容器，`FrameQueue` 对象被释放。随后 `delete player_` 触发 `DemuxThread::stop()` → `clearRestreamQueues()` → `q->abort()`，此时 `q` 是悬空指针，访问其内部 `mutex_` 触发 UAF。

### 修复

先让 DemuxThread 解除对队列的引用，再销毁队列：

```cpp
MainWindow::~MainWindow() {
    player_->clearRestreamPacketQueues(); // 先解除引用，abort 队列（对象仍存活）
    streamCtrl_->stop();                  // 再销毁队列
    delete streamCtrl_;
    delete player_;
}
```

---

## 测试方法

推流测试不需要真实直播账号，本地部署 SRS 即可。

**环境搭建**

下载 [SRS 5.0 Windows 版](https://github.com/ossrs/srs/releases/tag/v5.0-r0)，解压后双击 `srs-live.bat` 启动，默认监听 1935（RTMP）和 8080（HTTP）。

**推流与验证**

| 用途 | 地址 |
|------|------|
| 推流目标 | `rtmp://127.0.0.1/live/livestream` |
| 浏览器播放 | `http://127.0.0.1:8080/players/srs_player.html?schema=http` |
| ffplay 拉流 | `rtmp://127.0.0.1/live/livestream` |

推荐用 ffplay 验证 seek 行为，而非浏览器：

```bash
ffplay -fflags nobuffer -flags low_delay rtmp://127.0.0.1/live/livestream
```

浏览器的 HTTP-FLV 播放器（FLV.js）遇到时间戳抖动会暂停等待，seek 后需要手动点击 Play——这是客户端行为，不是推流问题。ffplay 对时间戳不连续的容忍度更高，seek 后画面自动恢复。

**DTS 单调性验证**

录制一段含 seek 操作的本地 FLV，用 ffprobe 检验 DTS 是否单调递增：

```powershell
$prev = -1.0
ffprobe -v quiet -show_entries packet=dts_time,flags -select_streams v:0 -of csv out.flv |
ForEach-Object {
    $cols = $_ -split ','; $dts = [double]$cols[2]
    if ($prev -ge 0 -and $dts -le $prev) { Write-Host "DTS 非单调: $prev -> $dts" }
    $prev = $dts
}
```

无输出说明 seek 续接逻辑正确。

---

## 总结

| 问题 | 根因 | 修复 |
|------|------|------|
| ffplay listen 卡顿 | 直连 TCP 无缓冲，下游反压上游 | 换 SRS，推拉解耦 |
| seek 后花屏 | 自插 IDR 帧 SPS/PPS 与源码流不兼容 | 等待源文件真正的关键帧，不重编码 |
| seek 后时间轴断裂 | sentinel 后 PTS 归零 | segBase + accumPts 跨段续接 |
| 关窗 UAF 崩溃 | 析构顺序错误，队列先于引用解除被销毁 | 先 `clearRestreamQueues`，再 `stop` |

最大的教训是第二个坑——"聪明"的重编码方案比"简单"的等待关键帧多了十倍复杂度，却引入了更严重的 SPS/PPS 不兼容问题。`-c copy` 的正确使用方式是相信源码流的完整性，而不是试图在中途插入外来编码器的产物。
