# Bug 修复记录

> 记录开发计划之外发现并修复的问题。每次修复后按模板追加一条记录。

---

## #001 — .ui 文件 contentsMargins 格式不兼容 Qt 5.14.2

- **日期**：2026-05-09
- **现象**：编译报错 `uic: Error in line 15, column 17 : Unexpected element left`，cmake_autogen 步骤失败（MSB3073）
- **根因**：`mainwindow.ui` 中 `contentsMargins` 使用了 `<rect><left>...</left>...</rect>` 格式，Qt 5.14.2 的 `uic` 不认识该写法（`<rect>` 只接受 `<x>/<y>/<width>/<height>`）
- **修复**：改为独立属性 `leftMargin`/`topMargin`/`rightMargin`/`bottomMargin`
- **涉及文件**：`src/mainwindow.ui`

---

## #002 — 三线程 abort_ 不重置导致播放器无法工作

- **日期**：2026-05-09
- **现象**：打开文件后播放无反应，DemuxThread / VideoDecodeThread / AudioDecodeThread 的 `run()` 立刻退出
- **根因**：`PlayerController::open()` 先调 `stop()`，将三个线程的 `abort_` 置为 `true`；但各自的 `open()` / `init()` 不重置 `abort_`，导致后续 `play()` 启动线程后 `run()` 循环条件立刻不满足
- **修复**：在 `DemuxThread::open()`、`VideoDecodeThread::init()`、`AudioDecodeThread::init()` 开头添加 `abort_.store(false)`
- **涉及文件**：`src/demuxthread.cpp`、`src/videodecodethread.cpp`、`src/audiodecodethread.cpp`

---

## #003 — 未打开文件时点击播放导致空指针崩溃

- **日期**：2026-05-09
- **现象**：未打开文件直接点击播放按钮，触发 `av_read_frame(nullptr)` → `W32/0xC0000005` 访问违规
- **根因**：`PlayerController::play()` 无前置检查，`DemuxThread::run()` 无 `fmtCtx_` 空指针保护；初始状态 `abort_` 为 `false`、`fmtCtx_` 为 `nullptr`，线程启动后直接进入循环调用 `av_read_frame`
- **修复**：`PlayerController::play()` 添加 `!demux_.formatContext()` 前置检查；`DemuxThread::run()` 入口添加 `if (!fmtCtx_) return`
- **涉及文件**：`src/playercontroller.cpp`、`src/demuxthread.cpp`

---

## #004 — VideoRenderer::onTimer() 阻塞主线程导致 UI 卡死

- **日期**：2026-05-09
- **现象**：打开电影文件后 UI 完全卡死无响应
- **根因**：`onTimer()` 在 GUI 线程中调用 `QThread::msleep()`（最高 1000ms）等待音视频同步；且将帧 `push()` 回有界队列（容量 15），若队列满则阻塞主线程造成死锁（主线程既是唯一消费者又在阻塞等待空间）
- **修复**：引入 `pendingFrame_` 成员变量暂存未到渲染时间的帧，下次 timer 回调再检查；彻底去除主线程上的 `msleep` 和 `push` 回队列操作
- **涉及文件**：`src/videorenderer.h`、`src/videorenderer.cpp`

---

## #005 — QAudioOutput 线程亲和性违规

- **日期**：2026-05-09
- **现象**：打开文件后可能卡死或行为异常
- **根因**：`QAudioOutput` 在主线程 `init()` 中创建，但在工作线程 `run()` 中调用 `start()` / `write()` / `stop()`，违反 Qt 线程亲和性规则（QObject 应在创建它的线程中使用）
- **修复**：将 `QAudioOutput` 的创建从 `init()` 移至 `run()`，确保创建、start、write、stop 全部在同一工作线程内完成；`run()` 退出时 delete，析构函数做兜底清理
- **涉及文件**：`src/audiodecodethread.cpp`

---

## #006 — 关闭窗口时 UAF 崩溃：renderer_ 先于 player_ 析构

- **日期**：2026-05-10
- **现象**：加载视频后直接关闭窗口，崩溃于 `VideoRenderer::stopRendering()` 中的 `av_frame_free(&pendingFrame_)`（访问违规）
- **根因**：`player_` 以 MainWindow 为 Qt parent，析构顺序为：`delete ui`（含 `renderer_`）→ Qt 自动删除 `player_`。`~PlayerController()` 调 `stop()` → `renderer_->stopRendering()`，此时 `renderer_` 已随 `ui` 销毁，造成 Use-After-Free
- **修复**：`player_` 构造时不传 `this` parent，改由 `~MainWindow()` 手动按顺序 `delete player_; delete ui;`，保证 `stop()` 在 `renderer_` 仍存活时执行
- **涉及文件**：`src/mainwindow.cpp`

