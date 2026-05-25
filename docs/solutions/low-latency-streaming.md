# 低延迟推流方案：GPU 实时重编码 + mpegts.js 追帧

## 1. 问题分析

### 当前架构（-c copy 直通）

```
DemuxThread ─→ restreamVideoQueues_ ─→ HttpFlvServer (FLV mux, copy mode) ─→ flv.js
                  ↑ 原始 H.264 packet，GOP = 源文件设置
```

- GOP 完全取决于源视频文件编码参数（测试文件 GOP ≈ 10s）
- 浏览器 flv.js 必须从关键帧开始解码，平均等待 5s+
- 晚连优化（`codecConfigHeader_` + `currentGopBytes_`）解决了白屏，但延迟无法消除
- 即使 GOP=2s，稳态延迟仍在 1-2s 级别（flv.js 缓冲机制）

### 延迟来源分解

| 环节 | 当前延迟 | 原因 |
|------|---------|------|
| 关键帧等待 | 0-10s | 源文件 GOP=10s，必须等到下一个 IDR |
| 编码器缓冲 | 0 | copy 模式无编码缓冲 |
| 容器封装 | ~50ms | FLV tag 封装开销 |
| TCP 传输 | ~10ms | 局域网 |
| flv.js 缓冲 | 1-2s | 默认追帧不够激进 |
| **总计** | **1-12s** | 波动极大，体验差 |

---

## 2. 目标架构

### 核心思路

放弃 `-c copy`，在推流路径上插入 **解码→重编码** 管线，强制小 GOP + GPU 加速。

```
DemuxThread
  ├→ videoPacketQ → VideoDecodeThread → VideoRenderer           ← 本地播放，不变
  │
  ├→ streamVideoQ → StreamVideoDecoder → EncodeThread (h264_nvenc) ─┐
  │                   (AVFrame*)          (GOP=0.5s, zerolatency)   │
  │                                                                  │
  └→ streamAudioQ → StreamAudioDecoder → AudioEncodeThread (AAC) ──┤
                      (AVFrame*)          (AAC-LC)                   │
                                                                     ▼
                                            ┌──────────────────────────┐
                                            │ MpegTsServer (MPEG-TS)   │
                                            │ - avio callback 广播     │
                                            │ - late-join 发送 PAT/PMT │
                                            │   + 当前 segment         │
                                            └──────────┬───────────────┘
                                                       │ HTTP chunked
                                                       ▼
                                               mpegts.js (追帧模式)
```

### 延迟目标

| 环节 | 目标延迟 |
|------|---------|
| 关键帧等待 | ≤ 0.5s（GOP=0.5s） |
| GPU 编码 (NVENC) | 5-15ms/帧 |
| 容器封装 | ~10ms（TS packet 188B） |
| 网络传输 | ~10ms |
| mpegts.js 追帧缓冲 | ≤ 500ms |
| **总计** | **≤ 600ms** |

---

## 3. 组件设计

### 3.1 复用现有编码线程

`EncodeThread` 和 `AudioEncodeThread` 已完整实现，直接复用：

**EncodeThread** (`src/encodethread.h/.cpp`)
- 输入：`FrameQueue<AVFrame*>`（原始 YUV/BGR 帧）
- 输出：`FrameQueue<AVPacket*>`（H.264 编码包），支持 fan-out 多队列
- 编码器选择：h264_nvenc → libx264 → libopenh264 → 通用 H.264
- 现有配置：`gop_size=fps`, `max_b_frames=0`, `preset=p4`, `tune=ll`, `zerolatency=1`
- **改动**：`gop_size` 改为 `fps × gopSeconds`（可配置，默认 0.5s），暴露 `setGopSize(int)` 接口

**AudioEncodeThread** (`src/audioencodethread.h/.cpp`)
- 输入：`FrameQueue<AVFrame*>`（原始 PCM 帧）
- 输出：`FrameQueue<AVPacket*>`（AAC 编码包），支持 fan-out
- 编码器：AAC-LC，`AV_CODEC_FLAG_GLOBAL_HEADER`
- **无需改动**

### 3.2 新增：StreamPipeline（推流编解码管线）

封装完整推流管线，管理解码+编码线程生命周期。

**文件**：`src/streampipeline.h/.cpp`（新增）

