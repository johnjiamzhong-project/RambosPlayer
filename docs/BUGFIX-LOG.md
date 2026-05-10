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

## #008 — Seek 后 UI 卡死、音频饿死：videoDelay() 不区分"丢帧"与"立即渲染"

- **日期**：2026-05-10
- **现象**：点击进度条跳转后，音频先到达新位置，UI 立刻冻结，几秒后音频也断；进度条偶发不跳转
- **根因**：`AVSync::videoDelay()` 对"严重落后"和"轻微落后/同步"都返回 0，调用方无法区分。`VideoRenderer::onTimer()` 拿 0 当作"立即渲染"处理，对 seek 后队列中所有过期旧帧（PTS 远落后于新音频钟）逐一执行 `sws_scale + update()`。1ms 定时器把主线程绘制事件淹没 → UI 冻结 → `videoFrameQ_` 来不及消费而塞满 → `VideoDecodeThread` 阻塞在 push → `videoPacketQ_` 堵满 → DemuxThread 无法读新包 → audioPacketQ_ 耗尽 → 音频饿死
- **修复**：`onTimer()` 自行计算 `diff = pts - audioClock`，落后超过 0.4s 时直接 `av_frame_free` 不渲染、不 `update()`，避免主线程被过期帧的渲染流量淹没
- **涉及文件**：`src/videorenderer.cpp`

---

## #007 — 关闭窗口时 UAF 崩溃：FrameQueue 先于线程析构导致 abort() 访问已销毁 mutex_

- **日期**：2026-05-10
- **现象**：关闭窗口时崩溃于 `FrameQueue::abort()` 中的 `QMutexLocker lk(&mutex_)`（访问违规）
- **根因**：`PlayerController` 中线程成员（`demux_`、`videoDec_`、`audioDec_`）声明在队列（`videoPacketQ_` 等）之前。C++ 成员析构为声明逆序，导致三条队列先被销毁，线程析构时 `stop()` 再次调用 `abort()`，访问已析构队列的 `mutex_` — UAF
- **修复**：将三条 `FrameQueue` 成员移至线程成员声明之前，确保队列比线程活得更久
- **涉及文件**：`src/playercontroller.h`

---

## #006 — 关闭窗口时 UAF 崩溃：renderer_ 先于 player_ 析构

- **日期**：2026-05-10
- **现象**：加载视频后直接关闭窗口，崩溃于 `VideoRenderer::stopRendering()` 中的 `av_frame_free(&pendingFrame_)`（访问违规）
- **根因**：`player_` 以 MainWindow 为 Qt parent，析构顺序为：`delete ui`（含 `renderer_`）→ Qt 自动删除 `player_`。`~PlayerController()` 调 `stop()` → `renderer_->stopRendering()`，此时 `renderer_` 已随 `ui` 销毁，造成 Use-After-Free
- **修复**：`player_` 构造时不传 `this` parent，改由 `~MainWindow()` 手动按顺序 `delete player_; delete ui;`，保证 `stop()` 在 `renderer_` 仍存活时执行
- **涉及文件**：`src/mainwindow.cpp`

---

## #005 — QAudioOutput 线程亲和性违规

- **日期**：2026-05-09
- **现象**：打开文件后可能卡死或行为异常
- **根因**：`QAudioOutput` 在主线程 `init()` 中创建，但在工作线程 `run()` 中调用 `start()` / `write()` / `stop()`，违反 Qt 线程亲和性规则（QObject 应在创建它的线程中使用）
- **修复**：将 `QAudioOutput` 的创建从 `init()` 移至 `run()`，确保创建、start、write、stop 全部在同一工作线程内完成；`run()` 退出时 delete，析构函数做兜底清理
- **涉及文件**：`src/audiodecodethread.cpp`