---

## #007 — 关闭窗口时 UAF 崩溃：FrameQueue 先于线程析构导致 abort() 访问已销毁 mutex_

- **日期**：2026-05-10
- **现象**：关闭窗口时崩溃于 `FrameQueue::abort()` 中的 `QMutexLocker lk(&mutex_)`（访问违规）
- **根因**：`PlayerController` 中线程成员（`demux_`、`videoDec_`、`audioDec_`）声明在队列（`videoPacketQ_` 等）之前。C++ 成员析构为声明逆序，导致三条队列先被销毁，线程析构时 `stop()` 再次调用 `abort()`，访问已析构队列的 `mutex_` — UAF
- **修复**：将三条 `FrameQueue` 成员移至线程成员声明之前，确保队列比线程活得更久
- **涉及文件**：`src/playercontroller.h`

---

## #008 — Seek 后画面不同步、UI 冻结、音频饿死

- **日期**：2026-05-10
- **现象**：点击进度条跳转后，画面停在旧位置不动，UI 随即冻结；几秒后音频也断；进度条偶发点击无响应
- **根因**：多个环节叠加导致死锁链：
  1. `AVSync::videoDelay()` 对"严重落后"和"轻微落后/同步"都返回 0，`VideoRenderer::onTimer()` 拿 0 当"立即渲染"，对 seek 后队列中所有过期旧帧逐一执行 `sws_scale + update()`，1ms 定时器把主线程绘制事件淹没 → UI 冻结
  2. `videoFrameQ_` 来不及消费而堵满 → `VideoDecodeThread` 阻塞在 push → `videoPacketQ_` 满 → `DemuxThread` 无法读新包 → `audioPacketQ_` 耗尽 → 音频饿死
  3. `PlayerController::seek()` 先 flush 线程再更新音频钟，flush 执行期间 `videoFrameQ_` 里的旧帧 `diff ≈ 0` 不会被丢弃，进一步加剧堵塞
  4. `AudioDecodeThread` flush 时未排空输入队列，seek 竞态下仍会解码到旧 PTS 的包，时钟回跳
  5. 进度条点击只触发 `sliderMoved`（拖拽信号），鼠标单击不发信号，点击无响应
- **修复**（多次迭代，最终方案）：
  - **`VideoRenderer`**：弃用 `AVSync::videoDelay()`，改为直接计算 `diff = pts - audioClock`；`diff < -0.4s`（正向 seek 旧帧）或 `diff > 1.5s`（反向 seek 旧帧）时直接 `av_frame_free` 不渲染不 `update()`，彻底避免主线程被渲染洪流淹没
  - **`VideoRenderer::flushPendingFrame()`**：新增接口，seek 时由 `PlayerController` 调用，清除暂存的旧帧，避免旧 PTS 卡住帧队列消费
  - **`PlayerController::seek()`**：调整调用顺序为：先 `sync_.setAudioClock(seconds)` 让渲染器立刻看到新位置触发旧帧丢弃，再 `flushPendingFrame()`，最后 flush 各线程
  - **`AudioDecodeThread`**：flush 时先 `tryPop` 排空输入队列残留旧包；时钟改为直接用帧 `PTS * timeBase` 而非 `processedUSecs` 推算（`processedUSecs` 在 `stop/start` 后重置为 0，seek 后时钟会瞬间归零再爬升，与视频严重不同步）；flush 后 `setBufferSize(16384)` 保持缓冲区大小稳定
  - **`MainWindow`**：对进度条 `installEventFilter`，拦截 `MouseButtonPress`，用 `QStyle::sliderValueFromPosition` 计算精确点击值并手动调 `onSeekSliderMoved`
- **涉及文件**：`src/videorenderer.h`、`src/videorenderer.cpp`、`src/playercontroller.cpp`、`src/audiodecodethread.cpp`、`src/mainwindow.h`、`src/mainwindow.cpp`

---

## #009 — 暂停键/空格键无法暂停声音

