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

## #010 — 音量滑块点击不跳转：QSlider 默认 pageStep 步进

- **日期**：2026-05-10
- **现象**：点击音量条轨道，滑块只向点击方向移动一个 pageStep（约 10），无法直接跳到点击位置
- **根因**：Qt `QSlider` 默认鼠标点击行为是 `pageStep` 步进，不会跳到点击坐标对应的值；原 `eventFilter` 只处理了进度条，未覆盖音量滑块
- **修复**：对 `volumeSlider` 也 `installEventFilter`，在 `eventFilter` 中用 `QStyle::sliderValueFromPosition` 计算点击坐标对应值并 `setValue`；同时将两个滑块的处理合并为统一的 `qobject_cast<QSlider*>` 逻辑，进度条额外手动调 `onSeekSliderMoved`（因其连接的是 `sliderMoved` 而非 `valueChanged`）
- **涉及文件**：`src/mainwindow.cpp`

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

## #013 — Seek 后实际播放位置偏移 10–20 秒

- **日期**：2026-05-16
- **现象**：按右箭头快进 10 秒，实际跳 20–30 秒；按左箭头后退 10 秒，反而净前进约 10 秒（方向反转）
- **根因**（经两轮日志确认）：`AudioDecodeThread::run()` flush 路径中的**第一轮清空**（`clear-1`）是真正凶手。
  - PlayerController::seek() 已执行 `audioPacketQ_.clear()`，DemuxThread::handleSeek() 紧接也 clear 一次，此后队列仅含 seek 目标位置之后的有效新包
  - DemuxThread 以文件 I/O 全速向队列推包；AudioDecodeThread 检测到 `flush_` 时队列中已积压 **500+ 个有效包**（覆盖约 11s 音频）
  - clear-1 把这 500+ 个有效包全部丢弃，DemuxThread 文件指针超前 ~11s；flush gen done 后消费起点变成 `seekTarget + 11s`，时钟跳升
  - 第一轮清空的初衷是清旧包，但 PlayerController 和 handleSeek 已完成清旧，实际清的全是有效新包
  - 第二轮清空同理（在后续迭代中已删除，但 clear-1 仍存在导致问题延续）
- **修复**：
  - 完全删除 `AudioDecodeThread` flush 路径中的 clear-1（不再主动清包）
  - 依赖 PlayerController 的 `audioPacketQ_.clear()` + DemuxThread `handleSeek` 的 tryPop 清旧，两步已足够
  - 保留 `minAcceptablePts_` 时钟过滤器（由 `flush(seekTargetSec)` 传入），应对极少数竞态漏入的 1–2 个旧包
- **涉及文件**：`src/audiodecodethread.h`、`src/audiodecodethread.cpp`、`src/playercontroller.cpp`

---

## #014 — Seek 后视频冻结 2–5 秒（H.264 精确 seek 丢弃参考帧）

- **日期**：2026-05-16
- **现象**：按右箭头快进或拖动进度条后，声音立即跳到新位置播放，但视频画面冻结 2–5 秒才更新；日志显示 `VideoRenderer: no frame for Xms (queue empty, dropCount=1)`
- **根因**：`DemuxThread::handleSeek` 执行完关键帧 seek 后，`run()` 的"精确 seek"阶段会丢弃关键帧到目标之间的**所有**视频包（包括 IDR 帧本身），只推送 PTS ≥ target 的包。VideoDecodeThread 调用 `avcodec_flush_buffers` 后缺少参考帧，收到的首包若为 P/B 帧则持续返回 `EAGAIN`，直到遇到**下一个 IDR 帧**才能输出解码帧。H.264 GOP 最长可达 8–10 秒，导致视频冻结对应时长
- **修复**：精确 seek 阶段改为**仅丢弃目标前的音频包**（防止播放旧音频），视频包从关键帧起全部放行送入 VideoDecodeThread；VideoDecodeThread 从关键帧开始正常解码，VideoRenderer 凭 `diff < -0.4s` 快速丢弃目标前旧帧，视频恢复时间从 2–5 s 缩短至 < 500 ms
- **涉及文件**：`src/demuxthread.cpp`

