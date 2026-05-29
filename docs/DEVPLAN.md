# RambosPlayer 开发计划

**Goal:** 从零构建功能完整的多媒体播放器，逐步掌握多线程解码、音视频同步、硬件加速和实时流媒体。

**Architecture:** 每个阶段产出独立可运行的软件；后一阶段在前一阶段代码基础上叠加，不推倒重来。

**Tech Stack:** C++17, VS2017, Qt 5.14.2, FFmpeg 4.x (vcpkg `D:\vcpkg`), Qt Test

---

## 阶段总览

| # | 阶段 | Task 范围 | 交付物 | 详细计划 |
|---|------|-----------|--------|----------|
| 1 | 项目骨架 | Task 1 | 可编译的空 Qt 项目 + 测试框架 | 本文 Phase 1 |
| 2 | 基础组件 | Task 2–3 | `FrameQueue` + `AVSync` 单元测试全绿 | 本文 Phase 2 |
| 3 | 解复用 | Task 4 | `DemuxThread` 能把包推入两条队列 | 本文 Phase 3 |
| 4 | 解码层 | Task 5–6 | 视频 + 音频解码线程独立工作 | 本文 Phase 4 |
| 5 | 渲染与同步 | Task 7 | `VideoRenderer` 能同步显示视频帧 | 本文 Phase 5 |
| 6 | 完整播放器集成 | Task 8–10 | 进度条 / 音量 / 全屏，端对端播放 | 本文 Phase 6 |
| 7 | 硬件加速 | Task 11–13 | D3D11VA 解码，CPU 占用明显下降 | 本文 Phase 7 |
| 8 | 视频滤镜 | Task 14–16 | 实时亮度/对比度/水印预览 | 本文 Phase 8 |
| 9 | 屏幕录制/推流 | Task 17–20 | 能推流到本地 RTMP 服务 | 本文 Phase 9 |
| 10 | 视频剪辑器 | Task 21–24 | 时间线 UI，无损剪切导出 | 本文 Phase 10 |

> **完整 TDD 子计划（含逐步骤代码）：**
> `docs/superpowers/plans/2026-04-26-rambos-player-core.md`（覆盖 Phase 1–6）

---

## Phase 1 — 项目骨架（Task 1）

**目标：** 空 Qt 项目能编译运行，测试子项目框架就位。

- [x] 创建 `CMakeLists.txt`，配置 Qt5 模块（`Core Gui Widgets Multimedia`）、C++17、vcpkg toolchain 和 FFmpeg
- [x] 创建 `CMakePresets.json`，preset `default` 指定 VS2017 x64 生成器和 vcpkg toolchain
- [x] 创建 `src/main.cpp`，`QApplication` + 空 `QMainWindow` 显示窗口
- [x] 创建 `tests/CMakeLists.txt`，封装 `add_qt_test` helper，`enable_testing()` 就位
- [x] `cmake --preset default && cmake --build build --config Debug` → 0 error
- [x] `git commit -m "chore: 项目脚手架"`

**验收：** 运行程序出现空窗口；`ctest --test-dir build --config Debug` 能执行（无用例时直接通过）。

---

## Phase 2 — 基础组件（Task 2–3）

**目标：** `FrameQueue<T>` 和 `AVSync` 单元测试全绿。

### Task 2 — FrameQueue（线程安全模板队列）✅

- [x] 创建 `src/framequeue.h`，实现 `push` / `tryPop` / `clear` / `abort` / `reset` / `size`
- [x] 创建 `tests/tst_framequeue.cpp`，覆盖：单线程推取、超时返回 false、达到上限时阻塞、`abort` 解锁等待线程、`clear` 后 size 为 0
- [x] 运行 `TstFrameQueue.exe`，全部 PASS（7 passed, 0 failed）
- [x] `git commit -m "feat: FrameQueue 线程安全有界阻塞队列（Task 2）"`

### Task 3 — AVSync（音频时钟）✅

- [x] 创建 `src/avsync.h` / `src/avsync.cpp`，实现 `setAudioClock` / `audioClock` / `videoDelay`
  - `videoDelay(pts)`：视频领先 → 返回等待毫秒数；落后超过 400 ms → 返回 0（丢帧）
