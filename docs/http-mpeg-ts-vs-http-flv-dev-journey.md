# HTTP-MPEG-TS 低延迟推流开发实战：踩坑记录与 FLV 对比

## 一、背景

最近在做一款多媒体播放器（RambosPlayer），它不仅支持本地播放，还内置了推流服务器，能把正在播放的视频实时推到局域网内的浏览器/平板上观看。推流方案经历了两个阶段：

1. **HTTP-FLV**（早期方案）：基于 flv.js + FFmpeg FLV muxer，延迟低但兼容性细节多
2. **HTTP-MPEG-TS**（后期方案）：基于 mpegts.js + FFmpeg MPEG-TS muxer，为了解决 FLV 方案的一些痛点

本文记录在实现 HTTP-MPEG-TS 推流过程中遇到的一系列问题和解决方案，并在最后对两种协议做客观对比。

---

## 二、两种协议简介

### HTTP-FLV

FLV 是 Adobe 制定的容器格式，HTTP-FLV 就是通过 HTTP 分块传输（chunked transfer）将 FLV 数据流式推送给浏览器。flv.js 用 MediaSource Extensions（MSE）在浏览器端解复用并喂给 `<video>` 元素。

优点：
- flv.js 成熟稳定，社区庞大
- 延迟极低（200-500ms）

缺点：
- FLV 容器只支持 H.264/AAC 编码（MPEG-4 Part 2、VP8 等不兼容）
- 必须按 DTS 顺序写入，B 帧处理稍有复杂度
- 晚连客户端需要特殊处理 `flvHeader_` + `currentGopBytes_` 才能避免白屏

### HTTP-MPEG-TS

MPEG-TS 是广播电视领域广泛使用的传输流格式（.ts 文件），通过 HTTP 分块传输给浏览器。mpegts.js 与 flv.js 同源（同一作者开发），使用相同的 MSE 技术栈。

优点：
- 天然支持 H.264/H.265/AAC/MP3/AC-3 等多种编码
- TS 内部周期性重发 PAT/PMT，新客户端可任意时刻加入
- 适合广播电视级的多路复用场景

缺点：
- mpegts.js 社区相对较小
- MSE 缓冲管理需要更精细的参数调整
- 每次 seek 重连后 SourceBuffer 需要重建

---

## 三、HTTP-MPEG-TS 开发中解决的关键问题

### 问题 1：AAC 编码器 seek 后 PTS 污染 → `audio write error`

**现象**：快进后服务端日志刷出大量 `audio write error: Invalid argument`，浏览器端声音停止。

**根因**：FFmpeg 原生 AAC 编码器有一个约 2048 个采样点的内部 lookahead 缓冲。seek 后调用 `avcodec_flush_buffers()` 无法清除这个缓冲，旧缓存帧的 PTS 从 0 重新开始计数。MpegTsServer 的 PTS remap 逻辑收到这些旧帧后，计算的 `pts = pts - audioSegBase_ + audioAccumPts_` 变成负数，AVIO 写入时报 `Invalid argument`。

**解决**：仿照 libx264 的处理方式，实现 `AudioEncodeThread::reopenCodec()`：收到 seek sentinel 后，销毁整个 `AVCodecContext`，重新创建并打开编码器。AAC 编码器重建后的 extradata 与之前一致（使用相同参数），不会影响 MPEG-TS 的 PMT 信息。

```cpp
bool AudioEncodeThread::reopenCodec() {
    avcodec_free_context(&codecCtx_);
    codecCtx_ = nullptr;
    // ... 重新分配、设置参数、avcodec_open2 ...
    return true;
}
```

**教训**：不是所有编码器的 `avcodec_flush_buffers` 都可靠。libx264 flush 后会进入 EOS 模式再也收不了帧，AAC 编码器 flush 后残留 lookahead 数据。遇到这类编码器，必须在 seek 时重建上下文。

---

### 问题 2：多次 seek 后浏览器延迟逐步升高

**现象**：多次快进后，平板浏览器的延迟越来越高，从几百毫秒逐渐累积到数秒。日志显示同一 IP 在多次重连后 `streamClients_` 中的 socket 数量从 1 增长到 3。

**根因**：
1. seek 后 H.264 解码预滚会产生目标点前的 GOP 重叠数据，旧浏览器连接继续接收这些数据并写入 MSE SourceBuffer
2. 每次新连接 `/stream.ts` 时旧 socket 未被主动替换，导致同一个浏览器持有多个 socket 连接，服务端向所有旧 socket 广播数据，造成带宽和缓冲双重浪费
3. `segmentBytes_`（最近一个 GOP 的 TS 数据）在 seek 后仍包含历史位置的数据，晚连客户端先消费这些旧数据，形成固定延迟