---

## #015 — 点击进度条后方向键快进/快退失效（QSlider 抢焦点）

- **日期**：2026-05-16
- **现象**：用键盘左右方向键快进/快退正常；鼠标点击进度条跳转后，再按方向键无任何 seek 效果
- **根因**：`eventFilter` 拦截进度条的 `MouseButtonPress` 并正确触发 seek，但 Qt 在事件分发后仍将输入焦点转移给 `QSlider`。此后 `Key_Left/Key_Right` 被 `QSlider` 的内置 `keyPressEvent` 消费（用于调整滑块步进），事件不再冒泡到 `VideoRenderer` 或 `MainWindow`，`eventFilter` 和 `keyPressEvent` 中的 seek 逻辑均不触发
- **修复**：
  1. 构造函数中对进度条、音量条、播放按钮设置 `Qt::NoFocus`，使点击不改变焦点，`renderer_` 始终持有焦点
  2. 连接 `progressSlider::sliderReleased` 信号，拖拽松开后显式调 `renderer_->setFocus()`，覆盖拖拽场景
- **涉及文件**：`src/mainwindow.cpp`

---

## #016 — 进度条拖拽失效：eventFilter 拦截手柄点击导致无法滑动

- **日期**：2026-05-16
- **现象**：点击进度条轨道跳转正常，但拖拽进度条滑块无法滑动；只能点不能拖
- **根因**：`#015` 修复中 `eventFilter` 无差别拦截所有 `QSlider` 的 `MouseButtonPress`，包括手柄上的点击。手柄点击被 `return true` 消费后，slider 收不到事件无法启动拖拽状态
- **修复**：通过 `QStyleOptionSlider` + `QStyle::subControlRect(CC_Slider, SC_SliderHandle)` 获取手柄矩形；若点击坐标在手柄内 → `return false` 放行给 slider 原生拖拽；若在轨道上 → 点哪跳哪（原有行为）
- **涉及文件**：`src/mainwindow.cpp`

---

## #017 — 旧推流方案（CaptureThread 录屏）设计缺陷，已重构废弃

- **日期**：2026-05-17
- **现象**：Phase 9 初版推流使用 `CaptureThread`（gdigrab 录屏）采集桌面画面后编码推流；存在两个根本性缺陷：① vcpkg 默认 FFmpeg 不含任何 H.264 编码器（缺少 `ffmpeg[gpl,x264]`），推流静默失败无提示；② 录屏推流与播放器定位不符，属于 OBS 的功能而非媒体播放器应有的功能，且存在"解码→显示→截图→重编码"的无意义像素往返
- **根因**：推流源选型错误，应直接从播放器解码管线分叉帧，而非重新采集屏幕
- **修复方案**：Phase 9 整体重构，删除 `CaptureThread`，改为在 `VideoDecodeThread` / `AudioDecodeThread` 中分叉解码帧直接推入编码管线；新增 `AudioEncodeThread`（AAC），`MuxThread` 支持音视频双流、RTMP 和 SRT 协议、多路同时推流
- **涉及文件**：删除 `src/capturethread.h/.cpp`；修改 `src/videodecodethread`、`src/audiodecodethread`、`src/encodethread`、`src/muxthread`、`src/streamcontroller`、`src/mainwindow`；新增 `src/audioencodethread`
- **详细计划**：`docs/superpowers/plans/2026-05-17-phase9-restream.md`

---

## #018 — 推流时关闭窗口 UAF 崩溃：FrameQueue 先于 DemuxThread 解除引用

- **日期**：2026-05-20
- **现象**：推流进行中直接关闭窗口，崩溃于 `FrameQueue::abort()` 内的 `QMutexLocker lk(&mutex_)`，异常码 `W32/0xC0000005`，读地址 `0xFFFFFFFFFFFFFFFF`
- **根因**：`~MainWindow()` 析构顺序错误：先调 `streamCtrl_->stop()` 销毁 `videoMuxQueues_`（`unique_ptr` 容器 `clear()`，`FrameQueue` 对象释放），再 `delete player_` 触发 `PlayerController` 析构 → `DemuxThread::stop()` → `clearRestreamQueues()` → `q->abort()`，此时 `q` 指向已释放的 `FrameQueue`，访问其内部 `mutex_` 发生 Use-After-Free
- **修复**：在 `~MainWindow()` 中，`streamCtrl_->stop()` 之前先调 `player_->clearRestreamPacketQueues()`，让 DemuxThread 在队列对象仍存活时解除引用并 abort；之后 `streamCtrl_->stop()` 销毁队列时已无人持有裸指针
- **涉及文件**：`src/mainwindow.cpp`