- [x] 创建 `tests/tst_avsync.cpp`，覆盖：默认时钟为负、set/get 精度、同步时延迟≈0、领先 200 ms 时延迟≈200、落后 500 ms 时延迟=0
- [x] 运行 `TstAVSync.exe`，全部 PASS（7 passed, 0 failed）
- [x] `git commit -m "feat: AVSync 音频时钟与视频延迟计算"`

**验收：** `tests.exe` 输出 0 failures。

---

## Phase 3 — 解复用线程（Task 4）

**目标：** `DemuxThread` 读取本地文件，按流索引分发包到视频/音频队列。

**准备：** 用以下命令生成 2 秒测试视频（执行一次）：
```bash
ffmpeg -f lavfi -i "testsrc=duration=2:size=320x240:rate=25" \
       -f lavfi -i "sine=frequency=440:duration=2" \
       -c:v libx264 -c:a aac -shortest tests/data/sample.mp4
```

- [x] 创建 `src/demuxthread.h` / `src/demuxthread.cpp`
  - `open(path, videoQueue, audioQueue)` → bool
  - `run()` 循环：`av_read_frame` → 按 stream_index 克隆包推入对应队列
  - `stop()` 设 abort 标志并 abort 两条队列
  - `seek(seconds)` 存原子量，`run()` 检测后调用 `av_seek_frame` 并清空队列
  - 暴露：`duration()`（微秒）、`videoStreamIdx()`、`audioStreamIdx()`、`formatContext()`
- [x] 创建 `tests/tst_demuxthread.cpp`，覆盖：open 有效文件返回 true、open 无效文件返回 false、运行 300 ms 后两条队列均有包
- [x] 运行测试，全部 PASS；清理测试中分配的 `AVPacket*`
- [x] `git commit -m "feat: DemuxThread 解复用线程"`

**验收：** 测试通过；Valgrind / ASAN 无包泄漏。

---

## Phase 4 — 解码层（Task 5–6）

**目标：** 视频解码线程产出 `AVFrame*` 到帧队列；音频解码线程输出 PCM 并维护音频时钟。

### Task 5 — VideoDecodeThread

- [x] 创建 `src/videodecodethread.h` / `src/videodecodethread.cpp`
  - `init(AVCodecParameters*)` 打开解码器
  - `run()` 循环：从输入包队列取包 → `avcodec_send_packet` / `avcodec_receive_frame` → clone 后推入帧队列
  - `flush()` 在 seek 后调用，清空解码器缓冲和输出队列
  - `stop()` abort 两侧队列
- [x] 编译通过（集成测试在 Phase 6 覆盖）
- [x] `git commit -m "feat: VideoDecodeThread 视频解码线程"`

### Task 6 — AudioDecodeThread

- [x] 创建 `src/audiodecodethread.h` / `src/audiodecodethread.cpp`
  - `init(AVCodecParameters*, AVRational timeBase, AVSync*)` 打开解码器 + 初始化 `SwrContext`（目标：S16 Stereo 44100）+ 创建 `QAudioOutput`
  - `run()` 循环：解码 → `swr_convert` → `QIODevice::write(PCM)` → 更新 `sync_->setAudioClock(pts)`
  - `setVolume(float)` 线程安全（原子量暂存，`run()` 循环中应用）
  - `flush()` 清空解码器和 swr 缓冲
- [x] 编译通过
- [x] `git commit -m "feat: AudioDecodeThread 音频解码与 QAudioOutput 推流"`

**验收：** 两个类编译无警告；接口签名与 Phase 5/6 使用一致。

---

## Phase 5 — 渲染与同步（Task 7）✅

**目标：** `VideoRenderer` 以音频时钟为基准定时渲染帧，视频不撕裂不跳帧。

- [x] 创建 `src/videorenderer.h` / `src/videorenderer.cpp`
  - 继承 `QWidget`，`setAttribute(Qt::WA_OpaquePaintEvent)`
  - `init(width, height, timeBase, AVSync*, FrameQueue<AVFrame*>*)` 初始化 `SwsContext`（YUV420P → RGB32）和 `QImage`
  - `QTimer`（1 ms）触发 `onTimer()`：从队列 `tryPop`（超时 0）→ 调用 `sync_->videoDelay(pts)` → 领先则 `push` 回队列等下次，`msleep` 延迟后 `sws_scale` 写入 `QImage` → `update()`
  - `paintEvent` 用 `QPainter` 保持宽高比居中绘制，背景黑色
