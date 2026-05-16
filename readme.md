# RambosPlayer 多媒体处理工具箱

> 基于 FFmpeg + Qt 的多线程多媒体实验项目，目标是覆盖 FFmpeg 全链路实战 —— 能播、能录、能推、能剪。

### 为什么一个"播放器"有录屏和推流？

传统播放器（VLC、PotPlayer、MPC-HC）只需解复用→解码→渲染，但本项目的学习目标是 **FFmpeg 全链路**：

- **解码链路（播放）**：`av_read_frame` / `avcodec_send_packet` / `sws_scale` — 数据从文件流向屏幕
- **编码链路（推流）**：`avcodec_send_frame` / `av_interleaved_write_frame` — 数据从采集设备流向网络

两条链路合在一起才构成完整的 FFmpeg 技能树。加上视频剪辑器（关键帧定位 + 无损剪切），覆盖了 `av_seek_frame` / `av_interleaved_write_frame` 的导出场景。所以定位更接近**"多媒体处理工具箱"**而非纯播放器。

## 文档

- 开发计划总览：[docs/DEVPLAN.md](docs/DEVPLAN.md)（Phase 1–10）
- 详细 TDD 计划：[docs/superpowers/plans/2026-04-26-rambos-player-core.md](docs/superpowers/plans/2026-04-26-rambos-player-core.md)
- 架构流程图：[docs/架构流程图.html](docs/架构流程图.html)
- 软解 vs 硬解对比：[docs/软解与硬解对比.html](docs/软解与硬解对比.html)

| 功能 | 状态 |
|------|:----:|
| 功能1 — 核心播放器 | ✅ 完成 |
| 功能2 — 完整播放器集成 | ✅ 完成 |
| 功能3 — 硬件加速解码 | ✅ 完成 |
| 功能4 — 视频滤镜编辑器 | ✅ 完成 |
| 功能5 — 屏幕录制 / 推流 | ✅ 完成 |
| 功能6 — 视频剪辑器 | ✅ 完成 |

---

## 使用指南

### 打开文件

| 方式 | 操作 |
|------|------|
| 菜单 | 文件 → 打开（或 Ctrl+O） |
| 最近文件 | 文件 → 最近文件 → 选择（支持 &1–&9 快捷键） |

支持格式：`.mp4` `.mkv` `.avi` `.mov` `.flv` `.wmv`

### 播放控制

| 功能 | 操作 |
|------|------|
| 播放 / 暂停 | 点击 ▶/⏸ 按钮，或按 **Space** |
| 快进 10 秒 | 按 **→ 右箭头** |
| 快退 10 秒 | 按 **← 左箭头** |
| 跳转到指定位置 | 点击进度条任意位置（点哪跳哪） |
| 拖拽跳转 | 拖动进度条滑块 |
| 音量调节 | 拖动音量滑块，或点击轨道直接跳值 |
| 全屏 / 退出全屏 | 双击视频区域 |

> 点击进度条后可直接用方向键继续快进/快退，焦点不丢失。

### 高级功能

| 功能 | 入口 |
|------|------|
| 硬件加速（D3D11VA） | 菜单 → 硬件加速（重新打开文件后生效） |
| 视频滤镜（亮度/对比度/饱和度/水印） | 菜单 → 滤镜编辑器，实时预览 |
| 屏幕录制 / RTMP 推流 | 菜单 → 推流，输入采集源和目标 URL |
| 视频剪辑（无损剪切导出） | 菜单 → 剪辑模式（Ctrl+T），拖拽入/出点，Ctrl+E 导出 |

---

## 已知问题

| 问题 | 状态 | 说明 |
|------|:----:|------|
| 水印不显示 | 🔴 待解决 | FFmpeg `movie` 滤镜无法正确解析含盘符的 Windows 绝对路径（`G:/...`）。盘符中的 `:` 经 `avfilter_graph_parse_ptr` → `av_get_token` → `avfilter_init_str` → `av_set_options_string` 四层解析后始终无法正确传递到 movie filter 的 `filename` 选项。`avformat_open_input` 直接打开文件正常（验证文件存在且可读），但通过滤镜图字符串传参时多层 `\` 转义均无法穿透。需研究 FFmpeg 源码中 `avfilter_graph_parse2` 对 movie 源滤镜的选项传递机制后再解决。 |

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

### 推流管线

```
MainWindow
    │ streamCtrl_->start(source, url)
    ▼
StreamController
    │  持有 rawFrameQ_ + encodedPacketQ_
    ▼
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  CaptureThread   │ ──▶ │  EncodeThread    │ ──▶ │   MuxThread      │
│  gdigrab/dshow   │     │  H.264 编码      │     │  FLV/RTMP 封装   │
│  → AVFrame*(BGR0)│     │  → AVPacket*     │     │  → 本地/网络输出  │
└──────────────────┘     └──────────────────┘     └──────────────────┘
      rawFrameQ_              encodedPacketQ_
      (cap 30)                (cap 60)
```

> 详细推流管线架构：[docs/推流管线架构2.html](docs/推流管线架构2.html)

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
| `MainWindow` | `src/mainwindow.h/.cpp` | Qt 主窗口，`QSlider` 进度条 + 音量，播放/暂停按钮，双击全屏，滤镜面板 Dock |
| `FilterGraph` | `src/filtergraph.h/.cpp` | FFmpeg libavfilter 封装，buffersrc → 滤镜链 → buffersink，支持在线重建 |
| `FilterPanel` | `src/filterpanel.h/.cpp/.ui` | 滤镜调参面板（QDockWidget），亮度/对比度/饱和度/水印，参数实时生效 |
| `CaptureThread` | `src/capturethread.h/.cpp` | 桌面 (gdigrab) 或摄像头 (dshow) 采集，解码后输出 AVFrame* 到 FrameQueue |
| `EncodeThread` | `src/encodethread.h/.cpp` | 消费原始帧队列，sws_scale 转 YUV420P，H.264 编码后输出 AVPacket* |
| `MuxThread` | `src/muxthread.h/.cpp` | 消费编码包队列，av_interleaved_write_frame 写 FLV/RTMP，停止时 avio_closep 冲刷文件 |
| `StreamController` | `src/streamcontroller.h/.cpp` | 持有 CaptureThread + EncodeThread + MuxThread 生命周期，管理启动/停止顺序 |

---

## 目录结构

```
RambosPlayer/
├── src/                        # 全部源码（.h / .cpp / .ui）
├── tests/                      # Qt Test 单元测试
│   ├── tst_framequeue.cpp
│   ├── tst_avsync.cpp
│   ├── tst_demuxthread.cpp
│   └── data/sample.mp4         # 测试用 2 秒视频（ffmpeg 生成）
├── docs/
│   ├── DEVPLAN.md              # 开发计划总览（Phase 1–10，含 Task 编号）
│   ├── BUGFIX-LOG.md           # 计划外 Bug 修复记录
│   ├── interview-claude-code-workflow.md  # Claude Code 工作流记录（需求→计划→执行全流程、架构决策与收益总结）
│   ├── *.html                  # 架构流程图（可视化）
│   └── superpowers/
│       ├── plans/              # 逐步骤 TDD 执行计划（含完整代码）
│       └── specs/              # 设计规格文档
├── build/                      # CMake 构建输出（不纳入版本控制）
├── logs/                       # 运行时日志（不纳入版本控制）
├── CMakeLists.txt              # 主构建配置
└── CMakePresets.json           # 构建预设（preset: default，VS2017 x64）
```

---