---

## #019 — MuxThread 视频 PTS 单调性检查误报 B 帧，掩盖真实 DTS 异常

- **日期**：2026-05-20
- **现象**：推流含 B 帧的 H.264 视频时，日志持续输出大量 `video PTS non-monotonic` 警告，干扰真实问题排查
- **根因**：MuxThread 监控 `videoLastOut_`（PTS）的单调性，但 FLV/RTMP 按 DTS 顺序传输包，B 帧的 PTS 天然非单调（如 IBBP 序列 PTS 顺序为 0 3 1 4 2），属正常编码特性，不代表传输异常。真正需要保证单调递增的是 DTS，PTS 非单调对 FLV 容器完全合法
- **修复**：将检查变量从 PTS（`videoLastOut_` 存 `pkt->pts`）改为 DTS（`videoLastOut_` 存 `pkt->dts`），日志文案同步改为 `video DTS non-monotonic`；仅在 DTS 回跳时才报警，消除 B 帧误报
- **涉及文件**：`src/muxthread.cpp`

---

## #020 — 网页推流播放器默认静音无声音

- **日期**：2026-05-21
- **现象**：局域网浏览器打开推流页面，只有画面没有声音
- **根因**：HTML `<video autoplay muted>` 的 `muted` 属性是浏览器 autoplay 策略要求（去掉则 `play()` 被拒绝导致完全无画面），但导致播放器永远静音
- **修复**：保留 `muted` 保证自动播放，`play().then()` 成功后若 `v.muted === true` 则在右上角显示"🔊 点击开声音"按钮，用户点击后执行 `v.muted=false; v.volume=1`
- **涉及文件**：`src/httpflvserver.cpp`（`buildPlayerHtml()`）

---

## #021 — HTTP-FLV 初次推流音频落后画面约 5 秒

- **日期**：2026-05-21
- **现象**：网页加载直播流后，声音比画面晚出 5 秒左右
- **根因**：音频未参与 `needsKeyframe_` 门控，在首个视频关键帧到达前持续写入 FLV 流。等待期间约 5 秒的音频数据（PTS 0→5s）被写入 `flvHeader_`；而视频从首帧关键帧（PTS≈1ms）开始，导致 flv.js 看到 audio PTS≈5s、video PTS≈1ms，等视频追上音频需要 5 秒——表现为声音"慢了 5 秒"
- **修复**：音频与视频统一门控（`needsKeyframe_` 条件下丢弃音频帧），等首个视频关键帧确认后再开放音频写入。`flvHeader_` 冻结点改为"首帧音频写入后"而非"首帧关键帧前"，确保 `flvHeader_` 含 AVC 序列头 + I 帧 + AAC 序列头，音视频 PTS 均从 ~1ms 起步
- **涉及文件**：`src/httpflvserver.cpp`

---

## #022 — HTTP-FLV seek 后声画偏差 0.5s

- **日期**：2026-05-21
- **现象**：快进后画面与声音同步，但声音整体落后画面约 0.5 秒
- **根因**：`av_seek_frame(BACKWARD)` 落到 seek 目标前的关键帧（如目标 60s 落到 59.5s），视频从 59.5s 开始输出，但 DemuxThread 的 `seekExactAudioTarget_` 将音频过滤到目标点（60s）。两路都重映射到相同的 `accumPts`（T），但 `videoSegBase_=59.5s`、`audioSegBase_=60s`，同一输出 PTS T 对应的源内容相差 0.5s，导致持续偏差
- **修复**：在 `HttpFlvServer` 中，首个关键帧通过门控时记录其源 PTS（`firstKeyframeSrcSec_`）；首帧音频到来时计算 `gapSec = audioSrcSec - firstKeyframeSrcSec_`，将 `audioAccumPts_` 向后偏移相同量，使同一源时刻的音视频输出 PTS 完全一致
- **涉及文件**：`src/httpflvserver.cpp`、`src/httpflvserver.h`

