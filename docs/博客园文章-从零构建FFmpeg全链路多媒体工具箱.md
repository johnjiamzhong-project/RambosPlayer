# 从零构建 FFmpeg 全链路多媒体工具箱：播放、推流、剪辑三合一的设计与实现

## 一、项目概述

RambosPlayer 是一个基于 FFmpeg 4.x + Qt 5.14 的 Windows 桌面多媒体实验项目，使用 C++17 开发。项目的核心目标并不是做一个"媲美 PotPlayer 的播放器"，而是通过亲手实现播放、推流、剪辑三条管线，系统性地掌握 FFmpeg 的完整 API 体系。

代码量约 5000+ 行，14 个线程协作，覆盖了 `av_read_frame` / `avcodec_send_packet` / `sws_scale` / `av_seek_frame` / `av_interleaved_write_frame` 等核心 API 的实战用法。

## 二、整体架构

项目采用多线程生产者-消费者模型，线程间通过有界阻塞队列 `FrameQueue<T>` 通信，不使用 Qt 信号槽做跨线程数据传递。

### 2.1 播放管线（解码链路）

```
UI 交互层
  │
播放控制器
  │
解复用模块
  │ 视频包队列          音频包队列
  ├──────────────┬──────────────┐
视频解码模块     音频解码模块
D3D11VA 硬解    AAC/MP3 软解
  │                 │ PCM 输出
视频滤镜         音频时钟更新
  │                 │
视频渲染  ◄─── 音视频同步
```

**为什么音频做主时钟？**

人耳对声音中断远比眼睛对帧延迟敏感。音频通过硬件 DMA 以固定速率播放，天然是稳定的时间基准；视频则根据音频时钟计算每帧的渲染时机——领先则等待，落后超过阈值则丢帧。

### 2.2 推流管线（编码链路）

```
桌面/摄像头采集 → sws_scale 转 YUV420P → H.264 编码 → FLV 封装 → RTMP 推流
```

解码链路是 `av_read_frame → avcodec_send_packet → avcodec_receive_frame`，编码链路正好相反 `avcodec_send_frame → avcodec_receive_packet → av_interleaved_write_frame`。两条链路合在一起，才构成 FFmpeg 的完整技能闭环。

### 2.3 剪辑管线

```
时间轴选择入/出点 → 缩略图提取 → 关键帧定位 → av_seek_frame 跳转 → 无损导出
```

## 三、关键设计决策

### 3.1 FrameQueue：为什么自己写而不复用 std::queue

`FrameQueue<T>` 是一个模板化的有界阻塞队列，核心需求有三点：

1. **push 满时阻塞** —— 防止生产者以远超消费者的速度推入数据，导致内存膨胀
2. **pop 空时超时等待** —— 消费者可设置超时时间（如 20ms），避免死等
3. **abort() 全局唤醒** —— 停止播放时需要立即解除所有阻塞线程

视频包队列容量设为 100，音频包队列 400（音频包密度远高于视频），视频帧队列仅 15（解码后的帧数据量大，需要限制内存占用）。

### 3.2 Seek 的坑：多线程竞态下的时钟同步

seek 是整个项目中最复杂的操作，涉及四个线程的状态重置。踩过的坑包括：

- **旧帧残留**：seek 后队列中残留目标位置前的帧，VideoRenderer 计算 `pts - audioClock` 得到正数，误判为"视频超前"暂存等待音频追上，但音频已在目标位置播放，导致画面永久卡死
- **有效包误清**：AudioDecodeThread flush 路径中的 `clear()` 清掉了 DemuxThread seek 后推入的有效新包，导致播放位置偏移 10-20 秒
- **参考帧缺失**：H.264 精确 seek 丢弃关键帧后，P/B 帧缺少参考帧持续返回 EAGAIN，视频冻结 2-5 秒直到下一个 IDR 帧出现

**最终方案：**

1. 由 PlayerController 统一清队列 → DemuxThread 跳转 → 解码线程 flush
2. 关键帧起全部放行，由 VideoRenderer 凭 PTS 过滤旧帧（`diff < -0.4s` 直接丢弃）
3. 音频时钟更新前设置过滤器，屏蔽 flush 完成前产生的旧 PTS

### 3.3 硬件加速：D3D11VA 的格式陷阱

启用硬件加速后，`avcodec_open2` 返回的 `pix_fmt` 是 `AV_PIX_FMT_D3D11`（硬件表面格式），但经过 `av_hwframe_transfer_data` 转出来的帧格式通常是 `AV_PIX_FMT_NV12`（D3D11VA 的通用软解输出）。

如果在滤镜图初始化时用 `D3D11` 格式作为 buffersrc 的像素格式，后续送入 `NV12` 帧时会直接崩溃。解决方案是：硬解场景下固定以 `NV12` 初始化滤镜图，并在首帧转换后验证实际格式，不一致则触发重建。

### 3.4 QAudioOutput 线程亲和性

Qt 的 `QAudioOutput` 要求创建、start、write、stop 全部在同一线程完成。初始设计在主线程创建、工作线程使用，导致行为异常。改为在音频解码线程的 run 函数中创建并启动，退出时 delete，确保线程亲和性正确。

### 3.5 音量同步的双时钟策略

音频时钟有两个候选来源：

- `QAudioOutput::processedUSecs()` —— 硬件实际播放量，真实可靠
- 帧 PTS × time_base —— 解码层面的位置

问题在于 `processedUSecs()` 在 stop/start 后会重置为 0，seek 后时钟瞬间归零再爬升，与视频严重不同步。最终方案是优先使用帧 PTS 推算，仅在正常播放时依赖硬件时钟。

## 四、技术栈一览

| 层 | 技术选型 | 原因 |
|---|---|---|
| 语言 | C++17 | 性能零妥协，原子操作 + RAII 管理 FFmpeg 资源 |
| UI | Qt 5.14.2 Widgets | 成熟稳定，QPainter 直接渲染视频帧 |
| 解码 | FFmpeg 4.x (vcpkg) | 全格式覆盖，libavfilter 滤镜链 |
| 硬解 | D3D11VA | Windows 原生 API，无需额外驱动，兼容性最好 |
| 音频 | QAudioOutput | 与 Qt 事件循环无缝集成 |
| 构建 | CMake + MSVC 2017 | Ninja 生成器，增量编译 < 5s |

## 五、不足与后续方向

1. **水印功能** —— FFmpeg movie 滤镜在 Windows 下无法通过滤镜图描述字符串传递含盘符的绝对路径，四层解析链路（`avfilter_graph_parse_ptr → av_get_token → avfilter_init_str → av_set_options_string`）中的冒号转义始终穿透失败，待研究 FFmpeg 源码后解决
2. **GPU 滤镜** —— 当前滤镜走 CPU 路径（libavfilter soft），可考虑接入 D3D11 视频处理器实现零拷贝
3. **播放列表** —— 目前只支持单文件播放，轮播和列表管理尚未实现
4. **跨平台** —— D3D11VA 和 gdigrab 绑定 Windows，迁移到 VAAPI/x11grab 可支持 Linux

## 六、项目地址

GitHub: 待补充

---

> 完整开发过程（从需求 → spec → TDD 任务拆分 → 逐条执行 → 代码审查 → 提交）全部通过 Claude Code 辅助完成。关于 AI 辅助工程决策的工作流见项目文档。
