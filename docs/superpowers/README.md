# docs/superpowers

本目录存放由 Superpowers 技能生成的结构化文档，分两类子目录：

## plans/

实现计划文件（由 `superpowers:writing-plans` 技能生成）。每份文件对应一个开发阶段，包含任务分解、逐步 checkbox、完整代码片段，供 `superpowers:executing-plans` 或 `superpowers:subagent-driven-development` 技能按步骤执行。

| 文件 | 覆盖范围 |
|------|----------|
| [2026-04-26-rambos-player-core.md](plans/2026-04-26-rambos-player-core.md) | Task 1–10：项目脚手架 → FrameQueue → AVSync → DemuxThread → 解码线程 → VideoRenderer → PlayerController → MainWindow → 集成验证 |
| [2026-05-08-demuxthread.md](plans/2026-05-08-demuxthread.md) | Task 4 专项计划：`DemuxThread` av_read_frame 循环、stop/seek 支持 |
| [2026-05-17-phase9-restream.md](plans/2026-05-17-phase9-restream.md) | Phase 9 重构：推流源改为解码帧分叉，音视频双流，RTMP/SRT/本地录制多目标，三选项可单多选 UI |

## specs/

设计规格文件，记录单个组件的接口约定、行为契约和边界条件，是实现前的"需求文档"。

| 文件 | 组件 |
|------|------|
| [2026-05-08-framequeue-design.md](specs/2026-05-08-framequeue-design.md) | `FrameQueue<T>` — 线程安全有界阻塞队列 |
| [2026-05-08-demuxthread-design.md](specs/2026-05-08-demuxthread-design.md) | `DemuxThread` — FFmpeg 解复用线程，stop/seek/内存/测试策略 |
| [2026-05-11-phase8-filter-design.md](specs/2026-05-11-phase8-filter-design.md) | Phase 8 视频滤镜编辑器 — FilterGraph + VideoDecodeThread 集成 + FilterPanel UI |
