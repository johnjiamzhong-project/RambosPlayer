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
- [ ] `git commit -m "feat: AVSync 音频时钟与视频延迟计算"`

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
- [ ] `git commit -m "feat: VideoDecodeThread 视频解码线程"`

### Task 6 — AudioDecodeThread

- [x] 创建 `src/audiodecodethread.h` / `src/audiodecodethread.cpp`
  - `init(AVCodecParameters*, AVRational timeBase, AVSync*)` 打开解码器 + 初始化 `SwrContext`（目标：S16 Stereo 44100）+ 创建 `QAudioOutput`
  - `run()` 循环：解码 → `swr_convert` → `QIODevice::write(PCM)` → 更新 `sync_->setAudioClock(pts)`
  - `setVolume(float)` 线程安全（原子量暂存，`run()` 循环中应用）
  - `flush()` 清空解码器和 swr 缓冲
- [x] 编译通过
- [ ] `git commit -m "feat: AudioDecodeThread 音频解码与 QAudioOutput 推流"`

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
- [ ] `git commit -m "feat: VideoRenderer QPainter 帧渲染与音视频同步"`

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

- [ ] `git commit -m "feat: 完整播放器 UI 初级功能完成"`

**验收：** 全部端对端场景通过；单元测试 0 failures。

---

## Phase 7 — 硬件加速解码（D3D11VA）（Task 11–13）

**目标：** 播放 1080p 时 CPU 占用比软解低 30% 以上。

### Task 11 — HWAccel 封装

- [ ] 创建 `src/hwaccel.h` / `src/hwaccel.cpp`，封装 `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_D3D11VA)`

### Task 12 — VideoDecodeThread 硬解集成

- [ ] 修改 `VideoDecodeThread::init`，尝试绑定 `hw_device_ctx`；失败时静默回退软解
- [ ] 修改 `VideoDecodeThread::run`，检测 `frame->format == AV_PIX_FMT_D3D11`，若是则先 `av_hwframe_transfer_data` 下载到 CPU frame，再推入队列

### Task 13 — 性能验收

- [ ] 创建 `tests/tst_hwaccel.cpp`：open `sample.mp4`，解码 10 帧，验证 `frame->width > 0`（硬解/软解均接受）
- [ ] 对比测试：用任务管理器分别记录软解和硬解播放 1080p 时的 CPU 占用，截图存 `docs/perf/hwaccel.png`
- [ ] `git commit -m "feat: D3D11VA 硬件加速解码，软解自动回退"`

**验收：** 硬解路径下 CPU 降低 ≥ 30%；软解文件仍能正常播放。

---

## Phase 8 — 视频滤镜编辑器（Task 14–16）

**目标：** 实时预览亮度/对比度/裁剪/水印，可通过 UI 调节参数。

### Task 14 — FilterGraph 封装（TDD）

- [ ] 创建 `src/filtergraph.h` / `src/filtergraph.cpp`
  - `init(width, height, pixFmt, AVRational timeBase, const QString& filterStr)` → `avfilter_graph_parse_ptr`
  - `process(AVFrame* in)` → `av_buffersrc_add_frame` / `av_buffersink_get_frame` → 返回 `AVFrame*`
  - `rebuild(const QString& newFilterStr)` 重建滤镜图（用于实时调参）
- [ ] 创建 `tests/tst_filtergraph.cpp`：init 一个 `eq=brightness=0.1` 图，process 一帧，验证返回帧不为 null 且尺寸不变

### Task 15 — VideoDecodeThread 滤镜集成

- [ ] 在 `VideoDecodeThread::run` 中，帧推入队列前先过 `FilterGraph::process`（可选，由 `PlayerController` 控制是否启用）

### Task 16 — FilterPanel UI + 集成验证

- [ ] 创建 `src/filterpanel.h` / `src/filterpanel.cpp`（`QDockWidget`）
  - 亮度滑块 → 生成 `eq=brightness=<v>` 滤镜字符串
  - 对比度滑块 → `eq=contrast=<v>`
  - 水印路径输入 → `movie=<path>[wm];[in][wm]overlay=10:10`
  - 任意参数变化 → 调用 `filterGraph_->rebuild(str)`
- [ ] 手动验证：调节亮度/对比度滑块时视频预览实时变化
- [ ] `git commit -m "feat: libavfilter 滤镜图 + 实时调参 UI"`

