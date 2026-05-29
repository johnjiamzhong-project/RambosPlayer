# 导出管线架构与优化策略分析

> 版本: v1.0 | 日期: 2026-05-29 | 项目: RambosPlayer

---

## 一、现有架构

### 1.1 整体数据流

```
                          ExportWorker (QThread)

  [磁盘IO] --> [Demuxer] --> [Decoder] --> [Encoder] --> [Muxer] --> [磁盘IO]
  (源文件)    av_read_frame  vDecCtx/aDec  vEncCtx/aEnc  av_interleaved  (输出MP4)
                                  |              |         _write_frame
                                  |              |
                            AVPacket        AVFrame          AVPacket
                                  |              |
                            [像素格式转换]   [音频重采样]
                            sws_scale       swr_convert
                            (YUV420P跳过)
```

### 1.2 关键代码路径

```
mainwindow.cpp                    exportworker.cpp
-----------------                 -----------------
onExportTriggered()               run()
  +-- QInputDialog (名称)           +-- 1. avformat_open_input
  +-- QFileDialog (保存路径)        +-- 2. 创建解码器 (vDecCtx, aDecCtx)
  +-- exportTimer_.start()          +-- 3. 创建输出上下文 (MP4)
  +-- exportWorker_->run(...) --->  +-- 4. 创建编码器 (vEncCtx, aEncCtx)
  |                                 +-- 5. sws_getContext / swr_alloc
  |                                 +-- 6. avio_open + write_header
onExportProgress() <--progressed--  +-- 7. av_seek_frame -> inPts_
  |                                 +-- 8. 主循环:
  |                                 |     while (av_read_frame) {
  |                                 |       if (video packet):
  |                                 |         avcodec_send_packet(vDec)
  |                                 |         while (avcodec_receive_frame):
  |                                 |           if (framePts in [in,out]):
  |                                 |             sws_scale (if needed)
  |                                 |             avcodec_send_frame(vEnc)
  |                                 |             while (avcodec_receive_packet):
  |                                 |               av_interleaved_write_frame
  |                                 |       if (audio packet):
  |                                 |         ... (same pattern)
  |                                 |     }
onExportFinished() <--finished---  +-- 9. EOS flush encoders
  +-- writeExportLog()             +-- 10. av_write_trailer
  +-- statusBar                    +-- 11. cleanup_all (goto)
```

### 1.3 编码器配置

| 参数 | libx264 (CPU) | h264_nvenc (GPU) |
|------|---------------|-------------------|
| preset | `superfast` | `p1` |
| 质量控制 | CRF 17 | CQP 18 |
| profile | `high` | -- |
| GOP | 2 秒 | 2 秒 |
| B 帧 | 0 | 0 |
| 像素格式 | YUV420P | YUV420P |
| 线程 | `auto` | -- |

音频: AAC-LC 192kbps, FLTP 采样格式

### 1.4 当前已实施的优化

| # | 优化项 | 效果 |
|---|--------|------|
| 1 | YUV420P 源帧跳过 sws_scale | 大多数视频省全帧拷贝+转换 |
| 2 | 编码包 encPkt 循环外一次 alloc 复用 | 消除 O(N帧) malloc/free |
| 3 | superfast 替代 medium/veryfast preset | ~3-4x 编码提速 |
| 4 | CRF 17 补偿 preset 画质损失 | 码率增10-15%, 视觉无损 |
| 5 | SWS_FAST_BILINEAR 替代 BICUBIC | ~2x 缩放提速 |
| 6 | NVENC p1 替代 p4 | ~1.5x GPU编码提速 |
| 7 | threads auto 显式声明 | 充分利用多核 |

---

## 二、瓶颈分析

### 2.1 瓶颈分布 (1080p 30fps 60s 视频, libx264 superfast)

```
解码:       ████████████░░░░░░░░░░  25%
sws:        ██░░░░░░░░░░░░░░░░░░░░   5% (YUV420P源则0%)
编码:       ████████████████████░░░  40%
封装写入:   ████░░░░░░░░░░░░░░░░░░░   8%
IO等待:     ██████░░░░░░░░░░░░░░░░░  12%
其他:       ██████░░░░░░░░░░░░░░░░░  10%
-------------------------------------------------
总计: ~1.7x 实时 (60s视频约35s导出)
```

### 2.2 核心瓶颈

| 瓶颈 | 根因 | 影响 |
|------|------|------|
| **串行管线** | decode->encode 严格先后，无法重叠 | CPU利用率 <60% |
| **批量重建** | 每段重新open/init源文件和解码器 | N段浪费(N-1)*3s |
| **IO阻塞** | av_read_frame可能阻塞在磁盘 | 编码器饥饿等待 |

### 2.3 批量导出现状 (关键问题)

