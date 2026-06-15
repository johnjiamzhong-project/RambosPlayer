# 如何用 Claude Code 从 0 开发项目

> 记录 RambosPlayer 项目的开发过程与架构理解，包括 Claude Code 工作流和关键设计决策。

## 结构化回答（STAR 框架）

### 工作流程

**1. 需求澄清阶段**

用 `/brainstorming` 做设计对话。Claude 会一次只问一个问题，逼你把模糊想法具体化，最后输出 spec 文档。好处是避免"先写代码再想需求"的常见错误。

**2. 计划阶段**

用 `/writing-plans` 把 spec 转成可执行的 TDD 任务列表。每个任务粒度是 2-5 分钟，包含具体代码、测试、命令和预期输出，不是模糊的"实现 XX 功能"。

**3. 执行阶段**

用 subagent-driven-development，每个任务派一个新的 subagent 执行，执行完做代码审查，再继续下一个。这样上下文不会污染，每步可控。

**4. 上下文管理**

用 CLAUDE.md 存项目架构规则，用 memory 系统跨会话保留用户偏好，避免每次重复解释背景。

---

### 关键收益（可量化的部分）

- Phase 1–10（项目脚手架 → FrameQueue → AVSync → DemuxThread → 解码线程 → VideoRenderer → PlayerController → MainWindow → 硬件加速 → 视频滤镜 → 推流管线 → 视频剪辑器）通过这套流程完整实现，覆盖多线程架构、解码、音视频同步、seek、D3D11VA 硬解、实时滤镜调参、解码帧分叉音视频双流推流（RTMP/SRT）、时间轴无损剪辑等完整多媒体功能
- 每个功能都有对应 spec 和 plan 文档，可追溯决策过程

---

### 强调的差异点

> "我不是把 Claude 当搜索引擎用，而是用它做结构化的工程决策流程——设计评审、任务分解、代码审查都在流程里，而不是单点问答。"

---

*基于 RambosPlayer 项目（FFmpeg + Qt 多媒体播放器）的实际开发经验，2026-04-26 起草，2026-05-15 更新至 Phase 10 完成*

---

## RambosPlayer 架构深度解析（技术面试版）

### 四层流水线架构

| 阶段 | 组件 | 作用 |
|------|------|------|
| 1️⃣ 解复用 | DemuxThread | 读取视频文件，分离视频/音频包 → 推入 2 条队列 |
| 2️⃣ 解码 | VideoDecodeThread + AudioDecodeThread | 消费包队列，解码成帧 |
| 3️⃣ 同步 | AudioDecodeThread + AVSync | 音频输出推动主时钟，视频帧依音频延迟决定绘制时间 |
| 4️⃣ 渲染 | VideoRenderer + MainWindow | 计算延迟后用 QPainter 绘帧，暴露播放/暂停/音量等控制 |

**核心设计原则**：音频是主时钟，视频跟随音频同步。

---

### 关键设计决策与技术亮点

#### 1. 跨线程通信：FrameQueue<T>（Header-only 模板）

**问题**：多个线程如何安全地传递数据，同时提供背压（防止内存溢出）？