- [x] 手动集成测试：用 DemuxThread + VideoDecodeThread + AudioDecodeThread + VideoRenderer 搭出最小 demo，打开 sample.mp4，确认视频/音频同步播放（Task 8 PlayerController 完成后执行）
- [x] `git commit -m "feat: VideoRenderer QPainter 帧渲染与音视频同步"`

**验收：** 视频流畅，音画同步误差 < 100 ms（耳听目测）。

---

## Phase 6 — 完整播放器集成（Task 8–10）✅

**目标：** 带进度条、音量滑块、全屏的完整播放器交付。

- [x] 创建 `src/playercontroller.h` / `src/playercontroller.cpp`
  - 持有并管理全部组件的生命周期：`DemuxThread`、`VideoDecodeThread`、`AudioDecodeThread`、三条队列、`AVSync`
  - 对外接口：`open(path)` / `play()` / `pause()` / `stop()` / `seek(seconds)` / `setVolume(float)`
  - 信号：`durationChanged(int64_t ms)` / `positionChanged(int64_t ms)`（100 ms `QTimer` 轮询音频时钟）/ `playbackFinished()`
- [x] 创建 `src/mainwindow.h` / `src/mainwindow.cpp` / `src/mainwindow.ui`
  - `VideoRenderer` 作为 `centralWidget`
  - 底部控制栏：`playPauseBtn`、`progressSlider`（range 0–1000）、`volumeSlider`（0–100）、`timeLabel`
  - 菜单：文件 → 打开（`QFileDialog`）
  - 双击 `VideoRenderer` 切换全屏
  - `onSeekSliderMoved` → `player_->seek(value/1000.0 * duration_ms/1000.0)`
- [x] 端对端测试清单（手动）：

  | 场景 | 预期 |
  |------|------|
  | 打开 1080p H.264 mp4 | 正常播放，音画同步 ✅ |
  | 拖进度条到中间 | seek 后继续播放 ✅ |
  | 音量设为 0 | 静音 ✅ |
  | 播放到末尾 | 按钮变 ▶ ✅ |
  | 双击全屏 → 双击退出 | 正常切换 ✅ |
  | 播放中关闭窗口 | 无崩溃 ✅ |

- [x] `git commit -m "feat: 完整播放器 UI 初级功能完成"`

**验收：** 全部端对端场景通过；单元测试 0 failures。

---

## Phase 7 — 硬件加速解码（D3D11VA）（Task 11–13）

**目标：** 播放 1080p 时 CPU 占用比软解低 30% 以上。

### Task 11 — HWAccel 封装

- [x] 创建 `src/hwaccel.h` / `src/hwaccel.cpp`，封装 `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_D3D11VA)`

### Task 12 — VideoDecodeThread 硬解集成

- [x] 修改 `VideoDecodeThread::init`，尝试绑定 `hw_device_ctx`；失败时静默回退软解
- [x] 修改 `VideoDecodeThread::run`，检测 `frame->format == AV_PIX_FMT_D3D11`，若是则先 `av_hwframe_transfer_data` 下载到 CPU frame，再推入队列

### Task 13 — 性能验收

- [x] 创建 `tests/tst_hwaccel.cpp`：open `sample.mp4`，解码 10 帧，验证 `frame->width > 0`（硬解/软解均接受）
- [x] 对比测试：用任务管理器分别记录软解和硬解播放 1080p 时的 CPU 占用，截图存 `docs/perf/hwaccel.png`
- [x] `git commit -m "feat: D3D11VA 硬件加速解码，软解自动回退"`

**验收：** 硬解路径下 CPU 降低 ≥ 30%；软解文件仍能正常播放。

---

## Phase 8 — 视频滤镜编辑器（Task 14–16）

**目标：** 实时预览亮度/对比度/裁剪/水印，可通过 UI 调节参数。

### Task 14 — FilterGraph 封装（TDD）✅

- [x] 创建 `src/filtergraph.h` / `src/filtergraph.cpp`
  - `init(width, height, pixFmt, AVRational timeBase, const QString& filterStr)` → `avfilter_graph_parse_ptr`
  - `process(AVFrame* in, AVFrame* out)` → `av_buffersrc_add_frame` / `av_buffersink_get_frame`
  - `rebuild(const QString& newFilterStr)` 重建滤镜图（用于实时调参）
