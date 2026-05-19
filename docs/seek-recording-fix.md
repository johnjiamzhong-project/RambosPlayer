# Seek 期间推流/录制内容错乱问题修复说明

## 问题背景

RambosPlayer 支持在播放同时将内容推流到本地文件（LocalRecorder）或远程地址（MuxThread）。当用户在播放过程中执行 seek 操作时，推流输出的文件出现以下异常现象：

- 录制文件的前几秒视频内容丢失，播放器直接跳到 seek 目标附近才有画面
- seek 目标附近约 10 秒区间内画面花屏、帧丢失
- 花屏结束后画面恢复正常

远程推流（RTMP/SRT）侧，seek 后输出流的 PTS 从 0 重新计算，接收端播放器看到时间轴回跳，表现为卡顿或画面重置。

---

## 根本原因

### H.264 GOP 结构与 seek 的错位

H.264 视频中，P 帧和 B 帧依赖同一 GOP 内的 I 帧（关键帧）才能解码。`av_seek_frame` 使用 `AVSEEK_FLAG_BACKWARD` 时，必须落在目标时间点之前的最近关键帧（例如目标 15s，实际落点 12s）。

播放器的处理方式是：解码 12s–15s 的帧用于建立解码器参考状态，但只显示 15s 之后的帧。录制器挂载在 DemuxThread 层，拿到的是解复用器读出的原始数据，无法区分"解码器热身帧"与"真正要输出的帧"，因此把 12s–15s 的 GOP 重叠内容全部写入了录制文件。

### 原 warmup+IDR 方案的致命缺陷（LocalRecorder）

早期版本试图通过"预热编码"解决 GOP 重叠：seek 后先软解码 12s–15s 但不写入，等帧 PTS 到达目标后用 libx264 重新编码一个 IDR 帧写入，之后恢复 `-c copy` 模式。

这个方案有一个不可调和的矛盾：**IDR 帧由 libx264 编码（SPS/PPS 来自 libx264），后续帧从源文件 copy（SPS/PPS 来自原始编码器）**，两段码流参数不兼容。解码器在 IDR 处用 libx264 的参数重置，却收到原始编码器的 P 帧，无法解码，导致花屏。"前几秒丢失"则是因为部分播放器遇到破损 IDR 后，从该 IDR 位置重新定位，忽略了 IDR 之前已经合法写入的内容。

### MuxThread 的 PTS 归零问题

MuxThread（网络推流）在收到 seek sentinel（nullptr）后，将 `videoPtsBase_` 和 `audioPtsBase_` 重置为 `AV_NOPTS_VALUE`，下一个包的 PTS 成为新的基准，输出从 0 重新计时。对远端接收者而言，时间轴从 5s 突然跳回 0s，播放器触发重置；对本地 FLV 文件而言，`av_interleaved_write_frame` 收到 DTS 倒退的帧会乱序或静默丢帧，造成文件损坏。

---

## 修复方案

### 核心思路

放弃 warmup+IDR 重编码，改为**关键帧抑制期**：seek 后录制器/推流线程进入抑制状态，丢弃 PTS 小于 seek 目标的所有帧，在首个 PTS ≥ 目标且为关键帧（`AV_PKT_FLAG_KEY`）的帧处退出抑制，从该帧起继续 `-c copy` 写入。由于 `av_seek_frame(BACKWARD)` 保证关键帧后续帧的参考链完整，此帧写入后 H.264 码流全程自洽，不再需要重编码。

音频单独对齐：视频关键帧确定落点后，以该帧的源 PTS（秒）作为音频的截止点，丢弃早于此点的音频包，保证 A/V 内容同步。

MuxThread 的 PTS 连续性问题同步修复：sentinel 到来时不再归零，而是将上一段最后写出的 PTS 存为累积偏移，新段公式改为 `outPts = srcPts - segBase + accumPts`，输出时间轴跨 seek 保持连续。

---

## 关键改动说明

### 模块一：LocalRecorder（`src/localrecorder.h` / `src/localrecorder.cpp`）

**删除的内容**

- `LocalRecorder::initVideoCodec()` — 初始化 libx264 解码器+编码器，seek 后重编码 IDR 用，现已无用
- `LocalRecorder::cleanupVideoCodec()` — 释放上述编解码器资源
- `LocalRecorder::writeVideoWarmup()` — 预热解码路径：解码 12s–15s 帧，到目标帧时编码 IDR 写入，是花屏的直接来源
- 相关成员变量 `vDecCtx_`、`vEncCtx_`、`vDecFrame_`、`warmingUp_`、`targetSec_`

**新增的内容**

- 成员变量 `suppressVideoUntilSec_`（double）：seek 后视频抑制期的目标秒数，`-1.0` 表示未激活
- 成员变量 `suppressAudioUntilSec_`（double）：音频抑制截止秒数，`1e18` 表示等待视频关键帧落点，`-1.0` 表示未激活

**修改的函数**