---

## #023 — HTTP-FLV 晚连接客户端白屏 6 秒 + 修复后无声音

- **日期**：2026-05-22
- **现象**：推流已运行数秒后，在平板浏览器打开直播页面，开头白屏约 6 秒才出现画面，且画面内容滞后桌面约 6 秒；按上述方案修复后白屏消失，但完全无声音
- **根因（白屏/延迟）**：`flvHeader_` 在推流启动时冻结，含第一帧 PTS≈0ms。晚连客户端收到 `flvHeader_`（PTS=0）后紧接收到直播数据（PTS≈N×1000ms），flv.js 要"快进"跳过 N 秒时间轴，表现为白屏等待；此外 flv.js 默认不主动追直播边缘，浏览器缓冲持续积压导致稳态延迟
- **根因（无声音）**：为减少延迟新增了 JS `setInterval` 每 2 秒执行 `v.currentTime = bufferedEnd - 0.2`，直接修改 `currentTime` 在移动端 WebView（iOS/Android）会重置音频解码器状态导致静音；同时 `liveBufferLatencyChaseOffset: 0.2`（1.2x 倍速）在部分移动浏览器会触发音频静音策略
- **修复（白屏）**：
  1. `headerFrozen_` 后调 `buildCodecConfigHeader()` 将 `flvHeader_` 解析为仅含 FLV 文件头 + metadata + AVC 序列头 + AAC 序列头的 `codecConfigHeader_`（无帧 PTS 数据）
  2. 每次检测到关键帧时设 `newGopStarting_=true`，`writeCallback` 据此清空并重建 `currentGopBytes_`，始终保存最近一个 I 帧起的全部 FLV tag 字节
  3. `startStreaming()` 晚连路径改为发送 `codecConfigHeader_` + `currentGopBytes_`，客户端直接从最新关键帧起步，无时间戳跳变
- **修复（无声音）**：删除手动 `v.currentTime` 跳帧的 `setInterval`，完全依赖 flv.js 内置追帧机制；将 `liveBufferLatencyChaseOffset` 从 0.2 降至 0.1（1.1x 倍速，移动端安全上限）；新增 `autoCleanupSourceBuffer:true` 防止历史缓冲膨胀
- **涉及文件**：`src/httpflvserver.cpp`、`src/httpflvserver.h`

---

## #024 — 切换视频时播放按钮状态不同步（显示 play 但视频仍在播放）

- **日期**：2026-05-23
- **现象**：播放视频期间从最近文件菜单切换另一个视频，新视频正常播放，但播放按钮显示为 play 图标（关闭状态）
- **根因**：`PlayerController::onDemuxFinished()` 用 `QTimer::singleShot(500ms)` 延迟发出 `playbackFinished` 信号（目的是给解码线程留时间耗尽队列）。切换视频时 `openFile()` 在 `player_->play()` 后将按钮设为暂停图标，但 500ms 后旧文件的延迟信号触发 `onPlaybackFinished()`，该函数无条件将按钮改回 play 图标，覆盖了新视频的状态
- **修复**：在 `MainWindow::onPlaybackFinished()` 开头加 `if (player_->isPlaying()) return;`，若此时新文件已在播放则忽略旧文件的结束事件
- **涉及文件**：`src/mainwindow.cpp`

---

## #025 — HTTP-FLV 推流不兼容编解码时静默失败并大量丢包