- [x] 创建 `tests/tst_filtergraph.cpp`：passthrough、hflip、invalidFilter、rebuild 等 8 个测试全部通过

### Task 15 — VideoDecodeThread 滤镜集成 ✅

- [x] 在 `VideoDecodeThread::run` 中，帧推入队列前先过 `FilterGraph::process`（由 atomic `filterEnabled_` 控制是否启用）
- [x] 滤镜参数通过 atomic 变量 + `filterDirty_` 标志跨线程传递，解码线程自动 rebuild

### Task 16 — FilterPanel UI + 集成验证 ✅

- [x] 创建 `src/filterpanel.h` / `src/filterpanel.cpp` / `src/filterpanel.ui`（`QWidget`，挂在 `QDockWidget` 中）
  - 亮度滑块 → `hue=b=<v>`（范围 -1.0..1.0）
  - 对比度滑块 → `colorbalance=rm=<v>:gm=<v>:bm=<v>`（范围 -1.0..1.0）
  - 饱和度滑块 → `hue=s=<v>`（范围 0.0..3.0）
  - 水印路径输入 → `movie=<path>[wm];[in][wm]overlay=10:10`
  - 任意参数变化 → 设 dirty 标志，解码线程自动 rebuild
- [x] MainWindow 菜单栏"文件 → 滤镜面板"（checkable），控制右侧 Dock 显示/隐藏
- [x] 编译通过，端对端验证待手动测试

**验收：** 调节亮度/对比度滑块时视频预览实时变化；滤镜测试通过。

---

## Phase 9 — 推流播放内容（Task 17–20 重构）

**目标：** 将正在播放的视频（含滤镜效果）和音频推流到 RTMP 服务（在线平台 / 本地 SRS）
或通过 SRT 协议直连局域网设备，支持多路同时推流，不再依赖录屏。

> 详细实现计划：[docs/superpowers/plans/2026-05-17-phase9-restream.md](superpowers/plans/2026-05-17-phase9-restream.md)

```
VideoDecodeThread → videoFrameQueue  → VideoRenderer（不变）
                  ↘ restreamVideoQ  → VideoEncodeThread（H.264）─┐
                                                                   ├→ MuxThread[N] → RTMP / SRT / .flv
AudioDecodeThread → QAudioOutput（不变）                          │
                  ↘ restreamAudioQ → AudioEncodeThread（AAC）  ──┘
```

### Task 17 — 删除 CaptureThread / FrameQueue tryPush / 解码线程分叉

- [x] 删除 `src/capturethread.h` / `src/capturethread.cpp`
- [x] `framequeue.h` 新增 `tryPush()`（已存在，无需改动）
- [x] `VideoDecodeThread` 加 `setRestreamVideoQueue()`，run() 分叉 clone 帧
- [x] `AudioDecodeThread` 加 `setRestreamAudioQueue()`，swr_convert 前分叉 clone 帧

### Task 18 — VideoEncodeThread fan-out / AudioEncodeThread（新建）

- [x] `EncodeThread` 将单路输出改为 `vector` fan-out（clone 推多路）
- [x] 新建 `src/audioencodethread.h/.cpp`
  - `init(sampleRate, channels, bitrate)` 打开 AAC 编码器
  - 内部 `SwrContext`（任意格式 → fltp）+ `AVAudioFifo`（缓冲至 1024 samples）
  - fan-out：编码包 clone 推入所有输出队列

### Task 19 — MuxThread 重构（音视频双流 + SRT + PTS 归零）

- [x] 添加音频流（`AVStream`），`init()` 接收音视频参数及 extradata
- [x] 协议自动识别：`rtmp://` → FLV，`srt://` → MPEGTS，本地路径 → FLV
- [x] SRT listener 模式支持（`srt://:9000`）
- [x] PTS 归零：记录首帧 pts 作基准，后续包减去基准值
- [x] run() 轮询音视频两个队列，`av_interleaved_write_frame` 交错写

### Task 20 — StreamController 重构 + PlayerController 接口 + MainWindow UI

