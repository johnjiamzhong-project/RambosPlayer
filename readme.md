# RambosPlayer 多媒体播放器

> 基于 FFmpeg + Qt 的多线程媒体播放器，目标是掌握音视频同步、硬件加速与实时流媒体处理。

## 文档

- 开发计划总览：[docs/DEVPLAN.md](docs/DEVPLAN.md)（Phase 1–10）
- 详细 TDD 计划：[docs/superpowers/plans/2026-04-26-rambos-player-core.md](docs/superpowers/plans/2026-04-26-rambos-player-core.md)

| 功能 | 状态 |
|------|:----:|
| 功能1 — 核心播放器 | 🔨 进行中 |
| 功能2 — 完整播放器 UI | 📋 待开始 |
| 功能3 — 硬件加速解码 | 📋 待开始 |
| 功能4 — 视频滤镜编辑器 | 📋 待开始 |
| 功能5 — 屏幕录制 / 推流 | 📋 待开始 |
| 功能6 — 视频剪辑器 | 📋 待开始 |

## 功能1 进度

| 任务 | 状态 |
|------|:----:|
| Task 1 — 项目脚手架（CMake + Qt5 + FFmpeg） | ✅ 完成 |
| Task 2 — FrameQueue 线程安全队列（TDD） | ✅ 完成 |
| Task 3 — AVSync 音频时钟（TDD） | ✅ 完成 |
| Task 4 — DemuxThread 解复用线程（TDD） | ✅ 完成 |
| Task 5 — VideoDecodeThread | ✅ 完成 |
| Task 6 — AudioDecodeThread | ✅ 完成 |
| Task 7 — VideoRenderer | 📋 |
| Task 8 — PlayerController | 📋 |
| Task 9 — MainWindow UI | 📋 |
| Task 10 — 集成验证 | 📋 |

---

## 环境

| 项目 | 版本 / 路径 |
|------|-------------|
| IDE | VSCode + CMake Tools |
| 构建系统 | CMake 3.16+，preset: `default` |
| UI 框架 | Qt 5.14.2（`E:\Qt\Qt5.14.2\5.14.2\msvc2017_64`） |
| FFmpeg | vcpkg，toolchain `D:\vcpkg\scripts\buildsystems\vcpkg.cmake` |
| 编译器 | MSVC 2017 x64 |

---

## 架构

```
UI 层 (MainWindow)
    │
    ▼
PlayerController
    │
    ▼
DemuxThread  ──────────────────────────────────┐
    │ videoPacketQueue          audioPacketQueue │
    ▼                                           ▼
VideoDecodeThread                     AudioDecodeThread
    │ videoFrameQueue                    │ QAudioOutput (PCM)
    ▼                                    │ 维护音频时钟
VideoRenderer (QPainter)  ◄─── AVSync ◄─┘
```

### 核心组件

| 组件 | 文件 | 说明 |
|------|------|------|
| `FrameQueue<T>` | `src/framequeue.h` | 线程安全有界阻塞队列，生产者满时阻塞，消费者空时按超时等待，`abort()` 解锁所有等待线程 |
| `AVSync` | `src/avsync.h/.cpp` | 原子量存储音频时钟 PTS，`videoDelay(pts)` 返回视频帧应等待的毫秒数；落后超过 400 ms 返回 0（丢帧） |
| `DemuxThread` | `src/demuxthread.h/.cpp` | `av_read_frame` 循环，按流索引将 `AVPacket*` 分发到视频/音频包队列 |
| `VideoDecodeThread` | `src/videodecodethread.h/.cpp` | 消费视频包队列，解码为 `AVFrame*` 推入帧队列 |
| `AudioDecodeThread` | `src/audiodecodethread.h/.cpp` | 解码 + `swr_convert` 重采样为 S16 立体声 → `QAudioOutput`，同步更新音频时钟 |
| `VideoRenderer` | `src/videorenderer.h/.cpp` | `QTimer` 1 ms 检查渲染时机，`sws_scale` YUV420P→RGB32，`QPainter` 绘帧 |
| `PlayerController` | `src/playercontroller.h/.cpp` | 持有并管理所有组件生命周期，对外暴露 open/play/pause/stop/seek/setVolume |
| `MainWindow` | `src/mainwindow.h/.cpp` | Qt 主窗口，`QSlider` 进度条 + 音量，播放/暂停按钮，双击全屏 |

---

## 功能路线图

详细任务清单（含逐阶段 checkbox）见 [docs/DEVPLAN.md](docs/DEVPLAN.md)。