**验收：** 调节亮度/对比度滑块时视频预览实时变化；滤镜测试通过。

---

## Phase 9 — 屏幕录制 / 推流（Task 17–20）

**目标：** 能采集桌面或摄像头，编码后推流到本地 RTMP 服务。

**准备：** 本地启动一个 RTMP 服务（如 nginx-rtmp）监听 `rtmp://127.0.0.1:1935/live/test`。

### Task 17 — CaptureThread

- [ ] 创建 `src/capturethread.h` / `src/capturethread.cpp`
  - `init(source)` 其中 `source` 为 `"desktop"`（gdigrab）或设备名（dshow）
  - `run()` 循环：`av_read_frame` from input → 推入 `FrameQueue<AVPacket*>`

### Task 18 — EncodeThread

- [ ] 创建 `src/encodethread.h` / `src/encodethread.cpp`
  - `init(codecName, width, height, fps, bitrate)` 打开编码器（优先 `h264_nvenc`，失败回退 `libx264`）
  - `run()` 从帧队列取 raw frame → `avcodec_send_frame` / `avcodec_receive_packet` → 推入输出包队列

### Task 19 — MuxThread

- [ ] 创建 `src/muxthread.h` / `src/muxthread.cpp`
  - `init(outputUrl)` 打开 `flv` 输出格式，`avio_open` 连接 RTMP URL
  - `run()` 从编码包队列取包 → `av_interleaved_write_frame`

### Task 20 — StreamController + 推流 UI + 端对端验收

- [ ] 创建 `src/streamcontroller.h` / `src/streamcontroller.cpp` 串联三者
- [ ] 在 `MainWindow` 菜单栏加"推流"菜单，弹出 URL 输入对话框，启动/停止推流
- [ ] 手动验证：启动推流后，用 `ffplay rtmp://127.0.0.1:1935/live/test` 能看到桌面画面，延迟 < 3 秒
- [ ] `git commit -m "feat: 屏幕录制与 RTMP 推流"`

**验收：** ffplay 能接收并播放推流画面，延迟 < 3 秒。

---

## Phase 10 — 视频剪辑器（Task 21–24）

**目标：** 时间线 UI 可拖拽剪辑点，导出无损剪切片段。

### Task 21 — ThumbnailExtractor

- [ ] 创建 `src/thumbnailextractor.h` / `src/thumbnailextractor.cpp`
  - `extract(path, count)` → `QList<QImage>`，解码 count 个均匀分布的关键帧

### Task 22 — Timeline QWidget

- [ ] 创建 `src/timeline.h` / `src/timeline.cpp`（`QWidget` 子类）
  - 绘制时间轴刻度（`paintEvent`）
  - 绘制视频缩略图轨道（从 `ThumbnailExtractor` 取 `QImage`）
  - 左右剪辑把手可拖拽（`mousePressEvent` / `mouseMoveEvent`），拖拽后发 `trimPointChanged(in, out)` 信号

### Task 23 — ExportWorker

- [ ] 创建 `src/exportworker.h` / `src/exportworker.cpp`
  - `run(inputPath, outputPath, inPts, outPts)`：`av_seek_frame` 到 inPts，循环 `av_read_frame` 直到 outPts，`-c copy` 写出

### Task 24 — 剪辑模式 MainWindow 集成 + 端对端验收

- [ ] 在 `MainWindow` 加"剪辑模式"切换按钮，显示/隐藏 `Timeline` dock
- [ ] 手动验证：拖拽剪辑点，点击"导出"，用 ffprobe 确认输出文件时长与选区一致，误差 < 1 GOP
- [ ] `git commit -m "feat: 视频剪辑器时间线 UI 与无损剪切导出"`

**验收：** 导出文件时长误差 < 1 个 GOP（通常 < 2 秒）；程序无崩溃。

---

## 当前进度

- [x] Phase 1 — 项目骨架
- [x] Phase 2 — 基础组件（FrameQueue ✅，AVSync ✅）
- [x] Phase 3 — 解复用线程（DemuxThread ✅）
- [x] Phase 4 — 解码层
- [x] Phase 5 — 渲染与同步 ✅
- [x] Phase 6 — 完整播放器 UI ✅
- [ ] Phase 7 — 硬件加速
- [ ] Phase 8 — 视频滤镜
- [ ] Phase 9 — 屏幕录制/推流
- [ ] Phase 10 — 视频剪辑器
