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