- **日期**：2026-05-23
- **现象**：对 MPEG-4 编码的视频启动 HTTP-FLV 推流，`avformat_write_header` 失败，随后日志刷出数百条 `restream tryPush drop — mux queue full` 警告，用户弹窗只显示"服务初始化失败"，原因不明
- **根因**：FLV 容器只支持 H.264 视频，MPEG-4（Part 2）编码无法写入 FLV；`HttpFlvServer::init()` 未提前检查 codec 兼容性，导致 `avformat_write_header` 才报错。`run()` 的失败分支未 abort 队列，DemuxThread 持续向已退出线程的队列 push 包直到打满，造成大量丢包警告
- **修复**：
  1. `HttpFlvServer::init()` 开头检查 `vpar->codec_id != AV_CODEC_ID_H264`，不兼容时直接返回 false 并输出明确日志
  2. `run()` 的 `initMuxer` 失败分支立即 `videoQueue_->abort(); audioQueue_->abort()`，截断后续丢包
  3. `StreamController` 的错误信息改为包含 codec 名称：`"HTTP-FLV 推流失败：视频编码 mpeg4 不支持 FLV，请使用 H.264 编码的视频"`
- **涉及文件**：`src/httpflvserver.cpp`、`src/streamcontroller.cpp`

---

## #026 — 预配置推流（先配推流后开文件）平板端 10 秒才有画面

- **日期**：2026-05-25
- **现象**：先配置 HTTP-FLV 推流、再打开视频文件，平板浏览器连接后白屏约 10 秒才出画面，日志显示首个关键帧 PTS=9.927
- **根因**：`MainWindow::openFile()` 先调 `player_->play()` 启动 DemuxThread，再调 `startStreaming()` 注册推流队列。DemuxThread 在队列注册前已读完第一个关键帧（PTS≈0），推流通道只收到后续帧；`HttpFlvServer::needsKeyframe_` 门控丢弃所有非关键帧，必须等下一个 GOP 的关键帧（~10s）。同时平板早连时 `codecConfigHeader_` 为空，`startStreaming()` 发空 `flvHeader_` 后客户端无有效编解码配置，即使关键帧到达后广播数据也无法解码
- **修复**：
  1. `startStreaming()` 移到 `player_->play()` 之前，确保 DemuxThread 启动时推流队列已就位，首个关键帧即进入推流通道
  2. `HttpFlvServer` 新增 `pendingClients_` 列表：早连客户端不发空数据，暂存等待；FLV 头冻结后补发 `codecConfigHeader_` + `currentGopBytes_` 再移入广播列表，保证客户端始终从合法起点开始解码
  3. `removeClient()` 同步清理 `pendingClients_`，避免断连残留
  4. 无音频流冻结路径补建 `codecConfigHeader_` / `currentGopBytes_`，与有音频流路径一致
- **涉及文件**：`src/mainwindow.cpp`、`src/httpflvserver.h`、`src/httpflvserver.cpp`

---

## #027 — HTTP-MPEG-TS 多次 seek 后浏览器直播延迟逐步升高

- **日期**：2026-05-26
- **现象**：使用 HTTP-MPEG-TS 低延迟浏览器推流电影时，多次 seek 后浏览器端延迟逐渐升高；日志显示 seek 后 `MpegTsServer` 持续保持旧 `/stream.ts` 连接，且同一 IP 多次重连后客户端数从 1 增至 3
- **根因**：seek 后解码预滚会产生目标点前的 GOP 重叠视频数据，旧浏览器连接继续接收这些数据并写入 MSE SourceBuffer；同时旧 socket 未及时替换，服务端可能继续向滞后连接广播，导致写缓冲和浏览器缓冲累积
- **修复**：
  1. `MpegTsServer` 收到视频 seek sentinel 后主动断开所有 TS 流客户端，让播放页自动重连并重建 SourceBuffer
  2. 新 `/stream.ts` 请求会替换同一 IP 的旧流连接，避免同一浏览器反复重连后旧 socket 残留（**注**：此 IP 去重机制后被 #033 移除，多设备同 IP 场景下会误杀合法客户端）
  3. 广播 TS 数据时检测 `QTcpSocket::bytesToWrite()`，写缓冲超过 1 MB 的客户端直接断开，防止慢客户端拖累直播链路
  4. 内嵌 mpegts.js 播放页断流重连等待从 2 秒降至 300 ms（`ERROR` 回调），并监听 `stalled` / `emptied` / `abort` 快速重连（后因重连风暴在 #028 中撤销）
  5. HTTP-MPEG-TS 晚连客户端只发送 PAT/PMT header，不再预灌 `segmentBytes_` 历史 GOP，避免 seek 重连后先消费历史片段形成固定延迟
