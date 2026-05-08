---
title: FrameQueue<T> 设计规格
date: 2026-05-08
phase: Task 2
---

# FrameQueue<T> 设计规格

## 背景

RambosPlayer 的线程间通信唯一机制。DemuxThread push 数据包，VideoDecodeThread / AudioDecodeThread pop 消费。有界容量提供背压；abort() 用于干净关闭所有阻塞线程。

## API

```cpp
template<typename T>
class FrameQueue {
public:
    explicit FrameQueue(int maxSize);

    void push(T item);                   // 满时阻塞；abort 后静默返回（丢弃 item）
    bool tryPop(T& out, int timeoutMs);  // 超时或 abort 返回 false
    void clear();                        // 清空队列，唤醒所有被 push 阻塞的线程
    void abort();                        // 唤醒所有阻塞线程（push + pop），标记 aborted
    void reset();                        // 清除 aborted 标志 + 清空队列（seek 后调用）
    int  size() const;                   // 线程安全大小查询
};
```

## 内部实现

| 成员 | 类型 | 说明 |
|------|------|------|
| `mutex_` | `mutable QMutex` | 保护所有成员 |
| `notEmpty_` | `QWaitCondition` | pop 等待有数据 |
| `notFull_` | `QWaitCondition` | push 等待有空位 |
| `q_` | `std::queue<T>` | FIFO 存储 |
| `maxSize_` | `int` | 容量上限 |
| `aborted_` | `bool` | 关闭标志 |

## 设计决策

- **Qt 原语**（QMutex + QWaitCondition）：与项目其余部分保持一致
- **push 返回 void**：abort 后调用方通过 `aborted_` 状态自行退出循环，无需检查返回值
- **wakeAll() in abort()**：必须唤醒所有等待线程，而非只唤醒一个
- **header-only 模板**：避免显式实例化，直接 `#include "framequeue.h"` 即用

## 测试覆盖矩阵

| 测试用例 | 验证行为 | 关键断言 |
|----------|----------|----------|
| `pushPop_singleThread` | 基本读写正确 | pop 值 == push 值 |
| `tryPop_returnsFlase_whenEmpty` | 空队列超时 | 50ms 后返回 false |
| `blocksAt_maxSize` | 满容量时 push 阻塞 | 80ms 后推送线程仍运行；pop 一个后解锁 |
| `abort_unblocks_pop` | abort 唤醒阻塞的 pop | 500ms 内等待线程退出 |
| `size_and_clear` | size/clear 行为正确 | push 3 后 size==3，clear 后 size==0 |

## 使用方式（后续 Task）

```cpp
// DemuxThread 中
FrameQueue<AVPacket*> videoPacketQueue{100};
FrameQueue<AVPacket*> audioPacketQueue{400};

// VideoDecodeThread 中
FrameQueue<AVFrame*>  videoFrameQueue{15};

// 关闭时
videoPacketQueue.abort();
audioPacketQueue.abort();
videoFrameQueue.abort();

// seek 后
videoPacketQueue.reset();
audioPacketQueue.reset();
videoFrameQueue.reset();
```