```
Segment 0: open源(1s) -> 分析流(2s) -> init解码器(0.5s) -> seek+编码(10s) -> close
Segment 1: open源(1s) -> 分析流(2s) -> init解码器(0.5s) -> seek+编码(8s)  -> close
Segment 2: open源(1s) -> 分析流(2s) -> init解码器(0.5s) -> seek+编码(12s) -> close
...
----------------------------------------------------------------------------
重复开销: 3.5s * (N-1) = 14s (N=5时)
有效编码: 30s
总耗时:   44s  (浪费32%在重复初始化)
```

---

## 三、优化策略

### 策略总览

```
                    难度
                     |
                P3 * |  * P1
             (硬件解码)| (帧流水线)
                     |
          P2 *        |        * P0
     (音视频分离)      |     (批量复用)
                     |
  P5 *                |
(av_write)            |
  P4 *                |
(限制探测)            |
  P6 *                |
(进度频率)            |
                     |
  -----------------------------------> 收益
```

---

### P0 -- 批量导出复用文件/解码器上下文 ✅ 已实施

**问题**: 批量导出N段时，ExportWorker每段启动新QThread会话，重复: 打开源文件 -> 分析流 -> 创建解码器 -> 创建编码器。

**方案**: 一次线程运行处理多段（`runBatch` + `processSegment` 分层架构）

```
当前 (每段独立线程):
  run() { open -> init -> encode -> close }  * N次

优化后 (一次线程处理多段):
  run() {
    open -> init (一次完成)
    for each segment:
      seek -> encode -> flush -> close_output_file
    close (一次完成)
  }
```

**实现要点**:
1. ExportWorker 新增 `QList<ExportSegment>` 批量任务列表
2. run() 内: 打开源文件和解码器一次
3. 每段开始前 av_seek_frame + avcodec_flush_buffers
4. 每段创建新输出文件和新编码器 (或flush复用)
5. 所有段完成后统一关闭

**预期收益**:

| 场景 | 当前耗时 | 优化后 | 节省 |
|------|---------|--------|------|
| 单段60s视频 | ~35s | ~32s | 6% |
| 5段批量 | ~175s | ~130s | 25% |
| 10段批量 | ~350s | ~240s | 31% |

**风险**: 中。编码器跨段复用需验证flush后状态正确。

---

### P1 -- 解码/编码帧流水线 ❌ 已测试，已回退

> **实测结果**: 5.2-5.3×，比 P0 基准 6.0× 慢 ~13%。
> std::thread 创建/销毁 + QMutex + av_frame_clone 开销超过并行收益。
> 代码已回退，保留在 git 历史中备查。

**问题**: 解码和编码严格串行。解码一帧时编码器空闲，编码时解码器空闲。

**方案**: 解码与编码间插入3-5帧缓冲队列，生产者-消费者模式:

```
                  FrameQueue<AVFrame*> (容量3~5)
                  |                            |
av_read_frame -> vDecCtx -> [buf] -> sws -> vEncCtx -> mux
(生产者)              (队列)        (消费者)
```

**实现要点**:
1. 复用现有 FrameQueue<AVFrame*> 模板类
2. 主循环: 读包 -> 解码 -> 入队 (非阻塞)
3. 编码循环: 出队 -> 转换 -> 编码 -> 封装
4. sentinel机制: 编码完成后入队nullptr通知退出

**两种实现**:

| 方式 | 描述 | 复杂度 | 效果 |
|------|------|--------|------|
| A: 协程式 | 同线程交替执行 | 低 | 10-15% |
| B: 双线程 | decode线程 + encode线程 | 中 | 20-40% |

**预期收益**: B方式在4核以上CPU达20-40%。

**风险**: 中。需处理帧生命周期 (av_frame_ref/unref)。

---

### P2 -- 音视频分离处理 ❌ 已测试，已回退

> **实测结果**: 5.4×，与 P0 基准无差异。单线程内调度重排不改变 CPU 总负载。
> 代码已回退。

**问题**: 音视频在主循环交错处理，互相阻塞。视频解码while循环产出多帧时，音频包堆积。

**方案**: 双队列 + 独立编码:

```
av_read_frame --> [video?] --> vDecCtx --> [vFrameQ] --> vEncCtx --> mux
               `-> [audio?] --> aDecCtx --> [aFrameQ] --> aEncCtx -'
```

**实现要点**:
1. 读包后按流类型分发
2. 视频帧和音频帧各自独立编码+mux
3. av_interleaved_write_frame确保MP4内正确交织

**预期收益**: 10-15%

**风险**: 低。音视频独立，只需保证最终DTS顺序。

---

### P3 -- GPU 硬解码 (CUDA/NVDEC) ✅ 已实施

> **实测结果**: 5.2-6.3×，波动大。CUDA 硬解依赖 NVIDIA 驱动，无 GPU 时自动回退 CPU 路径。
> `av_hwdevice_ctx_create(CUDA)` → 解码器绑定 → `av_hwframe_transfer_data` GPU→CPU → libx264 编码。
> NVENC 零拷贝未实施（需验证 CUDA 环境后添加）。

**问题**: CPU解码 -> 系统内存 -> GPU编码(NVENC)，有一次PCIe数据搬运。

**方案**: GPU硬解码，帧留在显存，NVENC零拷贝:

```
磁盘 --> GPU硬解码(NVDEC) --> GPU显存Frame --> NVENC编码 --> 磁盘
                                  ^ 零拷贝路径