- **日期**：2026-05-10
- **现象**：点击暂停按钮或按空格键后，视频画面停止，但音频仍持续播放
- **根因**：`PlayerController::pause()` 只调用了 `renderer_->stopRendering()`，`AudioDecodeThread` 继续运行并向 `QAudioOutput` 写 PCM；且 `MainWindow` 无 `keyPressEvent`，空格键未绑定任何动作
- **修复**：`AudioDecodeThread` 增加 `paused_` 原子标志和 `setPaused(bool)` 方法；`run()` 循环检测到暂停时调 `sink_->suspend()`，恢复时调 `sink_->resume()`；`PlayerController::pause()` / `play()` 分别调 `setPaused(true/false)`；`MainWindow` 新增 `keyPressEvent`，空格键触发 `onPlayPause()`
- **涉及文件**：`src/audiodecodethread.h`、`src/audiodecodethread.cpp`、`src/playercontroller.cpp`、`src/mainwindow.h`、`src/mainwindow.cpp`

---

## #011 — 向后 Seek 声音恢复但画面卡死

- **日期**：2026-05-11
- **现象**：向前快进（向后拖动进度条）正常；向后 seek（往回拖动进度条）后声音恢复播放，画面却完全卡死不更新，日志中 seek 完成后无任何 `VideoRenderer` 输出
- **根因**：`PlayerController::seek()` 立即把 `audioClock` 设为目标位置（向后 seek 时新值比当前 PTS 小）。视频帧队列中残留的旧位置帧（PTS 远大于新时钟），`VideoRenderer::onTimer()` 计算 `diff = pts - audioClock` 得正数 → 进入"视频超前"分支 → 暂存到 `pendingFrame_` 等待音频追上来。但音频在 seek 目标位置播放，要追上旧帧的 PTS 需要数百秒实时间。`pendingFrame_` 持续不为空，renderer 永远不再从队列取新帧 → **画面永久卡死**
- **修复**：
  - `VideoRenderer::onTimer()` 在"落后丢弃"（`diff < -0.4`）和"超前暂存"（`diff > 0`）之间，新增对称的丢弃检查：`diff > 5.0` 秒时直接丢弃帧。5 秒阈值远超正常播放时视频可能超前的最大值（< 0.5s），不会误杀正常帧；但足以清除向后 seek 后数百秒偏移的残留旧帧，防止其卡死 `pendingFrame_`
  - `VideoDecodeThread::run()` flush 路径增加 `qInfo` 日志，与 `AudioDecodeThread` 对齐，便于后续诊断
- **涉及文件**：`src/videorenderer.cpp`、`src/videodecodethread.cpp`

---

## #012 — 播放中切换视频崩溃于 ff_rfps_calculate

- **日期**：2026-05-11
- **现象**：播放视频时通过最近文件菜单选择另一个视频，程序崩溃，调用栈指向 `ff_rfps_calculate(AVFormatContext *ic)`
- **根因**：`PlayerController::open()` → `stop()` 只停线程、不关 `fmtCtx_`。随后 `DemuxThread::open()` 直接调 `avformat_open_input(&fmtCtx_, ...)`，此时 `fmtCtx_` 仍指向旧文件的 `AVFormatContext`。FFmpeg 在 `avformat_find_stream_info` → `ff_rfps_calculate` 阶段访问已失效的内部流数据，导致崩溃
- **修复**：`DemuxThread::open()` 开头增加 `if (fmtCtx_) avformat_close_input(&fmtCtx_);` 释放旧上下文，同时重置 `videoIdx_` / `audioIdx_` 为 -1
- **涉及文件**：`src/demuxthread.cpp`

---

## #010 — 音量滑块点击不跳转：QSlider 默认 pageStep 步进

- **日期**：2026-05-10
- **现象**：点击音量条轨道，滑块只向点击方向移动一个 pageStep（约 10），无法直接跳到点击位置
- **根因**：Qt `QSlider` 默认鼠标点击行为是 `pageStep` 步进，不会跳到点击坐标对应的值；原 `eventFilter` 只处理了进度条，未覆盖音量滑块
- **修复**：对 `volumeSlider` 也 `installEventFilter`，在 `eventFilter` 中用 `QStyle::sliderValueFromPosition` 计算点击坐标对应值并 `setValue`；同时将两个滑块的处理合并为统一的 `qobject_cast<QSlider*>` 逻辑，进度条额外手动调 `onSeekSliderMoved`（因其连接的是 `sliderMoved` 而非 `valueChanged`）
- **涉及文件**：`src/mainwindow.cpp`