- **涉及文件**：`src/mpegtsserver.cpp`、`src/mpegtsserver.h`

---

## #028 — HTTP-MPEG-TS 快进后重连风暴触发 QList 迭代器断言

- **日期**：2026-05-26
- **现象**：HTTP-MPEG-TS 推流时按右方向键快进，浏览器端连续请求 `/stream.ts`，随后程序报 `QList::erase` 断言失败：`The specified iterator argument 'it' is invalid`
- **根因**：`disconnectClientsFromPeer()` 使用 `QMutableListIterator` 遍历 `streamClients_` 时调用 `QTcpSocket::disconnectFromHost()`；断开信号可能同步进入 `removeClient()` 修改同一个 `QList`，导致当前迭代器失效。同时前一次修复新增的 `stalled` / `emptied` / `abort` 事件重连过于敏感，`destroy()` / `unload()` 过程也可能触发事件，造成 300 ms 间隔的重连风暴
- **修复**：
  1. 替换旧客户端时先收集 socket 指针，再从 `streamClients_` / `pendingClients_` 中移除，最后统一 `disconnectFromHost()`，避免遍历期间修改 QList
  2. 内嵌播放页撤销 `stalled` / `emptied` / `abort` 事件重连，只保留 mpegts.js `ERROR` 回调快速重连，并将重连间隔设为 500 ms
- **涉及文件**：`src/mpegtsserver.cpp`
- **注**：`disconnectClientsFromPeer()` 方法后因多设备 NAT 场景下误杀同 IP 客户端，在 #033 中被彻底移除

---

## #029 — HTTP-MPEG-TS seek 后先推送 seek 目标前旧画面

- **日期**：2026-05-26
- **现象**：HTTP-MPEG-TS 推流时向前 seek 10 秒，浏览器端先播放 seek 之前的画面，几秒后才切到新位置；日志显示 target=35.045s，但 `EncodeThread: first frame after sentinel pts=2700000`（约 30s），随后 `preTargetVideoFrames=115`
- **根因**：DemuxThread 为保证 H.264 参考链完整，会从目标前关键帧开始把 pre-target 视频包送入 HTTP-MPEG-TS 的解码队列；但 StreamDecoder 解码出的 pre-target 帧没有被拦截，直接进入 EncodeThread 重编码，导致服务端先推送 30s–35s 的旧画面
- **修复**：
  1. `StreamDecoder` 新增 seek 最小输出 PTS 门控：继续消费目标前包完成解码预滚，但丢弃目标前解码帧
  2. `StreamPipeline` 记录视频流 time_base，并暴露 `setSeekTargetSeconds()` 给 UI seek 入口调用
  3. `MainWindow` 在进度条和左右方向键 seek 前通知所有 HTTP-MPEG-TS 管线设置 seek 目标，确保编码器第一帧接近目标位置
  4. `MpegTsServer` seek 断流改用 `QTcpSocket::abort()` 立即关闭 TS 连接，避免浏览器继续消费旧 HTTP 流缓冲
  5. 内嵌播放页在重连前立即销毁 mpegts.js player、清空 `<video>` 的 `src` 并调用 `load()`，先清掉旧 MSE/视频缓冲再重新连接
  6. seek sentinel 后 `MpegTsServer` 即使已有 `headerFrozen_`，也在新关键帧到达前把新 `/stream.ts` 连接挂到 `pendingClients_`，避免重连过早接入旧段尾部实时广播
  7. 暂停恢复重接 HTTP-MPEG-TS 管线时同步设置 `StreamDecoder` 最小输出时间，防止 DemuxThread 恢复时的超前旧帧直接进入编码器
- **涉及文件**：`src/streamdecoder.h`、`src/streamdecoder.cpp`、`src/streampipeline.h`、`src/streampipeline.cpp`、`src/streamcontroller.cpp`、`src/mainwindow.h`、`src/mainwindow.cpp`、`src/mpegtsserver.cpp`