```

**实现要点**:
1. av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_CUDA)
2. 解码器设置 hw_device_ctx 和 hw_frames_ctx
3. NVENC编码器设置 hw_frames_ctx 指向同一显存帧池
4. 跳过sws_scale (需GPU端scale_cuda滤镜)

**预期收益**: NVENC用户30-50%，纯CPU用户无收益。

**风险**: 高。平台差异大 (Win: D3D11VA, Linux: VAAPI/VDPAU)，需平台适配。

---

### P4 -- 限制 avformat_find_stream_info 探测范围 ✅ 已实施

**问题**: avformat_find_stream_info默认分析大量数据 (5MB probe, 5s duration)，耗时1-3秒。

**方案**: 限制探测参数:

```cpp
inCtx->probesize = 1 << 20;                      // 1MB (默认5MB)
inCtx->max_analyze_duration = 2 * AV_TIME_BASE;  // 2秒 (默认5秒)
```

**预期收益**: 每次导出节省1-2秒启动时间。

**风险**: 极低。标准MP4/H.264文件2秒足够检测所有流参数。

---

### P5 -- av_write_frame 替代 av_interleaved_write_frame ✅ 已实施

**问题**: av_interleaved_write_frame维护排序缓冲，但我们max_b_frames=0 (无B帧)，PTS严格递增，不需重排。

**方案**: 直接用av_write_frame。

**前提**: max_b_frames=0 (无B帧) / PTS严格递增 / 音视频按DTS顺序写入

**预期收益**: 3-5%封装层开销节省。

**风险**: 极低。

---

### P6 -- 进度信号频率优化 ✅ 已实施

**问题**: 每500ms emit信号跨线程 (QueuedConnection)。

**方案**: 改为每秒或每30帧报告一次。

**预期收益**: 边际1-2%，主要减UI线程负担。

---

## 四、实施路线图

### 第一阶段: 低风险快速优化 ✅ 已完成

```
P4: 限制探测范围       ✅ probesize=1MB, analyzedur=2s
P5: av_write_frame     ✅ 4处替换
P6: 进度频率调整        ✅ 500ms → 1000ms
```

### 第二阶段: 核心架构优化 ✅ 已完成

```
P0: 批量复用文件/解码器  ✅ runBatch + processSegment 分层
  单段实测: 6.0× 实时（与优化前持平，批量场景收益待测）
```

### 第三阶段: 深度管线优化 ❌ 测试后回退

```
P1: 解码/编码帧流水线    ❌ 5.2-5.3×, 线程开销 > 收益
P2: 音视频分离处理       ❌ 5.4×, 无差异
  结论: libx264 superfast 已触及 CPU 单线程编码天花板
```

### 第四阶段: 硬件加速 ✅ 已实施（部分）

```
P3: GPU硬解码(CUDA/NVDEC) ✅ 有CUDA则用，无则回退CPU
  NVENC零拷贝: 待 CUDA 环境确认后添加
  实测: 5.2-6.3×（波动大，依赖GPU环境）
```

---

## 五、涉及文件

| 文件 | 当前行数 | 涉及策略 |
|------|---------|---------|
| src/exportworker.h | 38 | P0, P1, P3 |
| src/exportworker.cpp | 461 | 全部 |
| src/mainwindow.cpp (导出部分) | ~170 | P0, P6 |
| src/mainwindow.h (导出成员) | ~15 | P0 |
| src/framequeue.h | 已有 | P1 (复用) |

---

## 六、性能基准参考

> 测试环境: i7-8700, GTX 1060, 16GB, Win10, MSVC 2017 Release
> 测试素材: 1920×1080 H.264 25fps 电影 (1h33m)
> 编码器: libx264 superfast CRF 17
> 详见 [EXPORT_BENCHMARK.md](EXPORT_BENCHMARK.md)

| 配置阶段 | 速度 | 状态 |
|---------|------|:----:|
| 原始 (medium+BICUBIC+每帧alloc, -c copy) | 0.67× | 已废弃 |
| + P4+P5+P6+P0 (当前最终版本) | **6.0×** | ✅ |
| + P1+P2 (线程流水线/音频优先) | 5.4× | ❌ 已回退 |
| + P3 (GPU硬解码CUDA) | 5.6× | ✅ 有CUDA则生效 |

> **结论**: CPU 编码在 libx264 superfast CRF17 下天花板约 6.0× 实时。
> 进一步提速需 NVENC 零拷贝（需 NVIDIA GPU + CUDA 驱动）。