**解决**：
1. seek sentinel 到达 MpegTsServer 时，主动 `abort()` 所有现有 TS 连接，让浏览器重连重建 SourceBuffer
2. 新增 `disconnectClientsFromPeer()`：新连接 `/stream.ts` 时，查找并替换同一 IP 的旧 socket，彻底消除同 IP 多连接残留
3. 晚连客户端不再发送 `segmentBytes_` 历史 GOP，只接收实时广播数据，靠 TS 流周期性自带的 PAT/PMT 自然起播
4. `broadcastData()` 中检测 `bytesToWrite()` > 1MB 的慢客户端，主动断开防止拖累整条链路

---

### 问题 3：快进后重连风暴触发 QList 迭代器断言

**现象**：按右方向键快进后，浏览器连续请求 `/stream.ts`，随后程序崩溃。日志显示断言错误：`QList::erase` — `The specified iterator argument 'it' is invalid`。

**根因**（涉及两个 bug 叠加）：

**Bug A — 迭代器失效**：`disconnectClientsFromPeer()` 使用 `QMutableListIterator` 遍历 `streamClients_`，遍历过程中调用了 `QTcpSocket::disconnectFromHost()`。断开信号可能**同步**进入 `removeClient()` 修改同一个 `QList`，导致当前迭代器失效。

**Bug B — 重连风暴**：问题 2 的修复中，HTML 播放页的 `ended` 事件监听添加了 `stalled` / `emptied` / `abort` 事件快速重连。但 `destroy()` / `unload()` 过程本身也可能触发这些事件，造成 300ms 间隔的连续重连风暴。

**解决**：
1. 替换旧客户端时，先循环**收集**所有待移除的 socket 指针，再统一从 QList 中移除，最后集中 `disconnectFromHost()` — 彻底消除遍历时修改 QList
2. 撤销 `stalled` / `emptied` / `abort` 事件重连，只保留 mpegts.js 的 `ERROR` 回调

**教训**：Qt 的信号-槽是同步的，`disconnectFromHost()` 可能同步触发 `disconnected` 信号 → `removeClient()` → 修改 `QList`。遍历 STL/Qt 容器时严禁增删元素。

---

### 问题 4：seek 后先推送目标前旧画面

**现象**：快进 10 秒（从 25s 到 35s），浏览器端先播放 seek 之前的画面（约 30s），几秒后才切到 35s 的新画面。日志显示 `EncodeThread: first frame after sentinel pts=2700000`（约 30s），随后有 115 帧 pre-target 帧。

**根因**：DemuxThread 的 seek 为了保证 H.264 参考帧链完整，会从目标点**前的关键帧**（约 30s 处的 IDR）开始推送视频包。StreamDecoder 忠实地解码了所有这些包，但 30s~35s 这段预滚区的解码帧本应丢弃等待新位置帧，它们却直接进入了 EncodeThread 重编码，导致服务端先推送 5 秒旧画面。

**解决**：
1. `StreamDecoder` 新增 `minOutputPts_` 原子门控：解码过程中如果 `frame->pts < minOutputPts_` 就丢弃该帧，只完成解码预滚不输出
2. `StreamPipeline` 暴露 `setSeekTargetSeconds()` 接口，供 UI 层在 seek 前设置目标 PTS
3. `MainWindow` 中进度条拖动和左右方向键触发 seek 前，先调用 `prepareMpegTsSeek()` 通知所有 StreamPipeline
4. HTML 播放页新增 `clearVideo()`：重连前销毁 player、清空 `<video>` 的 `src` 并 `load()`，确保 MSE 和视频缓冲全部清空

---

### 问题 5：开局闪白

**现象**：推流页面打开时，浏览器先闪烁一下白色，然后才显示视频画面。

**根因**：`VideoRenderer` 初始化 `QImage(width, height, Format_RGB32)` 时分配了未初始化的内存，`paintEvent` 在第一个有效帧到达前将其绘制到屏幕，内容为随机的灰色/白色。

**解决**：初始化后立即 `currentFrame_.fill(Qt::black)`。

---

### 问题 6：快进提示误用"流结束"

**现象**：快进后浏览器显示"流结束，重连..."，让用户误以为直播已经结束。

**根因**：HTML 播放页的 `ended` 事件监听和定时器检测都使用"流结束"文案。但在这个直播场景中，`ended` 事件只会在 seek 断连时触发（直播流不会自己结束）。

**解决**：将 `ended` 事件处理文案改为"快进中，重连..."，与真正网络断开的 "断开...正在重连..." 和画面卡住的 "画面卡住，重连..." 自然区分。

---

## 四、HTTP-MPEG-TS vs HTTP-FLV 对比

