# RambosPlayer 多媒体播放器

> 基于 FFmpeg + Qt 的多线程多媒体播放器，目标是覆盖 FFmpeg 全链路实战 —— 能播、能录、能推、能剪。



## 文档

- 开发计划总览：[docs/DEVPLAN.md](docs/DEVPLAN.md)（Phase 1–12）
- 详细 TDD 计划（Phase 1–6）：[docs/superpowers/plans/2026-04-26-rambos-player-core.md](docs/superpowers/plans/2026-04-26-rambos-player-core.md)
- 视频剪辑器增强计划（Phase 12）：[docs/superpowers/plans/2026-05-27-phase12-video-editor.md](docs/superpowers/plans/2026-05-27-phase12-video-editor.md)
- 架构流程图：[docs/架构流程图.html](docs/架构流程图.html)
- 软解 vs 硬解对比：[docs/软解与硬解对比.html](docs/软解与硬解对比.html)
- 低延迟推流方案：[docs/solutions/low-latency-streaming.md](docs/solutions/low-latency-streaming.md)（GPU 实时重编码 + mpegts.js 追帧）
- 推流方案对比架构图：[docs/推流方案对比架构图.html](docs/推流方案对比架构图.html)（HTTP-FLV vs HTTP-MPEG-TS 管线对比）
- 导出管线优化分析：[docs/EXPORT_OPTIMIZATION.md](docs/EXPORT_OPTIMIZATION.md)（架构、瓶颈、6 项优化策略）
- 导出性能基准记录：[docs/EXPORT_BENCHMARK.md](docs/EXPORT_BENCHMARK.md)（各阶段实测数据，11 次测试）

| 功能 | 状态 |
|------|:----:|
| 功能1 — 核心播放器 | ✅ 完成 |
| 功能2 — 完整播放器集成 | ✅ 完成 |
| 功能3 — 硬件加速解码 | ✅ 完成 |
| 功能4 — 视频滤镜编辑器 | ✅ 完成 |
| 功能5 — 推流播放内容（音视频双流，RTMP/SRT/HTTP-FLV） | ✅ 完成 |
| 功能6 — 视频剪辑器（自由剪切） | ✅ 完成 |
| 功能7 — 低延迟推流（GPU 重编码 + mpegts.js） | ✅ 完成 |
| 功能8 — 三模式剪切（浏览/多段）+ 视频合并 | ✅ 三模式完成 / 🚧 合并计划中 |

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
| 推流播放内容（RTMP / SRT / HTTP-FLV） | 菜单 → 推流，支持多路同时推流；HTTP-FLV 无需外部服务器，局域网浏览器直接打开链接收看 |
| 视频剪辑（自由剪切） | 菜单 → 剪辑 → 自由剪切（Ctrl+T），拖拽入/出点，Ctrl+E 导出 |
| 浏览剪切（边播边标） | 菜单 → 剪辑 → 浏览剪切（Ctrl+B），播放中标记入点/出点，自动追加到底部导轨 | ✅ 完成 |
| 多段剪切（批量区间） | 菜单 → 剪辑 → 多段剪切（Ctrl+M），逐行输入时间区间，批量导出 | ✅ 完成 |
| 视频拼接 / 音频混音 / 混流 | 菜单 → 合并（Ctrl+G），拖入多个文件，自动判断合并模式 | 🚧 计划中 |

---

## 环境

| 项目 | 版本 / 路径 |
|------|-------------|
| IDE | VSCode + CMake Tools |
| 构建系统 | CMake 3.16+，preset: `default` |
| UI 框架 | Qt 5.14.2（`E:\Qt\Qt5.14.2\5.14.2\msvc2017_64`） |
| FFmpeg | vcpkg，toolchain `E:\vcpkg\scripts\buildsystems\vcpkg.cmake` |
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

推流分两种模式，用户根据延迟需求选择：

**直通模式**（`-c copy`，当前默认）：在解复用层直接分叉压缩包，无需解码再编码，CPU 开销极低。

**低延迟模式**（计划中）：插入解码→GPU 重编码管线（`h264_nvenc`），强制小 GOP（0.5s），配合 mpegts.js 追帧，预期浏览器延迟 ≤ 600ms。详见 [低延迟推流方案](docs/solutions/low-latency-streaming.md)。

