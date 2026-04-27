# RambosPlayer 多媒体播放器

> 基于 FFmpeg + Qt 的多线程媒体播放器，目标是掌握音视频同步、硬件加速与实时流媒体处理。

## 文档

- 开发计划总览：[docs/DEVPLAN.md](docs/DEVPLAN.md)（Phase 1–10）
- 详细 TDD 计划：[docs/superpowers/plans/2026-04-26-rambos-player-core.md](docs/superpowers/plans/2026-04-26-rambos-player-core.md)

| 功能 | 状态 |
|------|:----:|
| 功能1 — 核心播放器。；/ | 📋 |
| 功能2 — 完整播放器 UI | 📋 |
| 功能3 — 硬件加速解码 | 📋 |
| 功能4 — 视频滤镜编辑器 | 📋 |
| 功能5 — 屏幕录制 / 推流 | 📋 |
| 功能6 — 视频剪辑器 | 📋 |

---

## 环境

| 项目 | 版本 / 路径 |
|------|-------------|
| IDE | Visual Studio 2017 |
| UI 框架 | Qt 5.14.2 |
| FFmpeg | 通过 vcpkg 安装，路径 `D:\vcpkg` |

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

### 初级阶段

#### 功能1 — 核心视频播放器

- [ ] 多线程架构：`QThread` 分离解复用、解码、渲染
- [ ] 线程安全帧队列：`FrameQueue<T>`（`std::queue` + `QMutex`）
- [ ] 音视频同步：以音频时钟为基准，比较 `AVFrame->pts * time_base`
- [ ] Seek：`av_seek_frame` + 清空队列 + 刷新解码器缓冲

#### 功能2 — 完整播放器 UI

- [ ] 进度条：`QSlider` 拖动 seek
- [ ] 音量控制：`QAudioSink::setVolume`
- [ ] 全屏：双击切换
- [ ] 帧率自适应渲染：`QTimer`（1 ms 轮询 + PTS 延迟计算）

### 进阶阶段

#### 功能3 — 硬件加速解码（Windows）

- [ ] D3D11VA 硬件上下文
- [ ] GPU 帧回传：NV12 → RGB（`hwdownload` / `sws_scale`）

```cpp
AVBufferRef* hw_device_ctx;
av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
```

#### 功能4 — 视频滤镜编辑器

- [ ] `libavfilter` 滤镜图
- [ ] 实时预览：亮度 / 对比度 / 裁剪 / 水印
- [ ] `avfilter_graph_parse_ptr` 支持自定义滤镜链字符串
- [ ] Qt 拖拽滤镜节点 UI

#### 功能5 — 屏幕录制 / 推流

- [ ] 采集：`gdigrab`（桌面）/ `dshow`（摄像头）
- [ ] 编码：`libx264` / `h264_nvenc`
- [ ] 推流：RTMP → OBS / 直播平台（输出格式 `flv`）

#### 功能6 — 视频剪辑器

- [ ] 时间线 UI（自定义 `QWidget` 绘制）
- [ ] 无损剪切（`-c copy`，关键帧对齐）
- [ ] 转场效果（`overlay` 滤镜）
- [ ] 多轨合并（`amix` / `overlay` 滤镜图）