- [x] `StreamController` 删除 CaptureThread，管理多路 `MuxThread`（vector + 独立音视频队列 fan-out）
- [x] `PlayerController` 新增 `setRestreamVideoQueue()` / `setRestreamAudioQueue()` / 视频参数 getter
- [x] 新建 `src/streamconfigdialog.h/.cpp`：三个可勾选场景（直播平台/局域网/本地录制），`QSettings` 持久化
- [x] `MainWindow::onStreamStart()` 接入新接口；播放结束自动停止推流
- [x] 端对端验收：本地 FLV / RTMP（SRS）/ SRT 三种场景全部验证
  - RTMP 推本地 SRS：`ffplay -fflags nobuffer -flags low_delay rtmp://127.0.0.1/live/livestream` 正常播放，seek 后画面自动恢复无需手动操作 ✅
  - 本地 FLV 录制：ffprobe 验证 DTS 单调递增，seek 续接正确 ✅
  - 关窗 UAF 崩溃修复（#018）：推流中直接关闭窗口不再崩溃 ✅
- [x] `git commit -m "feat: 推流重构 — 播放内容推流，音视频双流，RTMP/SRT 多目标"`

**验收：** 推流内容与播放画面一致，有声音；平板 VLC 可通过 SRT 或 RTMP 拉流正常播放。
- [ ] `git commit -m "feat: 屏幕录制与 RTMP 推流（Phase 9）"`

### 环境依赖 — H.264 编码器

> **问题**（2026-05-15）：vcpkg FFmpeg 默认安装 (`ffmpeg[avcodec,avformat,...]`) **不含任何 H.264 编码器**，
> 导致 `EncodeThread::init` 三步回退全部落空，推流静默失败。
>
> **排查结果：**
> ```
> $ .\vcpkg list ffmpeg
> ffmpeg:x64-windows  8.1#2
> ffmpeg[avcodec]:x64-windows
> ffmpeg[avdevice]:x64-windows
> ffmpeg[avfilter]:x64-windows
> ffmpeg[avformat]:x64-windows
> ffmpeg[swresample]:x64-windows
> ffmpeg[swscale]:x64-windows
> # ← 缺少 ffmpeg[gpl,x264] 或 ffmpeg[openh264] 或 ffmpeg[nvcodec]
> ```
>
> **解决方案（三选一）：**
> ```powershell
> # 推荐：libx264 软编（需要 GPL）
> .\vcpkg install ffmpeg[gpl,x264] --recurse
>
> # 备选：Cisco openh264（BSD 许可，无需 GPL）
> .\vcpkg install ffmpeg[openh264] --recurse
>
> # 可选：NVIDIA GPU 硬编（需要 NVENC GPU + 驱动）
> .\vcpkg install ffmpeg[nvcodec] --recurse
> ```
>
> **代码层改进**（已实施）：
> - `encodethread.cpp`：每步尝试写 `qInfo`，最终失败时输出 vcpkg 安装建议到日志
> - `streamcontroller.cpp`：每个 init 阶段写日志 + 发 `errorOccurred` 信号
> - `mainwindow.cpp`：`errorOccurred` → `QMessageBox::warning` 弹窗，不再只闪状态栏
> - 默认输出路径改为 `QCoreApplication::applicationDirPath() + "/record.flv"`

---

## Phase 10 — 视频剪辑器（Task 21–24）

**目标：** 时间线 UI 可拖拽剪辑点，导出无损剪切片段。

### Task 21 — ThumbnailExtractor

- [x] 创建 `src/thumbnailextractor.h` / `src/thumbnailextractor.cpp`
  - `extract(path, count)` → `QList<QImage>`，解码 count 个均匀分布的关键帧

### Task 22 — Timeline QWidget

- [x] 创建 `src/timeline.h` / `src/timeline.cpp`（`QWidget` 子类）
  - 绘制时间轴刻度（`paintEvent`），自适应间距
  - 绘制视频缩略图轨道（从 `ThumbnailExtractor` 取 `QImage`）
  - 左右剪辑把手可拖拽（`mousePressEvent` / `mouseMoveEvent`），拖拽后发 `trimPointChanged(in, out)` 信号
  - 把手正上方显示 MM:SS / HH:MM:SS 时间标签

### Task 23 — ExportWorker

- [x] 创建 `src/exportworker.h` / `src/exportworker.cpp`
  - `-c copy` 无损剪切：av_seek_frame 到入口 → 等所有视频流关键帧就绪 → PTS 归零 → av_interleaved_write_frame → av_write_trailer
  - 音视频同步启动，关键帧不丢失

