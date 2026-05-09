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

- Phase 1–4（项目脚手架、FrameQueue、AVSync、DemuxThread、VideoDecodeThread、AudioDecodeThread）通过这套流程完整实现，包括多线程架构、解码、音视频同步、seek 支持等核心模块
- 每个功能都有对应 spec 和 plan 文档，可追溯决策过程

---

### 强调的差异点

> "我不是把 Claude 当搜索引擎用，而是用它做结构化的工程决策流程——设计评审、任务分解、代码审查都在流程里，而不是单点问答。"

---

*基于 RambosPlayer 项目（FFmpeg + Qt 多媒体播放器）的实际开发经验，2026-04-26*

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
用户拖拽进度条
  ↓
DemuxThread::seek(targetMs) 接收请求
  ↓
av_seek_frame() 定位到关键帧
  ↓
清空 videoPacketQueue 和 audioPacketQueue
  ↓
VideoDecodeThread 和 AudioDecodeThread 调用 codec->flush()
  ↓
PlayerController::reset() 清空 frame 队列和 abort 标志
  ↓
重新播放，AVSync 重新同步
```

**关键点**：必须清空所有队列，否则旧包会污染新解码。

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
