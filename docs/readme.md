# docs

项目文档与可视化图表集中管理目录。

## 开发计划

| 文件 | 说明 |
|------|------|
| `DEVPLAN.md` | 10 阶段总体开发计划，定义各阶段目标与依赖关系 |
| `superpowers/plans/2026-04-26-rambos-player-core.md` | Phase 1–6 TDD 详细执行计划，含完整代码 |
| `superpowers/plans/2026-05-08-demuxthread.md` | DemuxThread 模块执行计划 |
| `superpowers/plans/2026-05-17-phase9-restream.md` | Phase 9 推流重构计划：从录屏推流转为解复用分叉直通 |

## 设计规格

| 文件 | 说明 |
|------|------|
| `superpowers/specs/2026-05-08-framequeue-design.md` | FrameQueue 有界阻塞队列设计规格 |
| `superpowers/specs/2026-05-08-demuxthread-design.md` | DemuxThread 解复用线程设计规格 |
| `superpowers/specs/2026-05-11-phase8-filter-design.md` | Phase 8 视频滤镜（FilterGraph）设计规格 |
| `superpowers/specs/demuxthread-design.html` | DemuxThread 设计可视化 |

## 项目文档

| 文件 | 说明 |
|------|------|
| `BUGFIX-LOG.md` | 开发计划之外的 bug 修复记录，含日期、现象、根因、修复方案 |
| `LOGGING.md` | 日志系统使用指南：日志级别、过滤规则、trace 类别、崩溃 dump 配置 |
| `interview-claude-code-workflow.md` | 面试总结：各模块设计决策、TDD 案例、Q&A 积累 |
| `seek-recording-fix.md` | seek 期间推流/录制内容错乱问题的完整修复说明，含根因分析与各模块改动详情 |
| `streaming-test-guide.md` | HTTP-FLV / SRT 推流测试指南，含防火墙配置与浏览器验证步骤 |

### seek 推流问题简述

推流功能上线后，在播放器执行 seek（跳转）操作时发现录制输出异常：跳转后录制文件前几秒画面丢失，且 seek 落点附近约 10 秒出现花屏。排查后发现根因有两层：

**一、H.264 GOP 结构与 seek 的错位**。`av_seek_frame` 必须落在目标时间点之前的关键帧（I 帧），播放器会解码关键帧到目标之间的帧用于"解码器热身"但不显示，录制器却把这段热身帧全部写入了文件，造成内容错乱。

**二、原 warmup+IDR 方案的 SPS/PPS 不兼容**。为消除热身帧，早期版本在 seek 后用 libx264 重新编码一个 IDR 帧写入，之后恢复 `-c copy`。但 IDR 帧的编码参数（SPS/PPS）来自 libx264，后续 copy 帧的参数来自源编码器，两段码流参数不兼容，播放器解码 IDR 后无法正确解析后续帧，导致花屏。

最终方案是引入**关键帧抑制期**：seek 后丢弃所有 PTS 小于目标的帧，在首个 PTS ≥ 目标且为关键帧的包处退出抑制并继续 `-c copy`，H.264 参考链全程自洽。音频单独对齐到视频关键帧落点，避免 A/V 内容错位。同步修复了网络推流（MuxThread）seek 后 PTS 归零导致接收端时间轴回跳的问题。

## 技术方案

| 文件 | 说明 |
|------|------|
| `solutions/low-latency-streaming.md` | 低延迟推流方案设计：GPU 实时重编码 + mpegts.js 追帧，目标延迟 ≤ 600ms |

## 可视化图表

| 文件 | 说明 |
|------|------|
| `架构流程图.html` | 完整播放管线：文件 → 解复用 → 解码 → 渲染 → 输出，含模块卡片和关键场景时序 |
| `数据获取与处理走向.html` | PlayerController 内部数据流向：队列、线程、信号连接关系 |
| `界面操作与数据逻辑.html` | 主窗口 UI 操作逻辑与数据流，含推流交互 |
| `界面操作与数据逻辑2.html` | 主窗口 UI 操作逻辑补充（剪辑模式、滤镜面板） |
| `软解与硬解对比.html` | 软件解码 vs D3D11VA 硬件解码流程对比 |
| `滤镜面板操作逻辑.html` | FilterPanel / FilterGraph 滤镜调参与在线重建流程 |
| `推流管线架构（已被重构掉）.html` | 初版推流架构（CaptureThread 录屏方案，已废弃） |
| `推流管线架构2(已被重构掉).html` | 初版推流架构补充（已废弃） |
| `推流架构-Phase9重构.html` | Phase 9 重构后推流架构：解复用分叉 + 多路 MuxThread/HttpFlvServer |
| `player-arch.html` | 播放器整体架构概览 |
| `phase9-pipeline.html` | Phase 9 推流管线详细流程 |

## 子目录

| 目录 | 说明 |
|------|------|
| `superpowers/` | Superpowers 执行计划（`plans/`）与规格文档（`specs/`），含 TDD 任务分解 |
| `solutions/` | 技术方案设计文档 |