### Task 24 — 剪辑模式 MainWindow 集成 + 端对端验收

- [x] 在 `MainWindow` 加"剪辑模式 (Ctrl+T)"菜单 + "导出片段 (Ctrl+E)"，显示/隐藏 `Timeline` dock
- [x] 缩略图逐张送达，状态栏实时显示修剪区间
- [x] 手动验证：拖拽剪辑点，导出，播放正常，时长正确
- [x] `git commit -m "feat: 视频剪辑器时间线 UI 与无损剪切导出（Phase 10）"`

**验收：** 导出文件画面流畅无卡顿，时长与选区一致；程序无崩溃。

---

## Phase 11 — 低延迟推流（GPU 重编码 + MPEG-TS）

**目标：** 浏览器端到端延迟 ≤ 600ms，取代 HTTP-FLV 的 1-12s 抖动延迟。

> 详细方案设计：[docs/solutions/low-latency-streaming.md](solutions/low-latency-streaming.md)

### Phase A — EncodeThread 改造 ✅

- [x] `EncodeThread` 新增 `setGopSize(int)` setter，`init()` 使用可配置 GOP（默认 = fps）

### Phase B — StreamDecoder（推流专用解码）✅

- [x] 新建 `src/streamdecoder.h/.cpp`：视频/音频通用解码线程
  - 正确处理 nullptr sentinel（调用 `avcodec_flush_buffers` 而非发送 EOF）
  - 无 QAudioOutput / AVSync / FilterGraph，仅解码推帧

### Phase C — StreamPipeline（管线封装）✅

- [x] 新建 `src/streampipeline.h/.cpp`：管理 4 线程（视频解码+编码、音频解码+编码）+ 6 队列
  - 连接：`streamVideoInQ_` → StreamDecoder → `rawVideoQ_` → EncodeThread → `encodedVideoQ_`
  - 连接：`streamAudioInQ_` → StreamDecoder → `rawAudioQ_` → AudioEncodeThread → `encodedAudioQ_`
  - GOP = fps × gopSeconds（最小 1 帧）

### Phase D — MpegTsServer ✅

- [x] 新建 `src/mpegtsserver.h/.cpp`：HTTP MPEG-TS 流媒体服务器
  - mpegts muxer + 自定义 avio 广播
  - late-join：发送 `headerBytes_`（PAT/PMT）+ `segmentBytes_`（最近关键帧起）
  - 内嵌播放页：mpegts.js + `liveBufferLatencyChasing=true`
  - PAT/PMT 每 0.5s 重发，保证 segment buffer 自包含

### Phase E — 集成对接 ✅

- [x] `StreamDestination` 新增 `HttpMpegTs` 类型（含 fps/gopSeconds/bitrate 字段）
- [x] `StreamController` 创建 StreamPipeline + MpegTsServer，管理生命周期
- [x] `StreamConfigDialog` 新增"低延迟浏览器 (HTTP-MPEG-TS)"配置组（端口/GOP/码率）
- [x] `mainwindow::startStreaming()` 注册 StreamPipeline 输入队列到 DemuxThread

---

## Phase 12 — 剪辑器增强：三模式剪切 + 合并

**目标：** 新增浏览剪切（边播边标）、多段剪切（批量区间输入）两种交互模式，以及视频拼接/音频混音/音视频混流。

> 详细计划：[docs/superpowers/plans/2026-05-27-phase12-video-editor.md](superpowers/plans/2026-05-27-phase12-video-editor.md)

**新增组件：**
- `BrowseClipper` — 浏览剪切控制器 ✅
- `SegmentClipper` — 多段剪切控制器
- `MergeWorker` — 合并任务入口
- `ConcatDemuxer` / `ConcatFilter` — 视频拼接
- `AudioMixer` — 音频混音
- `SimpleMuxer` — 音视频混流
- `MergePanel` UI

### Task 1 — Timeline 底部导轨扩展 ✅