- `LocalRecorder::resetPtsBase(double fromSourceSec, double targetSec)`：恢复 `targetSec` 参数，调用时将 `suppressVideoUntilSec_` 设为 `targetSec`，将 `suppressAudioUntilSec_` 设为 `1e18`（无限抑制，等待视频）
- `LocalRecorder::writeVideoCopy(AVPacket* pkt)`：在函数入口处检查 `suppressVideoUntilSec_`；若帧 PTS 小于目标或不是关键帧则直接丢弃（`return true`）；找到满足条件的关键帧后将 `suppressVideoUntilSec_` 清零，并将该关键帧的源 PTS 写入 `suppressAudioUntilSec_` 作为音频对齐点
- `LocalRecorder::writeAudioPacket(AVPacket* pkt)`：在函数入口处检查 `suppressAudioUntilSec_`；若为 `1e18`（视频未落点）或源 PTS 小于截止点则丢弃；到达截止点后清零，之后正常写入

---

### 模块二：MuxThread（`src/muxthread.h` / `src/muxthread.cpp`）

**PTS 连续性修复**

- 删除成员变量 `videoPtsBase_`、`audioPtsBase_`（单次归零基准）
- 新增成员变量 `videoSegBase_`、`audioSegBase_`（当前段起始源 PTS）、`videoAccumPts_`、`audioAccumPts_`（上段末尾输出 PTS）、`videoLastOut_`、`audioLastOut_`（本段最新写出 PTS）
- `MuxThread::run()` sentinel 处理：原来执行 `videoPtsBase_ = AV_NOPTS_VALUE`（归零），改为 `videoAccumPts_ = videoLastOut_; videoSegBase_ = AV_NOPTS_VALUE`（续接）
- `MuxThread::run()` PTS 计算：原来执行 `pkt->pts -= videoPtsBase_`，改为 `pkt->pts = pkt->pts - videoSegBase_ + videoAccumPts_`

**关键帧抑制期**

- 新增成员变量 `suppressVideoUntilSec_`（`std::atomic<double>`）、`suppressAudioUntilSec_`（`std::atomic<double>`）
- 新增方法 `MuxThread::setSuppressUntilKeyframe(double targetSec)`：将 `suppressVideoUntilSec_` 设为目标秒数，将 `suppressAudioUntilSec_` 设为 `1e18`
- `MuxThread::run()` 视频分支：在 `startSec_` 过滤之后、PTS 调整之前，检查 `suppressVideoUntilSec_`；不满足条件（PTS 不足或非关键帧）则 `av_packet_free` 并 `continue`；找到关键帧后将源 PTS 写入 `suppressAudioUntilSec_`
- `MuxThread::run()` 音频分支：在 PTS 调整之前，检查 `suppressAudioUntilSec_`；PTS 未追上则丢弃

---

### 模块三：DemuxThread（`src/demuxthread.h` / `src/demuxthread.cpp`）

- 新增成员变量 `muxThreads_`（`std::vector<MuxThread*>`）：持有所有已注册的网络推流线程指针
- 新增方法 `DemuxThread::addMuxThread(MuxThread* m)`：线程安全地将 MuxThread 加入列表
- `DemuxThread::clearRestreamQueues()`：在清空队列和录制器列表时，同步清空 `muxThreads_`
- `DemuxThread::handleSeek()`：在向视频队列推 sentinel 之前，遍历 `muxThreads_` 调用 `m->setSuppressUntilKeyframe(target)`；遍历 `localRecorders_` 调用 `r->resetPtsBase(fromPos, target)`，恢复传入 `target` 参数

---

### 模块四：PlayerController（`src/playercontroller.h`）

- 新增方法 `PlayerController::addMuxThread(MuxThread* m)`：转发给 `demux_.addMuxThread(m)`，与已有的 `addLocalRecorder`、`addRestreamVideoPacketQueue` 形成统一的推流注册接口

---

### 模块五：StreamController（`src/streamcontroller.h`）

- 新增只读访问器 `StreamController::muxThreads()`：返回 `muxThreads_` 的 const 引用，供调用方将 MuxThread 注册到 DemuxThread

---

### 模块六：MainWindow（`src/mainwindow.cpp`）

两处推流接线代码（初始启动 `startStreaming` 和暂停恢复 `reconnectStreaming`）均补充调用：

```
player_->addMuxThread(streamCtrl_->muxThreads()[remoteIdx].get());
```

与已有的 `addRestreamVideoPacketQueue` / `addRestreamAudioPacketQueue` 并列，完成 seek 抑制通知链路的最后一环。

---

## 修复后的效果

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 本地录制 + seek | 前几秒内容丢失，GOP 重叠帧花屏约 10s | 从 seek 目标最近关键帧处无缝续写，无花屏 |
| 网络推流 + seek | PTS 归零，接收端时间轴回跳，画面卡顿 | PTS 连续累积，接收端时间轴平滑 |
| GOP 重叠内容 | 写入录制文件（用户未看到的内容出现在录制中） | 静默丢弃，录制内容与播放内容一致 |

## 已知约束

seek 目标恰好不在关键帧上时，录制/推流从下一个关键帧开始，与 seek 目标的偏差最多为一个 GOP 长度（通常 1–4 秒）。这是 `-c copy` 模式的固有限制：若要精确到帧，需对 seek 点之后的全部帧进行转码，代价是显著的 CPU 开销。
