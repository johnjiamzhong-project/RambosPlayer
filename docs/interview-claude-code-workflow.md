# 如何用 Claude Code 从 0 开发项目（面试回答）

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

- 功能1+2（核心播放器 + UI）通过这套流程完整实现，包括多线程架构、音视频同步、seek、进度条、全屏等
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

#### 2. 音视频同步策略

**问题**：如何精确同步音频和视频播放？

**设计架构**：
- **音频作为主时钟**：AudioDecodeThread 通过 QAudioSink 输出 PCM，AVSync 维护原子量存储当前音频 PTS
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

#### 3. Seek（拖拽进度条）实现

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

**为什么这样做重要**：
- 不写代码就写测试，确保测试不是自动通过的废纸
- 看到测试失败，证明它真的在验证功能
- 最小实现避免 over-engineering

---

### 项目文档结构

```
docs/
├── DEVPLAN.md                          # 10 个 Phase 的高层计划
├── superpowers/
│   ├── specs/
│   │   └── 2026-05-08-framequeue-design.md    # API 表、设计决策、测试矩阵
│   └── plans/
│       └── 2026-04-26-rambos-player-core.md   # 6 个 Phase 的 TDD 分步指南
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
