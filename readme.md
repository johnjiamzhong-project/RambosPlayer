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
| Task 2 — FrameQueue 线程安全队列（TDD） | 📋 |
| Task 3 — AVSync 音频时钟（TDD） | 📋 |
| Task 4 — DemuxThread 解复用线程（TDD） | 📋 |
| Task 5 — VideoDecodeThread | 📋 |
| Task 6 — AudioDecodeThread | 📋 |
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
    │ videoFrameQueue                    │ QAudioSink (PCM)
    ▼                                    │ 维护音频时钟
VideoRenderer (QPainter)  ◄─── AVSync ◄─┘
```

---

## 功能路线图

详细任务清单（含逐阶段 checkbox）见 [docs/DEVPLAN.md](docs/DEVPLAN.md)。