```
class StreamPipeline : public QObject {
    // 创建并管理：
    //   VideoDecodeThread  streamVideoDec_;   // 推流专用视频解码
    //   AudioDecodeThread  streamAudioDec_;   // 推流专用音频解码
    //   EncodeThread       videoEnc_;         // H.264 编码
    //   AudioEncodeThread  audioEnc_;         // AAC 编码
    //
    // 队列连接：
    //   DemuxThread ─→ streamVideoQ ─→ streamVideoDec ─→ rawVideoQ ─→ videoEnc ─→ encodedVideoQ ─→ MpegTsServer
    //   DemuxThread ─→ streamAudioQ ─→ streamAudioDec ─→ rawAudioQ ─→ audioEnc ─→ encodedAudioQ ─→ MpegTsServer
    
    bool start(AVCodecParameters* vpar, AVCodecParameters* apar,
               AVRational vtb, AVRational atb,
               int targetFps, double gopSeconds);
    void stop();
    
    FrameQueue<AVPacket*>* encodedVideoQueue();
    FrameQueue<AVPacket*>* encodedAudioQueue();
};
```

**关键设计点**：
- 推流专用解码线程独立于播放解码线程，双方互不影响
- 队列容量：streamVideoQ cap=30, rawVideoQ cap=5, encodedVideoQ cap=15
- 为降低内存和延迟，rawVideoQ 容量设为 5（仅缓冲少量待编码帧）

### 3.3 新增/改造：MpegTsServer（替换 HttpFlvServer）

**方案**：在现有 `HttpFlvServer` 基础上改造，支持 MPEG-TS 输出。

**文件**：`src/mpegtsserver.h/.cpp`（新增，从 HttpFlvServer 分支）

**与 HttpFlvServer 的关键差异**：

| | HttpFlvServer | MpegTsServer |
|---|---|---|
| 容器格式 | FLV | MPEG-TS |
| 输入 | 原始 AVPacket（copy） | 编码后 AVPacket |
| muxer | `avformat_alloc_output_context2(flv)` | `avformat_alloc_output_context2(mpegts)` |
| late-join | codecConfigHeader + GOP | PAT/PMT + 当前 segment |
| 播放器 | flv.js | mpegts.js |
| 关键帧 | 不可控（源文件决定） | 每 0.5s 一个（编码器控制） |

**复用逻辑**：HTTP server、TCP socket 管理、`broadcastData()`、防火墙规则、`writeCallback` 回调机制 — 全部复用 HttpFlvServer 现有实现。

### 3.4 GOP 计算逻辑

```
GOP（帧数）= 帧率 × GOP 目标秒数

示例：
  30fps × 0.5s = 15 帧
  25fps × 0.5s = 12 帧（向下取整）
  60fps × 0.5s = 30 帧
```

**帧率获取**：用 ffprobe 读取源文件实际帧率，同时支持手动配置覆盖。

```cpp
// Streampipeline 初始化时
int gopSize = qMax(1, (int)(targetFps * gopSeconds_));
videoEnc_->setGopSize(gopSize);
```

**配置项**：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `gop_seconds` | 0.5 | GOP 目标时长（秒），越小延迟越低但码率越高 |
| `target_fps` | 自动检测 | 源文件帧率，可手动覆盖 |
| `encoder` | auto | `h264_nvenc` / `libx264` / auto（自动选择） |
| `bitrate` | 2M | 视频码率，CBR 模式 |
| `max_latency` | 1.5 | mpegts.js 追帧最大容忍延迟（秒） |

**配置存储**：复用 `QSettings("Rambos", "RambosPlayer")`，在 `StreamConfigDialog` 中暴露高级选项。

### 3.5 播放器端：flv.js → mpegts.js

**HTML 播放器**（`MpegTsServer::buildPlayerHtml()`）：

```javascript
mpegts.createPlayer({
  type: 'mpegts',
  url: '/stream.ts',
  isLive: true,
  enableStashBuffer: false,
  liveBufferLatencyChasing: true,
  liveBufferLatencyMaxLatency: 1.5,    // 最大容忍 1.5s
  liveBufferLatencyMinRemain: 0.3,     // 最少保留 0.3s 缓冲
  liveBufferLatencyChaseOffset: 0.1,   // 1.1x 倍速追赶
  autoCleanupSourceBuffer: true,
  autoCleanupMaxBackwardDuration: 2,
  autoCleanupMinBackwardDuration: 1
});
```

**mpegts.js 部署**：与 flv.js 同理，将 `mpegts.min.js` 放在 exe 同目录，`findMpegtsJs()` 查找并 serve。