---

## #030 — 启动推流时 VideoRenderer 短暂白屏

- **日期**：2026-05-25
- **现象**：打开文件启动推流后，本地播放窗口在首帧渲染前短暂闪白
- **根因**：`VideoRenderer` 的 `pendingFrame_` 和 render buffer 未显式初始化，QImage 默认填充白色背景，首帧解码完成前的若干 ms 窗口显示纯白
- **修复**：构造时对 `pendingFrame_` 的 QImage 调用 `fill(Qt::black)`，确保首帧到达前显示黑屏而非白屏
- **涉及文件**：`src/videorenderer.cpp`

---

## #031 — HTTP-MPEG-TS seek 后 AAC/libx264 编码器 PTS 异常

- **日期**：2026-05-26
- **现象**：HTTP-MPEG-TS 推流 seek 后，日志出现 `audio write error: Invalid argument`（PTS 为负数），且视频编码中断
- **根因**：
  1. AAC 编码器：`avcodec_flush_buffers` 无法清除内部 ~2048 samples lookahead 缓冲，旧缓存帧在 flush 后继续输出，PTS 基于旧 `segBase_` 计算 → remap 后为负数，`av_write_frame` 拒绝写入
  2. libx264 编码器：`avcodec_flush_buffers` 后进入 EOS（end-of-stream）状态，后续帧无法编码输出
- **修复**：seek sentinel 到达时不再依赖 `avcodec_flush_buffers`，改为 `reopenCodec()` 销毁并重建编码器上下文（保存并恢复 codec、比特率、分辨率、time_base 等参数）。AAC 重建时先排空 FIFO 再创建新编码器
- **涉及文件**：`src/audioencodethread.h`、`src/audioencodethread.cpp`、`src/encodethread.h`、`src/encodethread.cpp`

---

## #032 — HTTP-MPEG-TS 暂停恢复后浏览器黑屏

- **日期**：2026-05-26
- **现象**：推流中暂停再恢复，浏览器端先播 1 秒旧缓冲，随后黑屏数秒才恢复；有时第二次暂停后再恢复完全无画面
- **根因**（多轮定位，四个子问题叠加）：
  1. **双重断开**：暂停时 `mainwindow.cpp` 调用 `requestMpegTsClientReconnect()` 断开浏览器，恢复后 seek sentinel 再次触发 `disconnectAllStreamClients`，浏览器经历两次重连
  2. **pending 空流超时**：浏览器断开后重连（~300ms）远快于首个关键帧到达（0.5–7s），`startStreaming()` 将其放入 pending 队列但已发送 HTTP 200 OK，浏览器收到空流后超时触发重连 → 死循环
  3. **headerBytes_ 旧音频 PTS 污染**：flush pending 时写入 `headerBytes_`（含文件开头 36KB 旧 TS 数据），其中音频 PTS 与当前播放位置不连续，mpegts.js 解析报错 → 再次重连
  4. **HTML reconnect() 丢事件**：`if(retryTimer)return;` 守卫导致第二次重连（如 ended 后 ERROR）被静默丢弃，浏览器彻底卡死
- **修复**：
  1. 删除暂停时的 `requestMpegTsClientReconnect()` 调用，恢复后 sentinel 只触发一次重连
  2. pending 客户端延迟发送 HTTP 响应头（暂存不写），等关键帧到达后 `flushPendingClients()` 统一发送 HTTP 200 + segmentBytes_
  3. 晚连和 flush 只写 `segmentBytes_`（含 `resend_headers=1` 周期重发的 PAT/PMT），不写 `headerBytes_`
  4. `reconnect()` 将 `if(retryTimer)return;` 改为 `clearTimeout(retryTimer)`，确保最新重连始终执行
  5. 增加 `/status` 诊断端点，可查看 streamClients/pendingClients/headerBytes/segmentBytes 状态
- **涉及文件**：`src/mainwindow.cpp`、`src/mpegtsserver.cpp`、`src/mpegtsserver.h`

---

## #033 — 两台设备同时连接 HTTP-MPEG-TS 轮流卡顿

