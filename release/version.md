# RambosPlayer 版本发布记录

> 每次发布新版本时，按格式追加到此文件，再创建 GitHub Release。

---

## 版本格式说明

- **版本号**：遵循 [SemVer](https://semver.org/lang/zh-CN/)（主版本.次版本.补丁）
- **Release title**：GitHub Releases 的标题，简短描述本次发布
- **Release notes**：GitHub Releases 的正文，记录新增功能、修复、变更

---

## 发布流程

1. 确认当前代码状态，确定版本号
2. 追加版本记录到本文件
3. 提交包含本文件的 commit 并打 tag
4. `.\package.ps1 -Version "x.x.x"` 打包
5. 在 GitHub 创建 Release，填写 title 和 notes

详见 `RELEASE-GUIDE.md`。

---

## 版本历史

| 版本 | 日期 | Release Title | 概要 |
|------|------|---------------|------|
| 1.2.4 | 2026-06-18 | RambosPlayer v1.2.4 | 修复窗口最小化/被遮挡恢复后延迟累积，重连耗时从 ~5.6s 降至 ~0.6s |
| 1.2.3 | 2026-06-16 | RambosPlayer v1.2.3 | 修复 RTMP 纯视频直播流卡顿、6s 连接延迟与长时间延迟漂移 |
| 1.2.2 | 2026-06-15 | RambosPlayer v1.2.2 | 新增拉流播放（RTMP/RTSP/HTTP-FLV/SRT），异步探测 + 自动重连 + 码率帧率统计 |
| 1.2.1 | 2026-06-04 | RambosPlayer v1.2.1 | 修复浏览剪辑切换多段剪辑时区间残留导致误导出 |
| 1.2.0 | 2026-05-29 | RambosPlayer v1.2.0 | 新增多段剪切、视频合并、音频混合模块，最大化浮动标题栏，多项交互修复 |
| 1.1.0 | 2026-05-27 | RambosPlayer v1.1.0 | 新增 HTTP-MPEG-TS 低延迟推流，修复多设备卡顿、seek 重连风暴等问题 |
| 1.0.0 | 2026-05-23 | RambosPlayer v1.0.0 | 基于 FFmpeg + Qt 的 Windows 多媒体播放器首个正式版本 |

---

## v1.2.4 — RambosPlayer v1.2.4 (2026-06-18)

**Tag**: `v1.2.4`

### Release Notes

针对纯视频 RTMP 直播流在窗口最小化/被遮挡场景下的延迟累积问题，共四项修复。

**Bug 修复**

- 修复窗口最小化一段时间后还原，延迟随最小化时长线性增长：无音频轨的流缺少音频时钟驱动的"落后即丢帧追赶"机制，最小化期间 Windows 限流后台窗口的定时器调度，本地阻塞队列迅速填满反压至网络读取，积压堆积在 TCP/服务端缓冲区中不可见；新增短程节拍锚点的"落后超过 1 秒即清空本地队列只保留最新帧"逻辑，并加入独立看门狗线程与"落后超过 5 秒即触发整条流重连"机制（#048）
- 修复窗口被其他窗口遮挡（未触发最小化状态）时无法检测、延迟无法恢复：恢复触发点从只监听 `isMinimized()` 改为监听 `QGuiApplication::applicationStateChanged` 回到 `Active`，同时覆盖"最小化恢复"和"遮挡后切回前台"两种场景（#049）
- 修复"本地丢帧追赶是否已足够"的判断指标失真，导致本该执行的重连被错误跳过：撤回基于挂钟时间反推播放进度的本地追赶优化，恢复为窗口恢复前台时直接触发重连，避免一个与实际等待时长无关的结构性常量误判为"已追上"（#050）
- 修复每次重连（以及首次打开）固定卡顿约 5～6 秒：`avformat_find_stream_info` 因 `max_analyze_duration=0` 未按预期"立即返回"，退化为默认 5 秒探测窗口；改为 `max_analyze_duration=AV_TIME_BASE`（1 秒）+ `probesize=32KB`，重连总耗时从 5.6～5.8 秒降至 0.6 秒（#051）

### 下载

解压 `RambosPlayer-v1.2.4.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.2.3 — RambosPlayer v1.2.3 (2026-06-16)

**Tag**: `v1.2.3`

### Release Notes

针对 AlertGateway（RK3588S 边缘 AI 摄像头）RTMP 直播流的三项播放优化。

**Bug 修复**

- 修复纯视频 RTMP 流卡顿（实际渲染帧率退化至 1.7fps）：引入 Live Pacing 短程锚点节拍控制，SRS 按 GOP 批量投递的 8 帧将均匀铺开至 ~533ms 渲染，视觉帧率恢复 15fps
- 修复 RTMP 连接延迟约 6 秒：加入 `rtmp_buffer=0`、`AVFMT_FLAG_NOBUFFER`、`max_analyze_duration=0`，连接延迟从 ~6s 降至 ~0.5s
- 修复长时间播放后延迟持续升高：短程锚点避免全局锚点因编码器帧率与 PTS timebase 微小偏差（≤1%）导致 holdMs 线性积累

### 下载

解压 `RambosPlayer-v1.2.3.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.2.2 — RambosPlayer v1.2.2 (2026-06-15)

**Tag**: `v1.2.2`

### Release Notes

新增拉流播放功能，支持从 RTMP/RTSP/HTTP-FLV/SRT 网络流拉取播放，异步探测避免 UI 阻塞，断线自动重连。

**新功能**

- 拉流播放：工具菜单"拉流播放"显示控制栏，输入网络流地址点击"连接"即可播放
- 支持协议：RTMP、RTSP、HTTP-FLV、SRT、HTTPS
- 异步探测：`avformat_open_input` + `avformat_find_stream_info` 在独立 worker 线程执行，网络握手不阻塞 UI
- 自动重连：网络中断后 2 秒间隔循环重试，状态栏显示"重连中..."，用户可随时点"断开"取消
- 实时统计：码率（kbps）和视频帧率（fps）每秒刷新显示在状态栏
- 直播模式：`duration=0` 时进度条自动禁用并显示 "LIVE"
- 地址记忆：上次成功连接的 URL 自动保存，下次打开拉流面板自动回填
- FFmpeg 版本兼容：`AvioBuf` 类型别名适配 FFmpeg 7.0 `const uint8_t*` 签名变化，ARM64 板卡旧版 FFmpeg 同时兼容

**架构改进**

- `DemuxThread::probeOpen()` 静态方法：不访问 `this`，可在任意线程调用，网络流探测与 UI 线程完全解耦
- `PlayerController::open()` 异步化：worker 线程探测 + `onProbeFinished` 回调 + `probeGeneration_` 丢弃过期结果
- `DemuxThread::NetworkState` 状态机：Disconnected → Connecting → Connected → Reconnecting，信号链式转发到 UI
- 新增拉流架构图 `docs/pull-streaming-arch.html`

### 下载

解压 `RambosPlayer-v1.2.2.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.2.1 — RambosPlayer v1.2.1 (2026-06-04)

**Tag**: `v1.2.1`

### Release Notes

**Bug 修复**

- 修复浏览剪辑切换多段剪辑时，浏览区间残留导致误导出的问题；切换时弹出确认框（默认清空），用户可选择保留或重新输入

**交互 / 术语优化**

- 全局将"剪切"统一改为"剪辑"（UI 菜单"剪辑(&C)"、"浏览剪辑(&B)"、"多段剪辑(&M)"、状态栏提示、注释）
- 导出文件名时间戳始终显示小时位（`00h01m23s`），与长视频区间对齐
- 导出日志耗时改为 `00h00m00s` 格式

**构建**

- `RambosPlayer.pro` 补录 v1.2.0 漏加的源文件：MergePanel、MergeWorker、ConcatDemuxer、ConcatFilter、AudioMixPanel、AudioMixWorker、AudioPreviewWindow

### 下载

解压 `RambosPlayer-v1.2.1.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.2.0 — RambosPlayer v1.2.0 (2026-05-29)

**Tag**: `v1.2.0`

### Release Notes

在 v1.1.0 基础上新增多段剪切、视频合并、音频混合三大剪辑模块，并优化最大化窗口交互体验。

**新功能**

- 多段剪切（SegmentClipper）：Ctrl+M 进入多段模式，支持在时间轴上标记多个区间，三种剪切模式（自由/浏览/多段）互斥切换
- 视频合并（MergePanel）：选择多个视频文件合并为一个，支持编码器 fallback + 合成诊断日志
- 音频混合面板：支持 BGM 混合、音量调节、音频试听弹窗，abuffersrc time_base 自动匹配
- 最大化浮动标题栏：窗口最大化时标题栏自动隐藏，鼠标悬停顶部滑入显示，带 SVG 图标动画

**Bug 修复**

- 修复试播放快进时 StoppedState 伪触发导致播放中断（#037）
- 修复音频混合 BGM 无声 bug（abuffersrc time_base 与流不匹配）
- 修复未打开文件时三种剪切模式统一弹窗提示
- 修复多段剪切确认时不清空已有区间
- 修复多段剪切取消时恢复浏览/自由模式
- 修复浏览→自由切换时保留底部导轨区间
- 修复 UTF-8 乱码问题

### 下载

解压 `RambosPlayer-v1.2.0.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.1.0 — RambosPlayer v1.1.0 (2026-05-27)

**Tag**: `v1.1.0`

### Release Notes

在 v1.0.0 基础上新增 HTTP-MPEG-TS 低延迟推流通道，并修复多项推流稳定性问题。

**新功能**

- HTTP-MPEG-TS 推流：使用 StreamPipeline + MpegTsServer，浏览器通过 mpegts.js 直接播放，延迟约 1–2 秒
- 支持多设备同时连接，晚接入客户端自动补发 PAT/PMT + 当前 GOP 数据

**Bug 修复**

- 修复 seek 后重连风暴：不再断开客户端，改用 PTS remap 保持流连续
- 修复 AAC 编码器 PTS 污染：seek 时销毁重建编码器上下文，消除 `audio write error: Invalid argument`
- 修复 seek 推流卡死：libx264 `flush_buffers` 进入 EOS 状态，改为 `reopenCodec()` 重建
- 修复多设备轮流卡顿：移除错误的互斥锁，改用 QTcpSocket 非阻塞写入
- 修复暂停恢复后推流黑屏：暂停期间继续向编码线程喂帧
- 修复平板端首次连接 10 秒白屏：确保首个关键帧在编解码配置就绪后才发送
- 修复开局闪白：推流开始时丢弃编码器输出的首帧

### 下载

解压 `RambosPlayer-v1.1.0.zip`，双击 `RambosPlayer.exe` 运行。

---

## v1.0.0 — RambosPlayer v1.0.0 (2026-05-23)

**Tag**: `v1.0.0`

### Release Notes

基于 FFmpeg + Qt 的 Windows 多媒体播放器，首个正式版本。

**播放**

- 本地视频播放，支持主流格式（MP4 / MKV / AVI / FLV 等）
- 硬件加速解码（D3D11VA），降低 CPU 占用
- 进度条拖拽 Seek，键盘快退 / 快进（← / →）
- 双击切换全屏，Esc 退出
- 最近文件列表（最多 10 条）

**推流**

- HTTP-FLV 内置服务器：同局域网设备用浏览器打开即可实时观看，无需安装任何软件
- SRT 推流：低延迟局域网推流，支持 OBS 等客户端接收
- 本地录制：推流的同时保存为本地 FLV 文件
- 多路目标同时推流

**剪辑**

- 剪辑模式（Ctrl+T）：设置入点 / 出点，视频缩略图预览
- 无损导出（Ctrl+E）：直接 copy 码流，不重编码

**其他**

- 滤镜面板：亮度 / 对比度 / 饱和度实时调节
- 关于页面：快捷键说明 + 项目主页

### 快捷键

| 按键         | 功能             |
| ------------ | ---------------- |
| Ctrl+O       | 打开文件         |
| 空格         | 播放 / 暂停      |
| ← / →        | 快退 / 快进 5 秒 |
| 双击视频     | 切换全屏         |
| Ctrl+Shift+S | 推流设置         |
| Ctrl+T       | 剪辑模式         |
| Ctrl+E       | 导出片段         |

### 系统要求

- Windows 10 / 11 (64-bit)
- 解压即用，无需安装
- HTTP-FLV 推流需要防火墙放行对应端口（程序会自动尝试添加规则）

### 下载

解压 `RambosPlayer-v1.0.0.zip`，双击 `RambosPlayer.exe` 运行。

---

## 追加模板

发布新版本时在「版本历史」表格和下方正文各追加一条：

```markdown
## vX.Y.Z — Release Title (YYYY-MM-DD)

**Tag**: `vX.Y.Z`

### Release Notes

...
```