**解决方案**：
```cpp
template<typename T>
class FrameQueue {
    void push(T item);                   // 满时阻塞；abort 后静默返回
    bool tryPop(T& out, int timeoutMs);  // 超时或 abort 返回 false
    void abort();                        // 唤醒所有等待线程
    void reset();                        // seek 后调用
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **同步原语** | QMutex + QWaitCondition | 与 Qt 技术栈一致，Windows 上性能等价 std::mutex |
| **push 返回值** | void（abort 后静默丢弃） | 调用方通过检查 aborted_ 自行退出，避免异常处理开销 |
| **abort 唤醒策略** | wakeAll()（而非 wakeOne()） | 多线程同时阻塞时，必须全部唤醒，否则线程泄露 |
| **有界容量** | 可配上限（video 100, audio 400） | 解复用线程生产速度 >> 消费速度时，队列充当缓冲 |

**验证**：5 个 TDD 测试（单线程 push/pop、超时、阻塞、abort、size/clear）全部通过。

---

#### 2. 解复用线程：DemuxThread

**问题**：如何让主线程之外的线程持续读包，同时支持随时停止和 seek？

**核心设计**：

```cpp
class DemuxThread : public QThread {
    void open(path, videoQueue, audioQueue);  // 打开文件，绑定队列
    void run() override;    // av_read_frame 循环，按 stream_index 分发
    void stop();            // 设 abort 标志，abort 两条队列
    void seek(double sec);  // 存原子量，run() 检测后执行 av_seek_frame
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **seek 请求传递** | `std::atomic<double>` | 无锁，run() 每帧检测一次，避免加锁暂停整个读包循环 |
| **包所有权** | `av_packet_clone()` 后推队列 | clone 使包有独立引用计数，消费方 `av_packet_free` 后不影响解复用侧 |
| **stop 级联** | `stop()` abort 两条包队列 | 解码线程阻塞在 `tryPop`，abort 队列是唤醒它们退出的唯一手段 |
| **seek 后清空** | `run()` 中 `av_seek_frame` 后 clear 两条队列 | 防止旧包污染新位置的解码结果 |

**验证**：3 个 TDD 测试（open 有效/无效文件、运行 300ms 后两队列均有包）全部通过；消费端统一调用 `av_packet_free` 无内存泄漏。

---

#### 3. 音视频同步策略

**问题**：如何精确同步音频和视频播放？

**设计架构**：
- **音频作为主时钟**：AudioDecodeThread 通过 QAudioOutput 输出 PCM，AVSync 维护原子量存储当前音频 PTS
- **视频跟随音频**：VideoRenderer 每 1ms 查询一次，调用 `AVSync::videoDelay()` 决定是否延迟绘制
  - `videoDelay() > 0`：视频超前，等待
  - `videoDelay() < -400ms`：视频落后超过 400ms，丢帧追赶

**为什么音频是主时钟？**
| 考量 | 音频 | 视频 |
|------|------|------|
| 硬件缓冲 | 100-500ms，时钟稳定 | 无缓冲，逐帧计算 |
| 灵活性 | 无法丢帧（会产生爆音） | 可灵活丢帧（1-2 帧难以察觉） |
| 用户感知 | 对卡顿敏感（立即察觉） | 对卡顿容忍度高 |

---

#### 4. Seek（拖拽进度条）实现

**流程**：
```
用户拖拽进度条 / 按方向键
  ↓
PlayerController::seek(target)
  ├─ sync_.setAudioClock(target)   // 立即更新音频时钟，VideoRenderer 开始丢弃旧帧
  ├─ renderer_->flushPendingFrame() // 清除暂存旧帧
  ├─ demux_.seek(target)           // 原子写入 seekTarget_
  ├─ videoPacketQ_.clear()         // 唤醒阻塞 DemuxThread
  ├─ audioPacketQ_.clear()
  ├─ videoDec_.flush()             // 设 flush_ 标志，VideoDecodeThread 异步处理
  └─ audioDec_.flush(target)       // 设 flush_ + minAcceptablePts_ 过滤旧帧时钟
  ↓
DemuxThread::handleSeek()
  ├─ av_seek_frame(BACKWARD)       // 定位到 target 之前的关键帧 K
  ├─ 从 K 起推送所有视频包（H.264 参考帧依赖，不能丢弃 K→target 段）
  └─ 丢弃 target 前的音频包（防止播放旧音频片段）
  ↓
VideoDecodeThread
  ├─ 收到关键帧 K，从 K 正常解码
  └─ 推送帧到 videoFrameQ_
  ↓
VideoRenderer
  ├─ 丢弃 PTS < target - 0.4s 的旧帧（diff < -0.4）
  └─ 渲染 PTS ≈ target 的首帧
```

**关键设计：视频包必须从关键帧起全部放行**

早期实现曾将关键帧到 target 之间的视频包也丢弃（只推送 PTS ≥ target 的包），以为可以减少 VideoDecodeThread 的无效解码工作。但实测发现这会导致视频冻结 2–5 秒：

- `avcodec_flush_buffers` 后，H.264 解码器需要 IDR 帧（关键帧）作为解码起点
- 若第一个收到的包是 P/B 帧（关键帧已被丢弃），解码器持续返回 `EAGAIN`
- 直到遇到下一个 IDR 帧才能输出，GOP 间隔最长可达 8–10 秒
- 修复后让视频包从关键帧 K 起全部流入，VideoRenderer 凭 `diff < -0.4s` 快速丢弃旧帧（< 500 ms 恢复）

---

#### 5. 解码线程：VideoDecodeThread & AudioDecodeThread

**问题**：如何高效解码音视频包，并让音频输出驱动播放时钟？

**核心设计**：

```cpp
// VideoDecodeThread: 消费视频包队列，解码为 AVFrame* 推入帧队列
class VideoDecodeThread : public QThread {
    bool init(AVCodecParameters* params);  // 打开解码器
    void flush();   // seek 后清空解码器缓冲
    void run();     // 取包 → send_packet → receive_frame → clone → 推入输出队列
};

// AudioDecodeThread: 解码 + 重采样 + QAudioOutput + 更新音频时钟
class AudioDecodeThread : public QThread {
    bool init(AVCodecParameters*, AVRational, AVSync*);  // 打开解码器 + SwrContext + QAudioOutput
    void flush();   // seek 后清空解码器 + swr 缓冲
    void setVolume(float v);  // 原子量暂存，run() 中应用
    void run();     // 取包 → 解码 → swr_convert → QIODevice::write(PCM) → 更新 sync_
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **线程模型** | 视频/音频各一个独立 QThread | 视频解码是计算密集型，分开线程避免互相阻塞，充分利用多核 |
| **视频解码输出** | `AVFrame*` 队列供 VideoRenderer 消费 | VideoRenderer 持有帧队列，1ms 定时检测渲染时机，避免耦合 |
| **音频时钟驱动** | 每帧解码后更新 AVSync | 帧级别粒度 ~23ms，足够精确；无需样本级更新 |
| **重采样目标** | S16 Stereo 44100 | 通用 PCM 格式，QAudioOutput 原生支持，兼容绝大多数声卡 |
| **音量控制** | `atomic<float>` 暂存，run() 循环 apply | 避免跨线程直接调用 QAudioOutput::setVolume |
| **测试策略** | 仅编译验证，无独立单元测试 | 依赖真实 FFmpeg 解码，单独构造断言困难；集成测试在 PlayerController 中覆盖 |

**坑点记录（FFmpeg 7.x API 变更）**：

| 旧 API（FFmpeg 4.x） | 新 API（FFmpeg 7.x） | 原因 |
|----------------------|----------------------|------|
| `codecCtx_->channel_layout` | `codecCtx_->ch_layout`（`AVChannelLayout` 类型） | FFmpeg 5+ 改用复合类型支持更多声道布局 |
| `av_opt_set_int(swr, "in_channel_layout", ...)` | `av_opt_set_chlayout(swr, "in_chlayout", &ch_layout, 0)` | opt 接口同步更新 |
| `QAudioSink` | `QAudioOutput` | Qt 5 中叫 QAudioOutput，Qt 6 改名 QAudioSink |
| `QAudioFormat::setSampleFormat(Int16)` | `setSampleSize(16) + setSampleType(SignedInt) + setByteOrder(LittleEndian) + setCodec("audio/pcm")` | Qt 5 QAudioFormat API 不同 |

**验证**：编译通过 0 error；集成测试在 Phase 6（PlayerController）中覆盖。

---

#### 6. 渲染线程：VideoRenderer

**问题**：如何在 GUI 线程中以音频时钟为基准定时渲染视频帧，同时不阻塞 UI？

**核心设计**：

```cpp
class VideoRenderer : public QWidget {
    void init(int w, int h, AVRational timeBase, AVSync*, FrameQueue<AVFrame*>*);
    void startRendering();   // 启动 1ms QTimer
    void stopRendering();    // 停止定时器
protected:
    void paintEvent(QPaintEvent*) override;  // QPainter 绘制 currentFrame_
private slots:
    void onTimer();          // 拉帧 → 同步决策 → sws_scale → update()
private:
    QImage currentFrame_;    // sws_scale 直接写入其像素缓冲区
    QMutex frameMutex_;      // 保护 onTimer() 写与 paintEvent() 读的竞争
    SwsContext* swsCtx_;     // YUV420P → RGB32
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **驱动方式** | 1ms `QTimer` 而非独立 QThread | Qt 规定只有 GUI 线程才能绘制 Widget；QTimer 跑在 GUI 线程，无需跨线程调用 `update()` |
| **帧拉取** | `tryPop(timeout=0)` 非阻塞 | `onTimer` 在 GUI 主线程，阻塞会冻结整个 UI（进度条、鼠标响应全失效） |
| **领先 > 2s 时** | 把帧放回队列而非丢弃 | seek 后或缓冲积压时音频还未追上，此时丢帧会造成画面永久缺失；放回队列等音频时钟追上后自然进入渲染路径 |
| **轻微领先时** | `msleep(delay)` 等待 | 视频帧 pts 超前音频时钟属正常情况（解码比播放快）；等待让视频跟上音频 |
| **落后时** | 跳过 sleep 立即渲染 | `delay < 0` 意味着视频已落后，不等待相当于软性丢帧补偿，追赶音频 |
| **像素写入方式** | `currentFrame_.bits()` 传给 `sws_scale` | `bits()` 返回 `QImage` 内部像素缓冲区裸指针，`sws_scale` 原地写入，无额外内存拷贝 |
| **init 与构造函数分离** | 单独提供 `init()` | 视频分辨率和时间基须打开文件后才能从 `AVCodecContext` 读取，构造时尚未知晓 |
| **测试策略** | 编译验证 + 集成测试（Task 8 后） | 渲染正确性依赖完整流水线（帧队列有数据 + 音频时钟运行），无法独立单测 |

**`onTimer` 同步决策流程**：

```
tryPop(frame, 0)
  │ 队列空 → return（等下一个 tick）
  ↓
计算 pts = frame->pts * av_q2d(timeBase_)
计算 delay = sync_->videoDelay(pts)
  │
  ├─ delay > 2.0s  → push(frame) 放回，return（音频未追上）
  ├─ delay > 0     → msleep(delay)，然后渲染（视频轻微超前，等待）
  └─ delay ≤ 0     → 直接渲染（视频落后，立即追赶）
  ↓
sws_scale(YUV420P → RGB32) 写入 currentFrame_.bits()
av_frame_free(frame)
update()  →  触发 paintEvent()
```

**验证**：编译通过 0 error；逻辑集成测试待 Task 8（PlayerController）串联完整流水线后执行。

---

#### 7. 总控制器：PlayerController

**问题**：谁来持有所有组件，协调它们的启动、停止、seek 和生命周期？

**核心设计**：

```cpp
class PlayerController : public QObject {
    bool open(const QString& path);  // 打开文件，初始化所有组件
    void play();                     // 启动三个线程 + 渲染定时器
    void pause();                    // 停止渲染（线程继续）
    void stop();                     // 停止并等待所有线程退出
    void seek(double seconds);       // seek + flush 解码缓冲
    void setVolume(float v);
signals:
    void durationChanged(int64_t ms);
    void positionChanged(int64_t ms);   // 100ms QTimer 轮询
    void playbackFinished();
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **组件所有权** | PlayerController 按值持有线程，按指针持有 VideoRenderer | 线程生命周期与 Controller 绑定；VideoRenderer 是 QWidget 由 UI 层创建，不能在 QObject 树外析构 |
| **open() 先 stop()** | 每次 open 先调用 stop() + reset() 队列 | 支持重复打开文件，保证状态干净，不会出现旧包混入新队列 |
| **stopAllThreads 顺序** | 先停 DemuxThread，再停解码线程 | 解复用是生产方，先停生产防止继续推包；解码线程阻塞在 tryPop，abort 队列后自然退出 |
| **pause 简化实现** | 仅停渲染定时器，线程不暂停 | 生产级暂停需在各线程加原子标志，此阶段简化为 stop/seek/play 组合实现继续播放 |
| **onDemuxFinished 延迟 500ms** | `QTimer::singleShot(500, ...)` 后发 playbackFinished | 解复用结束时队列中可能还有未消费的包，延迟给解码线程留时间耗尽，避免音视频截断 |
| **positionChanged 驱动方式** | 100ms QTimer 轮询 `sync_.audioClock()` | 音频时钟是连续更新的原子量，定时轮询比信号回调简单且足够精确（UI 刷新不需要毫秒级精度） |
| **QTimer 前向声明** | 头文件用 `class QTimer;`，源文件 include | 避免头文件隐式传递 Qt include，减少编译依赖 |

**验证**：编译通过 0 error；集成测试（实际播放 sample.mp4）在 Task 9 MainWindow 完成后进行端对端验证。

#### Red 阶段
```cpp
// tests/tst_demuxthread.cpp - 3 个测试用例
TEST open_validFile_returnsTrue()       // 打开合法 mp4
TEST open_invalidFile_returnsFalse()    // 路径不存在
TEST run_300ms_bothQueuesHavePackets()  // 两条队列均有包
```
编译失败确认（DemuxThread 未实现）。

#### Green 阶段
实现 `src/demuxthread.h/.cpp`，全部测试通过；消费端 `av_packet_free` 验证无泄漏。

#### Refactor 阶段
将 seek 逻辑收拢到 `run()` 内原子检测，stop 级联 abort 两条队列，保持测试全绿。

---

### TDD 工作流案例：Phase 2（FrameQueue 实现）

#### Red 阶段
```cpp
// tests/tst_framequeue.cpp - 5 个测试用例
TEST pushPop_singleThread()        // 基本读写
TEST tryPop_returnsFlase_whenEmpty // 空队列超时
TEST blocksAt_maxSize              // 容量满阻塞
TEST abort_unblocks_pop            // 关闭唤醒
TEST size_and_clear                // 工具方法
```
编译失败确认（FrameQueue 未实现）。

#### Green 阶段
实现 `src/framequeue.h`，所有测试通过：`7 passed, 0 failed, 0 skipped, 252ms`

#### Refactor 阶段
检查代码清晰性、边界条件，保持测试全绿。

---

### 非 TDD 场景：编译验证模式（Phase 4 解码线程）

不是所有模块都适合单元测试。解码线程依赖真实 FFmpeg 解码器，输入是压缩包，输出是解码帧，单独的断言既难构造又难验证。

**替代方案**：
1. **头文件先写** — 设计接口签名，确保与上下游（DemuxThread 的包队列、PlayerController 的串联）一致
2. **源文件实现** — 完成解码循环、flush、stop 等逻辑
3. **编译验证** — `cmake --build` 0 error 即通过
4. **集成测试留到 Task 8** — PlayerController 串起整条流水线后统一验证音画同步

**为什么这样做重要**：
- 不写代码就写测试，确保测试不是自动通过的废纸
- 看到测试失败，证明它真的在验证功能
- 最小实现避免 over-engineering
- 编译验证模式适用于"依赖真实环境、难以 mock"的模块（解码、渲染、硬件加速）

---

### 项目文档结构

```
docs/
├── DEVPLAN.md                          # 10 个 Phase 的高层计划
├── superpowers/
│   ├── specs/
│   │   ├── 2026-05-08-framequeue-design.md    # API 表、设计决策、测试矩阵
│   │   └── 2026-05-08-demuxthread-design.md   # stop/seek/内存/测试策略
│   └── plans/
│       ├── 2026-04-26-rambos-player-core.md   # Phase 1–6 的 TDD 分步指南
│       └── 2026-05-08-demuxthread.md          # Task 4 专项计划（av_read_frame 循环）
```

每个 spec 冻结需求，每个 plan 给出具体代码和命令。

---

### 面试常见问题

**Q1: 为什么 FrameQueue 而不用 std::queue？**  
A: 有界容量 + 阻塞语义 + Qt 集成。std::queue 不提供这些。

**Q2: 为什么音频是主时钟？**  
A: 音频硬件缓冲稳定，视频帧可无损丢弃，用户对音频卡顿感知更强。

**Q3: abort() 为什么用 wakeAll() 而不是 wakeOne()？**  
A: 多线程同时阻塞在 push/pop，wakeOne() 只唤醒一个，其余线程会永久阻塞。

**Q4: TDD 中如果测试写得太复杂怎么办？**  
A: 说明设计有问题。应简化 API，而非简化测试。复杂的测试是重新设计的信号。

**Q5: DemuxThread 的 seek 为什么用原子量而不是加锁？**  
A: seek 请求只需传递一个 double，原子量足够。加锁会让 run() 循环在每次 seek 检测时阻塞，引入不必要的延迟；原子量读写是无等待操作。

**Q6: DemuxThread push 包时为什么要 av_packet_clone 而不是直接 push？**  
A: `av_read_frame` 返回的包由 FFmpeg 内部管理，下一次调用前必须 `av_packet_unref`。clone 出独立引用计数的副本后，队列里的包生命周期由消费方（decode 线程）控制，双方无竞争。

**Q7: stop() 为什么要 abort 两条队列，只 abort DemuxThread 自己不够吗？**  
A: DemuxThread 停止后，解码线程仍阻塞在 `tryPop` 等待新包。不 abort 包队列，解码线程会永久挂起，无法正常退出。

**Q8: 为什么视频解码线程和音频解码线程要分开？**  
A: 视频解码是计算密集型（尤其是 1080p 以上），音频解码相对轻量。分开后：① 视频解码不会阻塞音频输出，保证音频连续性；② 可单独为视频线程绑定不同优先级；③ 后续 Phase 7 加硬件加速时，只需改 VideoDecodeThread，AudioDecodeThread 不受影响。

**Q9: AudioDecodeThread 为什么不用单元测试？**  
A: 音频解码依赖真实 FFmpeg 解码器，输入是对的压缩包，输出是 PCM 流。要验证 PCM 正确性需要实际听音或频域分析，单元测试的断言难以表达"这段 PCM 听起来是对的"。改用编译验证 + 集成测试（PlayerController 全链路播放后耳听确认）。

**Q10: Seek 时为什么要同时 flush 解码器？**  
A: `av_seek_frame` 只是移动了文件读取指针，解码器内部还有引用帧的残留状态（如 H.264 的 DPB 缓冲区）。不清空的话，新位置的包喂给旧状态的解码器会产生花屏。`avcodec_flush_buffers` 的作用就是重置解码器到干净状态。

**Q11: FFmpeg 7.x 相比 4.x 有哪些破坏性变更影响了你们？**  
A: 主要有两个：① `AVCodecContext::channel_layout` 从 `uint64_t` 改为 `AVChannelLayout` 复合类型，相关 `av_opt_set_int` 改为 `av_opt_set_chlayout`，opt key 也从 `"in_channel_layout"` 改为 `"in_chlayout"`；② Qt 5 的 `QAudioOutput` 在 Qt 6 中改名 `QAudioSink`。这些在翻文档时很容易踩坑，编译报错后查 FFmpeg changelog 才确认。

**Q12: VideoRenderer 为什么用 QTimer 而不是单独开一个线程做渲染？**  
A: Qt 规定 Widget 的绘制必须发生在 GUI 主线程。如果在子线程调用 `update()` 或直接操作 Widget，轻则画面异常，重则崩溃。QTimer 运行在 GUI 线程，`onTimer()` 槽自然在主线程执行，`update()` 触发 `paintEvent()` 也在同一线程，完全符合 Qt 的线程模型，不需要额外同步。

**Q13: onTimer 为什么用非阻塞的 tryPop 而不是带超时的阻塞等待？**  
A: `onTimer` 运行在 GUI 主线程。如果帧队列空时阻塞等待，整个主线程会卡住，进度条拖拽、按钮点击、窗口响应全部失效，用户体验崩溃。用 `tryPop(timeout=0)`：队列空时立即返回，等下一个 1ms tick 再试，主线程始终保持响应。

**Q14: delay > 2s 时为什么把帧放回队列而不是直接丢弃？**  
A: 视频帧 pts 大幅超前音频时钟，通常发生在 seek 之后或音频缓冲积压阶段，此时音频时钟还没追上，并不是视频真的"多余"了。如果丢帧，这帧画面就永久缺失，视频会跳帧。放回队列后，等音频时钟追上来，delay 缩短到正常范围，帧自然进入渲染路径，画面连续。

**Q15: currentFrame_ 的像素数据是怎么更新的，为什么看不到明显的赋值？**  
A: `QImage::bits()` 返回该对象内部像素缓冲区的裸指针。`sws_scale` 把 YUV420P 转换后的 RGB32 数据直接写入这块内存，`currentFrame_` 对象本身没有重新赋值，但它持有的像素数据已被覆盖。`init()` 只分配一次缓冲区，每帧都原地覆写，避免重复分配。

**Q16: frameMutex_ 保护的是什么竞争？**  
A: `onTimer()` 在 GUI 线程写 `currentFrame_`（`sws_scale` 写入像素缓冲），`paintEvent()` 也在 GUI 线程读 `currentFrame_`（`QPainter::drawImage`）。两者都在主线程，正常情况下不会并发，但 `update()` 触发的 `paintEvent()` 可能在 `sws_scale` 写完之前被调度（Qt 的事件循环是协作式的）。mutex 确保写完整帧后再读，防止撕裂。

**Q17: PlayerController 为什么按值持有线程，却按指针持有 VideoRenderer？**  
A: DemuxThread / VideoDecodeThread / AudioDecodeThread 的生命周期完全由 PlayerController 管理，析构时随之销毁，按值持有最简洁。VideoRenderer 继承 QWidget，是 UI 组件，由 MainWindow 创建并放入 Qt 的 Widget 树中，由 Qt 负责析构；PlayerController 只是借用它，按指针持有明确表达"不拥有所有权"的语义。

**Q18: stopAllThreads 为什么要先停 DemuxThread，再停解码线程？**  
A: DemuxThread 是生产方，持续往包队列推包。如果先停解码线程（消费方），DemuxThread 继续推包，包队列满后 DemuxThread 阻塞在 push()，永远等不到消费者，导致 wait() 超时。先停 DemuxThread，包队列不再有新包，再 abort 包队列唤醒阻塞在 tryPop 的解码线程，线程自然退出。

**Q19: open() 为什么每次都先调用 stop()？**  
A: 支持重复打开文件（播放中切换视频）。不先 stop，旧线程还在运行、旧包还在队列，新文件打开后两套数据混在一起必然花屏或崩溃。先 stop 保证所有线程退出、队列清空，再 reset 队列恢复可用状态，然后用新文件参数初始化，状态完全干净。

**Q20: onDemuxFinished 为什么延迟 500ms 才发 playbackFinished？**  
A: DemuxThread 的 finished 信号在解复用循环结束时发出，但此时包队列里可能还有几十个未消费的包。如果立即发 playbackFinished，UI 会停掉播放，而解码线程还有数据没播完，最后几秒画面和声音会被截断。延迟 500ms 给解码线程留时间耗尽剩余包，是一种简化的"排水"策略。

**Q21: 为什么滤镜参数传递用 atomic + dirtyFlag 而不是直接调 FilterGraph 方法？**  
A: FilterGraph 跑在 VideoDecodeThread（解码线程），而 FilterPanel 的滑块在 GUI 线程。如果 GUI 线程直接调 FilterGraph::rebuild()，等于让 GUI 线程去碰解码线程正在使用的 AVFilterGraph 对象——在 process() 和 rebuild() 之间会产生竞态，可能导致崩溃。atomic + dirtyFlag 模式让 rebuild 始终在解码线程自己的上下文里执行，GUI 线程只负责"下订单"。

**Q22: 为什么用 hue + colorbalance 而不是 eq 滤镜？**  
A: FFmpeg 以独立模块编译滤镜，每个滤镜是一个 .so/.dll。vcpkg 构建环境没有编译 eq 滤镜模块。hue（调整亮度/饱和度）+ colorbalance（调整 RGB 通道对比度）在本构建中可用，功能上完全覆盖 eq 的场景，且参数更精细。

**Q23: 滤镜重建（rebuild）的性能开销有多大？能不能改成运行时调参？**  
A: FFmpeg libavfilter 不支持对已配置的滤镜链修改参数，必须 close → init 重建。重建一次约 1–5ms（取决于滤镜链复杂度），仅在用户拖滑块时触发，人眼的视觉延迟远大于这个开销。如果未来需要高性能实时调参（如 60fps 下逐帧改参数），方案是使用 GPU shader（OpenGL/DirectX）替代 FFmpeg 滤镜。

**Q24: 水印为什么用 movie + overlay 而不是 drawtext？**  
A: drawtext 只能渲染文字，不支持图片。movie 滤镜可以加载任意图片作为第二视频流，overlay 将两个流合成。但 drawtext 在本构建中同样不可用（编译时未启用 libfreetype）。

**Q28b: 推流为什么选择在 DemuxThread 层分叉，而不是在解码层分叉解码帧？**
A: 直通模式（-c copy）不需要解码再编码，DemuxThread 读出的压缩包直接 tryPush 给 MuxThread 封装写出，全程无解码无重编码。CPU 占用极低，画质也是原始文件的质量。如果未来需要转码推流（指定码率/分辨率），才需要在解码层分叉 AVFrame 进行重编码。

**Q28c: seek 时 MuxThread 如何保证推流时间戳不回跳？**
A: 每段记录 `videoSegBase_`（本段首包 DTS）和 `videoAccumPts_`（上段末尾的输出 DTS）。每个输出包的时间戳 = 原始 DTS - segBase + accumPts，保证跨 seek 的输出 DTS 始终单调递增。seek 时 DemuxThread 向 restream 队列推一个 `nullptr` sentinel，MuxThread 收到后 `accumPts = lastOut`，新段从上段末尾续接。

**Q28d: 推流中关闭窗口为什么会崩溃，怎么修复？**
A: `~MainWindow()` 原来先调 `streamCtrl_->stop()`，这一步会销毁 `videoMuxQueues_` 里的 `FrameQueue` 对象。但此时 DemuxThread 仍持有指向这些已销毁队列的裸指针，随后 `delete player_` 触发 `DemuxThread::stop()` → `clearRestreamQueues()` → `q->abort()`，访问已释放的 mutex，UAF 崩溃。修复是在 `streamCtrl_->stop()` 之前先调 `player_->clearRestreamPacketQueues()`，让 DemuxThread 在队列对象仍存活时解除引用。

**Q25: Seek 时为什么不能只推送 PTS ≥ target 的视频包，跳过关键帧到目标之间的包？**  
A: H.264 使用帧间预测，P/B 帧依赖前面的参考帧解码。`avcodec_flush_buffers` 重置解码器后，若第一个收到的包是 P/B 帧（关键帧已被跳过），解码器内部找不到参考帧，持续返回 `EAGAIN`，直到收到下一个 IDR 帧才能恢复输出。实测 GOP 间隔 2–8 秒的视频会冻结对应时长。正确做法是从 `av_seek_frame` 跳到的关键帧 K 起推送全部视频包，让解码器从 K 正常解码；VideoRenderer 凭 `diff < -0.4s` 丢弃 K→target 的旧帧，开销远小于等待下一个 IDR。

**Q26: Seek 时音频包为什么可以丢弃关键帧到目标之间的部分，视频却不行？**  
A: AAC 等音频编码器虽然也有帧间依赖（滤波器组重叠），但 FFmpeg 的音频解码器在 flush 后能从单帧直接输出 PCM，不会卡住。更重要的是：丢弃旧音频包是为了防止用户听到 seek 目标前的音频片段，这是用户体验需求。视频旧帧则由 VideoRenderer 静默丢弃（不显示），只损失少量 CPU 解码时间，不影响体验。

**Q27: Qt 的 eventFilter 返回 true 消费了鼠标事件，为什么控件还能获得焦点？**  
A: Qt 的焦点分配在 Windows 平台部分由 OS 原生消息（WM_SETFOCUS/WM_LBUTTONDOWN）驱动，发生在应用层事件过滤之前，或与之并行。即使 eventFilter 返回 true 阻止了 QMouseEvent 到达控件，OS 可能已经把焦点交给该控件。可靠的做法是对不需要接受焦点的控件设置 `Qt::NoFocus` 策略，从根本上告诉系统该控件不参与焦点调度，点击不改变焦点所有权。

**Q28: VideoRenderer 的 seek 诊断日志是如何设计的，为什么不直接用 qCDebug？**  
A: `qCDebug` 配合 `Q_LOGGING_CATEGORY(lcVideo, "rambos.video", QtWarningMsg)` 时，QtWarningMsg 是该 category 的最低级别，比 QtDebugMsg 高，导致所有 `qCDebug` 调用被静默过滤，日志文件中看不到任何 VideoRenderer 输出。为了在生产日志中保留关键诊断点（seek 后首帧、长时间无帧、旧帧连续丢弃），改为直接用 `qInfo()` 并配合速率限制：无帧超过 500 ms 才打印一次，丢帧在第 1、30 帧及之后每 60 帧打印一次，避免 30 fps 下日志爆炸。


#### 8. 视频滤镜：FilterGraph + 实时调参（Phase 8）

**问题**：如何在不阻塞解码线程的前提下，让 UI 实时调节视频滤镜参数（亮度/对比度/饱和度/水印）？

**核心设计**：

```cpp
// FilterGraph: 封装 libavfilter 滤镜链
class FilterGraph {
    bool init(int w, int h, AVPixelFormat fmt, AVRational tb, const QString& desc);
    int  process(AVFrame* in, AVFrame* out);  // 0=成功，out 调用方预分配
    bool rebuild(const QString& newDesc);     // close() + init()，在线重建
    void close();
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **滤镜插入点** | VideoDecodeThread::run() 解码后、push 前 | 解码线程是帧的生产者，就地处理避免跨线程搬运帧；GUI 线程不接触 AVFrame |
| **跨线程参数传递** | atomic + dirtyFlag（同 seek/flush 模式） | FilterPanel 写 atomic + 设 dirtyFlag，解码线程下个循环检测并 rebuild，无需加锁 |
| **FilterGraph 在线重建** | close() + init() 整链重建 | FFmpeg 不支持运行时修改滤镜参数，只能销毁重建；仅在用户拖滑块时触发，开销忽略 |
| **直通模式（passthrough）** | desc 为空时 process 直接 av_frame_clone | 滤镜未启用时无性能损耗 |
| **可用滤镜选型** | hue + colorbalance 替代 eq | 本构建环境缺 eq，hue（亮度/饱和度）+ colorbalance（对比度）可替代 |
| **水印实现** | movie + overlay 滤镜组合 | FFmpeg 原生支持，无需手动混合像素 |

**TDD 验证**：tst_filtergraph.cpp 8/8 全部通过（passthrough, hflipFilter, invalidFilter, rebuildAfterFailure, rebuildToPassthrough, probeAvailableFilters）。

---

#### 9. 主窗口：MainWindow

**问题**：如何组织播放器 UI，让进度条拖拽、音量调节、全屏切换协同工作？

**核心设计**：

```cpp
class MainWindow : public QMainWindow {
    void openFile(const QString& path);       // 打开文件 + 更新最近记录
    void onPlayPause();                       // 播放/暂停切换
    void onSeekSliderMoved(int value);        // 进度条拖拽 → seek
    void onVolumeChanged(int value);          // 音量滑块 → setVolume
    void onDurationChanged(int64_t ms);       // 更新时长标签
    void onPositionChanged(int64_t ms);       // 100ms 更新进度条位置
    void onPlaybackFinished();                // 播放结束复位按钮
    bool eventFilter(QObject*, QEvent*);      // 拦截滑块点击直接跳转
};
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **控件布局** | 全部在 `.ui` 文件中用 Qt Designer 绘制 | 布局可视化可调整，`.cpp` 只负责信号连接和逻辑，不动态创建控件 |
| **VideoRenderer 集成** | 通过 Qt Designer promoted widget 放入 | 无需 `setCentralWidget` 代码，布局在 `.ui` 中完全可见 |
| **进度条点击跳转** | `eventFilter` 拦截 `MouseButtonPress`，用 `QStyle::sliderValueFromPosition` 计算精确位置 | Qt 默认点击是 pageStep 步进，不符合视频播放器"点哪里跳哪里"的直觉 |
| **拖拽时跳过进度更新** | `isSliderDown()` 检测，拖拽中忽略 `positionChanged` | 否则进度条会在用户的拖拽和 100ms 定时更新之间来回跳动 |
| **析构顺序** | 先 `delete player_`（触发 stop），再 `delete ui` | `PlayerController::stop()` 通过 `renderer_->stopRendering()` 停止 QTimer；若先 `delete ui` 则 renderer_ 已销毁，再次访问会 UAF 崩溃 |
| **最近文件列表** | `QSettings` 持久化，动态重建 `QMenu` 条目 | 分隔线前删除旧条目再按序插入，支持编号快捷键 `&1`–`&9` |
| **全屏切换** | 双击 `mouseDoubleClickEvent` → `showFullScreen()` / `showNormal()` | Qt 原生全屏，无边框无任务栏，符合视频播放器预期 |

**端对端验证结果（8/8 通过）**：

| 场景 | 结果 |
|------|:--:|
| 打开 1080p H.264 mp4，正常播放，音画同步 | ✅ |
| 拖进度条到中间，seek 后继续播放 | ✅ |
| 音量设为 0，静音 | ✅ |
| 播放到末尾，按钮变 ▶ | ✅ |
| 双击全屏 → 双击退出，正常切换 | ✅ |
| 播放中关闭窗口，无崩溃 | ✅ |
| 大跨度 seek（跳转 1000 s+），视频 < 500 ms 恢复（H.264 参考帧修复） | ✅ |
| 点击进度条后立即按方向键，快进/快退正常（Qt::NoFocus 修复） | ✅ |

**验证**：全部端对端场景通过；单元测试 0 failures（FrameQueue 7 passed, AVSync 7 passed, DemuxThread 3 passed）。

---

#### 10. 推流播放内容：解复用层直通分叉 + 多目标 MuxThread（Phase 9 重构）

**问题**：如何将正在播放的视频内容实时推流到直播平台或本地文件，且不影响播放器本身的性能？

**核心设计**：`-c copy` 直通模式。DemuxThread 读出压缩包（H.264 + AAC）后，在推入播放队列的同时 `tryPush` 到 restream 队列，MuxThread 直接封装写出，全程无解码无重编码。

```
DemuxThread → videoPacketQueue → VideoDecodeThread → VideoRenderer（播放）
            → audioPacketQueue → AudioDecodeThread → QAudioOutput（播放）
            ↘ restreamVideoQ  → MuxThread[N] → RTMP / FLV
            ↘ restreamAudioQ  → MuxThread[N]
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **推流层级** | DemuxThread 层分叉压缩包 | 直通无需解码再编码，CPU 占用极低；源文件 H.264/AAC 原样转发，画质无损 |
| **分叉方式** | `tryPush()`（非阻塞） | 推流慢时丢帧而非阻塞 DemuxThread，播放管线永远不受推流影响 |
| **协议识别** | URL 前缀自动判断：rtmp:// → FLV，srt:// → MPEGTS，本地路径 → FLV | 对外接口统一为 URL 字符串，MuxThread 内部透明处理 |
| **seek 后 PTS 续接** | 每段记录 segBase 和 accumPts，seek sentinel 到来时 accumPts = lastOut，新段从 accumPts 续接 | seek 会造成源包 PTS 回跳；不处理则接收端解码器报错或画面跳变 |
| **B 帧 DTS 监控** | 只检查 DTS 单调性，不检查 PTS | FLV 按 DTS 传输，B 帧 PTS 天然非单调；检查 PTS 会误报，检查 DTS 才能发现真实问题 |
| **MuxThread 写帧** | `av_write_frame` 而非 `av_interleaved_write_frame` | FLV/RTMP 音视频独立传输，interleaved 的内部缓冲区遇到音频迟到时报 EINVAL |
| **UI 设计** | 可勾选的推流目标（RTMP / 本地录制） | 用户按需组合，不强制全选 |
| **关窗安全停止** | `~MainWindow` 先调 `clearRestreamPacketQueues()` 再 `streamCtrl_->stop()` | 必须先让 DemuxThread 解除对 FrameQueue 的引用，再销毁队列对象，否则 UAF 崩溃 |

**端对端验证结果**：

| 场景 | 结果 |
|------|:--:|
| RTMP 推本地 SRS，ffplay 拉流正常播放 | ✅ |
| seek 后 ffplay 画面自动恢复，无需手动操作 | ✅ |
| ffprobe 验证本地 FLV 文件 DTS 单调递增 | ✅ |
| 推流中直接关闭窗口，无 UAF 崩溃 | ✅ |

---

#### 11. 视频剪辑器：ThumbnailExtractor + Timeline + ExportWorker（Phase 10）

**问题**：如何实现带时间轴预览的无损视频剪辑，起点精确、导出流畅、PTS 归零？

**核心设计**：

```
MainWindow "剪辑模式 (Ctrl+T)"
    │  currentFile_
    ▼
ThumbnailExtractor (QThread)         Timeline (QWidget)
  av_seek_frame 逐点解码      ──→    缩略图轨道 + 把手拖拽
  逐张 thumbnailReady               trimPointChanged(in, out)
    │                                    │
    └──── 缩略图逐张即时刷新 ─────────────┘
                                         │
                                    Ctrl+E 导出
                                         │
                                         ▼
                                   ExportWorker (QThread)
                                     -c copy + 关键帧对齐 + PTS 归零
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **缩略图提取方式** | 独立 QThread，av_seek_frame 逐点定位解码 | 不依赖播放进度，暂停状态也能提取；独立线程避免阻塞 UI |
| **缩略图送达** | 逐张 thumbnailReady 信号，非批量 | 95 分钟电影也能几秒内看到第一张图，用户不用等全部完成 |
| **探测优化** | max_analyze_duration=3s, probesize=5MB | 默认 avformat_find_stream_info 对大文件探测极慢，限制后可秒开 |
| **时间轴控件** | 纯 QWidget + QPainter 自绘 | 无需第三方库；刻度尺自适应间距、缩略图均匀分布、把手颜色区分（绿入红出） |
| **无损剪切** | -c copy 模式 | 不重编码，速度只受磁盘 I/O 限制，画质无损 |
| **关键帧对齐** | 等待所有视频流首关键帧就绪后才开始写 | 避免 P/B 帧缺少参照帧导致前 3 秒卡顿或花屏 |
| **PTS 归零** | 以最早关键帧 PTS 为偏移量统一扣除 | 避免多帧被挤压到 PTS=0 造成卡顿；输出从 00:00 开始 |
| **多流处理** | 视频/音频各自跟踪出口 | 有视频无音频或有音频无视频的文件都能正确处理 |

**踩坑记录**：

1. **PTS 偏移用 inPts_ 而非实际关键帧 PTS**：inPts_ 之后第一个关键帧之前的所有帧 PTS 被扣成负数 → clamp 到 0 → 多帧堆在 0 位，开头卡死。修改为以最早关键帧实际 PTS 为偏移。

2. **触发就绪的关键帧被丢弃**：keyframeSeen 检查通过后直接 continue，导致关键帧本身未写入，后续 P 帧依赖缺失 → 前几秒花屏。修复：关键帧触发就绪后不跳转，继续执行写入逻辑。

3. **信号名 finished 与 QThread::finished() 冲突**：导致二次导出按钮失效。更名为 exportFinished。

4. **缩略图 PTS 比较不可靠**：seek 后 frame->pts 可能为 AV_NOPTS_VALUE，PTS 比较永远失败 → 零张缩略图。移除 PTS 比较，seek 后解码到的第一帧直接用。

**端对端验证**：拖拽剪辑点 → 导出，画面流畅无卡顿，时长与选区一致。

---

#### 12. ARM64 (RK3588) 交叉编译适配（Phase 13）

**问题**：同一份 MSVC（Windows）下编译通过的代码，如何在不破坏 Windows 构建的前提下，让 WSL 内 `aarch64-linux-gnu-g++` 交叉编译也通过？

**核心设计**：

```
CMakePresets.json
  ├─ default（VS2017 x64 + vcpkg）  ── Windows 原生
  └─ arm64（Unix Makefiles）         ── WSL 交叉编译
        │
        ▼
  cmake/toolchain-aarch64-linux-gnu.cmake
        │ CMAKE_FIND_ROOT_PATH_MODE_* = ONLY
        ▼
  cmake/FindFFMPEG.cmake（FFMPEG_ROOT 直接 find_path/find_library，绕开 pkg-config prefix 不匹配）
        │
        ▼
  build-arm64/RambosPlayer（aarch64 ELF）
        │ deploy-arm64.sh（scp + run.sh）
        ▼
  Firefly ROC-RK3588S-PC（LD_LIBRARY_PATH=~/ffmpeg_rkmpp/lib）
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **共享同一套源码** | 用 `CMAKE_CROSSCOMPILING` / `Q_OS_WIN` 宏隔离平台差异，不拆分代码分支 | 两个平台始终编译同一套源码，避免维护两份逻辑产生分叉 |
| **FFmpeg 查找方式** | 自定义 `FindFFMPEG.cmake` 直接 `find_path`/`find_library`，不用 pkg-config | 板卡 rootfs 拷贝下 `.pc` 文件里的 prefix 指向板卡路径（`/home/firefly/...`），本机不存在，pkg-config 必然失败 |
| **VSCode 任务跨平台** | `tasks.json` 用 `windows`/`linux` 字段覆盖 `command`，而非拆两份 tasks.json | 同一个 `.vscode/tasks.json` 被 git 共享给 Windows 原生窗口和 WSL 窗口，OS 覆盖是 VSCode 原生支持的写法 |
| **F7 编译快捷键** | 全局 `keybindings.json` 绑定 `F7 → workbench.action.tasks.build`，而非改 tasks 默认触发方式 | VSCode 默认无 F7 绑定（Ctrl+Shift+B 才是），照搬 Visual Studio 习惯需要显式键绑定 |
| **板卡部署方式** | `deploy-arm64.sh` 生成 `run.sh` wrapper 设置 `LD_LIBRARY_PATH`，而非用 `patchelf` 改 RPATH | 本机未装 `patchelf`；wrapper 方案零依赖、可读性更好，且换板卡路径只需改一行 |

**踩坑记录**（均为 MSVC 宽松、GCC 严格导致）：

1. **`enum AVPixelFormat;` 前向声明**：C++ 标准下无固定底层类型的无作用域枚举不可前向声明，MSVC 不检查但 GCC 11 报 `underlying type mismatch`。改为 `#include <libavutil/pixfmt.h>`（纯枚举+宏，无需 `extern "C"`）。

2. **`goto` 跨越带初始化器的局部变量**：`exportworker.cpp`/`mergeworker.cpp`/`audiomixworker.cpp` 中 `goto done` 跳过了 `int videoStreamIdx = ...` 这类带初始化器的声明，MSVC 放行、GCC 报 `crosses initialization of`。修复：将声明提到函数顶部仅声明不初始化，或用 `{ }` 给局部变量整体限定作用域使其在 `goto` 目标处已脱离作用域。

3. **Windows 专属代码未加宏保护**：`main.cpp` 中 `setDarkTitleBar(HWND hwnd)` 函数签名本身含 `HWND` 类型，仅函数体内逻辑包了 `#ifdef Q_OS_WIN`，GCC 下 `HWND` 未声明直接报错。修复：把整个函数定义和调用都移入 `#ifdef Q_OS_WIN`。

4. **运行时找不到 `libQt5Multimedia.so.5` / `libavformat.so.62`**：前者是板卡未装 Qt Multimedia 模块（`apt install libqt5multimedia5`）；后者是交叉编译产物 RUNPATH 写死本机 sysroot 路径，板卡上不存在且 `.so.62` 未注册到 `ldconfig`，靠 `run.sh` 的 `LD_LIBRARY_PATH` 兜底。

**端对端验证**：`build-arm64/RambosPlayer` 为合法 aarch64 PIE ELF；`ldd`（带 `LD_LIBRARY_PATH`）所有依赖均解析成功；Windows 端 `git diff` 逐项确认改动均被 `CMAKE_CROSSCOMPILING`/`Q_OS_WIN` 隔离或为语义等价的合法 C++，不影响 MSVC 构建。

---

#### 13. 拉流播放：异步探测 + 自动重连 + 实时统计（Phase 14）

**问题**：如何让播放器支持 RTMP/RTSP/HTTP-FLV/SRT 网络流拉取播放，同时避免 `avformat_open_input` 网络握手阻塞 UI 线程？

**核心设计**：

```
MainWindow (streamBar: URL 输入 + 连接按钮)
    │ url + connectStreamBtn::clicked
    ▼
PlayerController::open(url)
    │ QThread::create → DemuxThread::probeOpen(url)  ← 静态方法，worker 线程
    │ QThread::finished → onProbeFinished(result, gen)
    ▼
DemuxThread::adopt(fmtCtx, videoQ, audioQ)  ← 主线程接管
    │ start() → run()
    ▼
av_read_frame 循环
    ├─ 正常分发 → videoQueue / audioQueue
    ├─ EOF/error + isNetwork_ → reconnect() (2s 间隔重试)
    └─ 1s 窗口统计 → emit statsUpdated(kbps, fps)
```

**设计决策解释**：

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **探测方式** | `probeOpen()` 为纯静态方法，不访问 this | 可在任意 worker 线程调用，避免 `avformat_open_input` + `avformat_find_stream_info` 网络握手阻塞 UI（DNS 解析 + TCP 握手可能耗时数秒） |
| **异步回调** | `QThread::create` + `finished` 信号 + `probeGeneration_` 计数器 | `open()` 返回 void，结果通过 `openResult(bool)` 信号异步通知；连续快速调用 open() 时，gen 不匹配的过期结果被丢弃（释放 fmtCtx），避免竞态 |
| **重连策略** | 2 秒间隔循环重试，分段 sleep（100ms 步进）检查 abort | 用户点"断开"可立即退出重连循环；emit `Reconnecting` 状态让 UI 显示"重连中..." |
| **超时配置** | RTSP: `stimeout=5s`，HTTP: `rw_timeout=5s`，RTMP/SRT: 默认 | `buildNetworkOptions()` 按 URL scheme 自动构造 AVDictionary，避免 `avformat_open_input` 永久阻塞 |
| **状态机** | `NetworkState` 枚举 (Disconnected/Connecting/Connected/Reconnecting) + Qt 信号链 | DemuxThread → PlayerController → MainWindow 三层转发，UI 状态栏实时更新 |
| **统计方式** | 1 秒窗口累加 `pkt->size` 和视频包计数 | 码率 = bytes*8/elapsed，帧率 = videoFrames*1000/elapsed，emit `statsUpdated(kbps, fps)` |
| **直播适配** | `duration=0` 时进度条禁用 + 显示 "LIVE" | 网络直播流无总时长，seek 无意义 |
| **地址记忆** | QSettings 存储上次成功 URL，打开拉流面板自动回填 | 重复测试同一流地址时无需反复输入 |

**关键接口变化**：

| 接口 | 变化前 | 变化后 |
|------|--------|--------|
| `PlayerController::open()` | `bool open(path)` 同步返回 | `void open(path)` 异步，结果通过 `openResult(bool)` 信号 |
| `DemuxThread::open()` | 直接打开 fmtCtx | 改为 `probeOpen()` 静态 + `adopt()` 主线程接管（保留同步便捷封装供测试用） |
| `DemuxThread` 新增信号 | 无 | `networkStateChanged(int)` + `statsUpdated(int kbps, double fps)` |
| `MainWindow` 新增 UI | 无 | `streamBar`（URL 输入 + 连接按钮 + 状态/码率/帧率标签）+ 工具菜单"拉流播放"开关 |

**踩坑记录**：

1. **`avformat_network_init()` 必须在 main() 中调用**：不调用则 RTMP/RTSP 协议的 `avformat_open_input` 返回 "Protocol not found"。与 `avformat_network_deinit()` 配对放在 `QApplication` 生命周期内。

2. **FFmpeg 7.0 `writeCallback` 签名变化**：`avio_alloc_context` 的 `write_packet` 回调缓冲区参数从 `uint8_t*` 改为 `const uint8_t*`。用 `#if LIBAVFORMAT_VERSION_MAJOR >= 61` 条件编译 `AvioBuf` 类型别名，HttpFlvServer 和 MpegTsServer 共用。

3. **异步 open 后 UI 状态竞态**：`openFile()` 和 `onConnectStreamClicked()` 都调用 `player_->open()`，但后续逻辑（播放/推流/缩略图提取）依赖探测结果。引入 `pendingOpenIsStream_` / `pendingOpenPath_` / `pendingAutoPlay_` 三个状态变量，在 `onPlayerOpenResult` 回调中区分来源执行不同收尾逻辑。