---

## 4. 文件变更清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `src/streampipeline.h` | 推流编解码管线，管理解码+编码线程 |
| `src/streampipeline.cpp` | 同上实现 |
| `src/mpegtsserver.h` | MPEG-TS HTTP 广播服务器（类 HttpFlvServer） |
| `src/mpegtsserver.cpp` | 同上实现 |

### 修改文件

| 文件 | 改动 |
|------|------|
| `src/encodethread.h` | 新增 `setGopSize(int)`, `setBitrate(int)`, `reset()` |
| `src/encodethread.cpp` | `gop_size` 改为参数化；新增 `reset()` 重建编码器 |
| `src/streamcontroller.h` | 新增 `StreamPipeline` 成员；HttpFlv 目标改用 `MpegTsServer` |
| `src/streamcontroller.cpp` | 创建 `StreamPipeline` 替代直连 HttpFlvServer 队列 |
| `src/mainwindow.cpp` | `startStreaming()` 传递 fps/gopSeconds 配置；注册 encoded 队列到 DemuxThread |
| `src/streamconfigdialog.cpp` | 新增高级选项（GOP 秒数、编码器选择、码率） |
| `src/streamconfigdialog.h` | 同上 |
| `src/playercontroller.h` | 新增 `videoFps()` 获取源视频帧率接口 |
| `src/playercontroller.cpp` | 实现 `videoFps()` |
| `RambosPlayer.pro` | 添加新文件到 SOURCES/HEADERS |

### 不影响文件

- 本地播放管线完全不变
- `HttpFlvServer` 保留不动（向后兼容，后续可选删除）
- `LocalRecorder`、`MuxThread`、RTMP/SRT 推流保持不变
- `VideoRenderer`、`AVSync` 不变

---

## 5. 实施步骤

### Phase A: 编码器改造（小改）

1. `EncodeThread` 新增 `setGopSize(int)` / `setBitrate(int)` / `reset()`
2. 编译验证

### Phase B: StreamPipeline（中改）

1. 实现 `StreamPipeline` 类
2. 管理 4 个线程生命周期
3. 编译验证

### Phase C: MpegTsServer（中改）

1. 从 `HttpFlvServer` 复制分支
2. 改 FLV → MPEG-TS muxer
3. 改 late-join 逻辑（PAT/PMT + segment）
4. 改 player HTML（flv.js → mpegts.js）
5. 准备 `mpegts.min.js`
6. 编译验证

### Phase D: 管线对接（大改）

1. `StreamController` 接入 `StreamPipeline` + `MpegTsServer`
2. `MainWindow::startStreaming()` 传递编码参数
3. `StreamConfigDialog` 新增配置项
4. 端到端编译验证

### Phase E: 调优测试

1. 不同 GOP 设置下延迟对比
2. GPU vs CPU 编码性能对比
3. 多客户端并发测试
4. seek 后推流恢复验证

---

## 6. 验证方案

### 本地验证

```powershell
# 1. Debug 编译
cmake --build build --config Debug

# 2. 开 trace 日志
$env:QT_LOGGING_RULES="rambos.trace=true"

# 3. 启动，配置 HTTP-TS 推流，打开视频
.\build\Debug\RambosPlayer.exe

# 4. 本机浏览器 http://127.0.0.1:8080/player.html 验证
# 5. 检查日志：编码器选择、GOP 设置、关键帧间隔
```

### 延迟测量

1. 本地播放器画面 vs 浏览器画面同步拍照
2. 播放器显示 PTS 时间戳（已有 `timeLabel`）
3. 拍照对比时间差 = 端到端延迟

### 预期结果

- 启动后浏览器 1-2s 内出画面（首个 0.5s 关键帧）
- 稳态延迟 < 500ms
- GPU 编码 CPU 占用 < 5%

---

## 7. 风险与回退

| 风险 | 缓解 |
|------|------|
| NVENC 不可用 | 已有三级回退：NVENC → x264 → openh264 |
| 编码 CPU 占用过高 | ultrafast + zerolatency preset; 码率可控 |
| mpegts.js 兼容性 | flv.js 方案保留不动，配置开关切换 |
| 双解码内存翻倍 | rawVideoQ 容量仅 5，解码后立刻编码释放 |
| seek 后推流异常 | sentinel 机制已有，StreamPipeline 复用现有 PTS 续接逻辑 |