- **日期**：2026-05-26
- **现象**：手机和平板同时连接推流页面，两台设备交替播放和停顿，每 1–2 秒轮换一次；日志中 `total` 始终为 1，频繁出现 `replacing old stream client`
- **根因**：`startStreaming()` 调用 `disconnectClientsFromPeer()` 按 IP 地址去重客户端。两台设备在同一 WiFi 下共享出口 IP，设备 B 连接时踢掉设备 A → A 断线重连又踢掉 B → 无限循环，每次只有一台能收到 TS 数据
- **修复**：删除 `disconnectClientsFromPeer()` 调用及方法定义。旧 socket 的清理由 `broadcastData()` 中的 `ConnectedState` 检查和 `QTcpSocket::disconnected` 信号接管。同时删除 `mpegtsserver.h` 中 `QHostAddress` 前向声明（不再需要）
- **涉及文件**：`src/mpegtsserver.cpp`、`src/mpegtsserver.h`
- **关联**：本修复撤销了 #027 第 2 点和 #028 中引入的 IP 去重机制，该机制在单设备场景下可防止旧 socket 残留，但在多设备 NAT 场景下误杀合法客户端

---

## #034 — ConcatDemuxer 临时文件格式与输入编码不兼容

- **日期**：2026-05-30
- **现象**：MergePanel「替换音频」模式，选择 1 个视频 + 多个 MP3 音频后点击开始，日志报错 `写入文件头失败 [fmt=ipod, err=Invalid argument]`，合成失败
- **根因**：MuxAV 多音频路径先将音频拼接为临时文件，临时文件扩展名 `.aac`（ADTS 格式），ADTS 容器只接受 AAC 编码；用户的音频为 MP3，`-c copy` 直通写入被 FFmpeg 拒绝
- **修复**：临时文件扩展名改为 `.m4a`（MOV/ipod 容器），同时将 MuxAV 多音频路径统一改为调 `execAudioConcat` 重编码为 AAC，彻底规避格式不兼容
- **涉及文件**：`src/mergeworker.cpp`

---

## #035 — execAudioConcat avfilter concat 导致多段音频全部丢失

- **日期**：2026-05-30
- **现象**：替换音频模式下，1 个视频 + 3 个音频，合成成功但输出文件完全没有声音；单个音频替换正常
- **根因**：`execAudioConcat` 使用 `concat=n=N:v=0:a=1` avfilter 拼接音频。该滤镜要求按段顺序驱动，且每段 EOF 前必须先刷新解码器缓冲；代码中未调用 `avcodec_send_packet(nullptr)` 刷新，导致解码器末尾缓存的帧永远不被送入滤镜，concat 滤镜等待超时后不输出任何数据，最终生成的临时音频文件为空，SimpleMuxer 写入的音频流无内容
- **修复**：完全重写 `execAudioConcat`，改为逐文件顺序解码 + `SwrContext` 重采样 + AAC 编码方案（无 avfilter），每个文件处理完毕后显式用 `avcodec_send_packet(nullptr)` 刷新解码器，确保所有帧都被编码写入
- **涉及文件**：`src/mergeworker.cpp`

---

## #036 — QWidgetAction 在 QMenu 中高度为零导致最近文件条目不显示

- **日期**：2026-05-30
- **现象**：最近文件子菜单改为用 `QWidgetAction` + 自定义 widget 实现后，菜单打开只剩「清除所有」一项，文件条目全部消失
- **根因**：自定义 widget 的 `paintEvent` 调用 `QStyle::CE_MenuItem` 进行绘制，Qt Style 在 `QWidgetAction` 上下文中计算该控件高度时返回 0，导致 `QMenu` 为这些条目分配的区域为空，视觉上完全不可见
- **修复**：放弃 `QWidgetAction` 方案，恢复使用原生 `QAction`（保证与「清除所有」风格完全一致），另创建一个悬浮 `QToolButton` 作为覆盖层，通过 `eventFilter` 监听菜单鼠标移动事件，动态将 × 按钮定位到 hover 条目右侧
- **涉及文件**：`src/mainwindow.cpp`、`src/mainwindow.h`