- [x] 底部导轨层绘制，区间色块 + 时间标签
- [x] 添加/清空/获取已标记区间列表
- [x] 底部导轨显示/隐藏控制
- [x] 浏览剪切首次空格时在底部导轨显示绿色竖线标记（待定入点），第二次空格确认后转为区间块
- [x] 自由剪辑把手显示/隐藏（隐藏时把手位置保留，切回时还原）
- [x] 按起止时间移除单个区间、合并重叠区间到已有区间、合并相邻/重叠区间
- [x] 添加区间时检测是否与已有区间重叠，重叠则拒绝并返回 false
- [x] 把手点击区域扩大至整条竖线，方便鼠标抓取
- [x] 设置总时长时过滤 0 值保护，防止把手叠在同一位置

### Task 2 — BrowseClipper 浏览剪切 ✅

- [x] 创建浏览剪切控制器类
- [x] 菜单"剪切 → 浏览剪切 (Ctrl+B)"启动，可勾选
- [x] 播放中按空格标记入点/出点，底部导轨实时显示
  - 首次空格 → 底部导轨绿色竖线标记入点
  - 二次空格 → 区间块显示，可继续标记下一段
- [x] 退出模式时弹出对话框列出所有已标记区间，可勾选保留或全部丢弃
- [x] 新增区间与已有区间重叠时弹出"合并到已有区间/丢弃此段/取消标记"三选项
- [x] 退出浏览剪切时只删除未勾选的区间，不清空重建，避免误伤其他模式保存的区间
- [x] 与自由剪辑互斥，切换到对方时询问是否保存当前入点/出点区间到底部导轨

### Timeline 信号与导出增强

- [x] 底部导轨区间数量变化时发射信号，供导出按钮状态使用
- [x] 添加/合并/删除/清空区间均发射该信号
- [x] 多段批量导出：有底部导轨区间时选择目录，每段导出为独立文件（文件名加序号）
- [x] 批量导出链式执行：每段完成后自动启动下一段
- [x] 单段导出保持原有的保存对话框
- [x] 每次导出完成后在同目录生成导出记录日志，记录源文件路径、每段区间、时长

### 模式切换管理

- [x] 菜单重组：文件 → 剪切子菜单 → {自由剪辑, 浏览剪切, 分隔线, 导出片段}
- [x] 两种模式互斥，不可同时激活
- [x] 切换模式时询问是否保存当前模式的区间数据
- [x] 把手随模式切换显隐（自由显示，浏览隐藏），把手位置保留不变
- [x] 切换中使用标志防止 dock 显隐信号循环触发状态回退
- [x] 先打开自由剪辑再打开文件时自动设置 Timeline 时长
- [x] 修复浏览模式时 dock 显示误触自由剪辑勾选状态
- [x] 导出按钮在自由剪辑拖把手时启用，浏览模式加区间时启用

### Task 3 — SegmentClipper 多段剪切 ✅
- [x] 输入面板 UI，逐行输入时间区间（QTextEdit + 验证列表）
- [x] 解析 MM:SS 和 HH:MM:SS 两种时间格式
- [x] 验证：不超过视频时长、起始≤结束、区间不重叠
- [x] 确定后填充底部导轨
- [x] 菜单入口 "多段剪切 (Ctrl+M)"，关于对话框同步更新

### Task 4 — 合并（Concat / Mix / Mux）
- [ ] ConcatDemuxer：同参数无损拼接
- [ ] AudioMixer：N 路音频混音
- [ ] ConcatFilter：异参数重编码拼接
- [ ] SimpleMuxer：音视频混流

### Task 5 — MergePanel UI + MainWindow 集成
- [ ] 文件拖入列表、模式自动判断、音量滑块
- [ ] 进度条、取消、日志

**验收：** 三种剪切模式均可正常标记区间并导出，模式切换互斥逻辑完整；各种合并场景输出正确。

---

## 当前进度

- [x] Phase 1 — 项目骨架
- [x] Phase 2 — 基础组件（FrameQueue，AVSync）
- [x] Phase 3 — 解复用线程（DemuxThread）
- [x] Phase 4 — 解码层
- [x] Phase 5 — 渲染与同步
- [x] Phase 6 — 完整播放器 UI
- [x] Phase 7 — 硬件加速
- [x] Phase 8 — 视频滤镜
- [x] Phase 9 — 屏幕录制/推流
- [x] Phase 10 — 视频剪辑器
- [x] Phase 11 — 低延迟推流（GPU 重编码 + MPEG-TS）
- [x] Phase 12 — 剪辑器增强：三模式剪切 + 合并（Task 1✅ Task 2✅ Task 3✅）