| 对比维度 | HTTP-FLV | HTTP-MPEG-TS |
|---------|----------|-------------|
| **编码兼容性** | 仅 H.264 + AAC | H.264/H.265 + AAC/MP3/AC-3 |
| **浏览器库** | flv.js（成熟稳定） | mpegts.js（同作者，较新） |
| **延迟** | 200-500ms | 200-500ms，无本质差别 |
| **晚连起播** | 需 `codecConfigHeader_` + `currentGopBytes_` | 靠 PAT/PMT 周期重发自然起播 |
| **seek 后重建** | SourceBuffer 可 reuse | 推荐 destroy 重建 SourceBuffer |
| **PAT/PMT 机制** | 无（FLV header 一次性发送） | 周期性发送，新客户端任意时刻可加入 |
| **B 帧处理** | 必须按 DTS 写入 | 可 PTS 顺序写入 |
| **服务端复杂度** | 较低（FLV 是简单的 tag 序列） | 中等（TS 有 PAT/PMT/PES 分层） |
| **社区资源** | 丰富 | 较少 |

### 延迟性能

实测两种方案的端到端延迟都在 **200-500ms** 范围内，没有本质差异。影响延迟的主要因素是：
- mpegts.js/flv.js 的 `liveBufferLatencyChasing` 配置
- 浏览器 MSE 缓冲大小
- 网络 RTT

### 晚连场景（客户端在推流开始后打开页面）

- **HTTP-FLV**：需要服务器端维护 `codecConfigHeader_`（FLV 头）和 `currentGopBytes_`（最近一个 GOP 数据），客户端连接时补发。代码实现相对复杂。
- **HTTP-MPEG-TS**：MPEG-TS muxer 会周期性（约每 0.5s）自动写入 PAT/PMT，新客户端只需接入实时数据流即可，服务端无需额外缓存逻辑。

### Seek 场景

- **HTTP-FLV**：seek 后 SourceBuffer 可以继续复用，只需从新位置开始推送数据。
- **HTTP-MPEG-TS**：seek 后推荐断开重建 SourceBuffer，因为 mpegts.js 内部 PTS 连续性假设较强，seek 造成的 PTS 跳变可能导致浏览器端解码异常。但断开重连意味着短暂的黑屏和缓冲重建时间（约 300-800ms）。

---

## 五、选择建议

### 什么时候选 HTTP-MPEG-TS？

- 需要支持 H.265/HEVC 编码（如 4K 视频推流）
- 希望晚连客户端逻辑简单（靠 PAT/PMT 自动适配）
- 接受 seek 时短暂黑屏重建 SourceBuffer
- 目标浏览器为 Chrome/Edge/Firefox 最新版本

### 什么时候选 HTTP-FLV？

- 需要兼容更广泛的浏览器（包括低版本）
- 希望 seek 时无缝切换（不断开 SourceBuffer）
- FLV 社区更大，遇到问题更容易找到解决方案
- 编码限定 H.264，不需要 HEVC

### 实际项目中的方案

在 RambosPlayer 中，两种方案目前**共存**：HTTP-FLV 作为成熟稳定的默认选择，HTTP-MPEG-TS 作为技术前瞻选项。用户可以在推流配置界面选择目标协议。

---

## 六、技术总结

在这次 HTTP-MPEG-TS 推流开发中，踩过的坑可以归纳为几类：

1. **FFmpeg 编码器状态管理**：libx264 和 AAC 编码器的 `avcodec_flush_buffers` 都不可靠，必须重建上下文。这是 FFmpeg 编码器实现的历史遗留问题。

2. **多线程数据流同步**：seek 操作需要在整个管线（DemuxThread → StreamDecoder → EncodeThread → MpegTsServer）中同步传播。使用 **nullptr sentinel** 模式统一处理所有下游线程的状态重置。

3. **PTS 续接算法**：重编码场景下，编码器从 0 开始计数 PTS，但 TS 流要求 PTS 单调递增。通过 `pts - segBase_ + accumPts_` 的 remap 算法，实现 seek 前后流时间的自然衔接。

4. **浏览器 MSE 行为**：`ended` 事件、`stalled`、`emptied` 的触发时机与直播场景不完全匹配。需要根据实际行为调整重连策略和文案。

5. **Qt 线程安全**：`QList` 遍历中不能修改集合，`QTcpSocket` 的断开操作可能同步触发信号回调。这类问题在桌面应用中容易忽略但后果严重（crash）。

---

希望这篇记录对做类似推流方案的同学有帮助。如果文中有任何错误或遗漏，欢迎指正。

*项目地址：https://github.com/rambos/RambosPlayer（需自备 Qt 5.14 + FFmpeg 4.x 编译环境）*