```
直通模式 (-c copy):
DemuxThread ──→ videoPacketQueue ──→ VideoDecodeThread ──→ VideoRenderer（播放）
            ──→ audioPacketQueue ──→ AudioDecodeThread ──→ QAudioOutput（播放）
            ↘ restreamVideoQ ──→ MuxThread[0]       → rtmp://...（RTMP/SRS）
            ↘ restreamAudioQ ──→ MuxThread[1]       → record.flv（本地录制）
            ↘ httpFlvVideoQ  ──→ HttpFlvServer → HTTP → 浏览器（局域网）
            ↘ httpFlvAudioQ  ──┘

低延迟模式 (GPU re-encode, 计划中):
DemuxThread ──→ StreamVideoDecoder ──→ EncodeThread (h264_nvenc, GOP=0.5s) ──┐
            ──→ StreamAudioDecoder ──→ AudioEncodeThread (AAC) ─────────────┤
                                                                              ▼
                                                          MpegTsServer → HTTP → mpegts.js
```

| 推流协议 | 地址格式 | 适用场景 |
|----------|----------|----------|
| RTMP | `rtmp://127.0.0.1:1935/live/test` | 在线平台（Twitch/B站）或本地 SRS 服务器 |
| SRT | `srt://:9000` | 局域网直连（平板/手机用 VLC 拉流），无需服务器 |
| HTTP-FLV | 端口 8080（内置） | 局域网浏览器直接打开 `http://IP:8080/player.html`，无需安装任何客户端；**仅支持 H.264 编码的视频**（MPEG-4 等格式不支持 FLV 容器） |
| HTTP-MPEG-TS | 端口 8090（内置） | **低延迟模式**：GPU 重编码（h264_nvenc + AAC），GOP=0.5s，浏览器延迟 ≤ 600ms；需将 `mpegts.min.js` 放到 exe 同目录 |
| 本地录制 | `C:/record.flv` | 录制当前播放内容到文件 |

> 重构计划：[docs/superpowers/plans/2026-05-17-phase9-restream.md](docs/superpowers/plans/2026-05-17-phase9-restream.md)

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
| `MuxThread` | `src/muxthread.h/.cpp` | 消费音视频压缩包（`-c copy` 直通），`av_write_frame` 写出；支持 RTMP（FLV）和 SRT（MPEGTS），seek 后 PTS 续接保证单调递增 |
| `ExportWorker` | `src/exportworker.h/.cpp` | 帧精确重编码导出线程（QThread）；解码源文件 → H.264 + AAC 重编码 → MP4 封装，支持单段/批量导出。CRF 17 / CQP 18 高画质，superfast 预设快速编码。详见 [导出优化分析](docs/EXPORT_OPTIMIZATION.md) |
| `HttpFlvServer` | `src/httpflvserver.h/.cpp` | 内置 HTTP-FLV 流媒体服务器；FFmpeg FLV muxer 通过自定义 avio 将数据广播给所有已连接的浏览器客户端；含音视频 PTS 对齐、关键帧门控、内嵌 flv.js 播放页 |
| `StreamController` | `src/streamcontroller.h/.cpp` | 管理多路 MuxThread / HttpFlvServer 生命周期；向 DemuxThread 提供 restream 分叉队列入口 |

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
│   ├── DEVPLAN.md              # 开发计划总览（Phase 1–12，含 Task 编号）
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



# 其他

## 已知问题

| 问题       |   状态   | 说明                                                         |
| ---------- | :------: | ------------------------------------------------------------ |
| 水印不显示 | 🔴 待解决 | FFmpeg `movie` 滤镜无法正确解析含盘符的 Windows 绝对路径（`G:/...`）。盘符中的 `:` 经 `avfilter_graph_parse_ptr` → `av_get_token` → `avfilter_init_str` → `av_set_options_string` 四层解析后始终无法正确传递到 movie filter 的 `filename` 选项。`avformat_open_input` 直接打开文件正常（验证文件存在且可读），但通过滤镜图字符串传参时多层 `\` 转义均无法穿透。需研究 FFmpeg 源码中 `avfilter_graph_parse2` 对 movie 源滤镜的选项传递机制后再解决。 |

---

## 

### 为什么一个"播放器"有推流和剪辑？

传统播放器（VLC、PotPlayer、MPC-HC）只需解复用→解码→渲染，但本项目的学习目标是 **FFmpeg 全链路**：

- **解码链路（播放）**：`av_read_frame` / `avcodec_send_packet` / `sws_scale` — 数据从文件流向屏幕

- **编码链路（推流）**：`avcodec_send_frame` / `av_interleaved_write_frame` — 解码帧直接分叉进入编码管线，推送到 RTMP 服务器或 SRT 对端；音视频双流，支持多路同时推流

- **重编码链路（剪辑导出）**：解码 → 像素格式转换 → H.264 重编码（CRF 17 / CQP 18）+ AAC 音频重编码 → MP4 封装；帧精确裁剪，输出时长与选中区间完全一致。详见 [导出管线优化分析](docs/EXPORT_OPTIMIZATION.md)

  三条链路合在一起才构成完整的 FFmpeg 技能树，所以定位更接近**"多媒体处理工具箱"**而非纯播放器。
