# 推流测试指南（本地 SRS）

## 为什么用 SRS，而不是 ffplay listen

### ffplay listen 的问题

`ffplay -listen 1 rtmp://...` 本质上是让 ffplay 充当一个极简 TCP 服务端，推流端和播放端**直接 TCP 对接**，中间没有任何缓冲：

```
RambosPlayer → [直连 TCP] → ffplay
```

结果：一旦 ffplay 处理帧的速度跟不上推流速度，就会反压推流端，导致 MuxThread 的 `av_write_frame` 阻塞，进而让整个播放卡顿。实测非常卡，无法用于正常测试。

### SRS 的优势

SRS 是一个完整的流媒体服务器，推流端和播放端通过服务器缓冲**完全解耦**：

```
RambosPlayer →[RTMP推流]→ SRS[缓冲+GOP cache] →[HTTP-FLV/RTMP拉流]→ 播放端
```

- 推流端写入速度不受播放端影响
- 支持多个播放端同时拉流
- 内置 Web 播放页面，方便直接在浏览器里验证

---

## 下载与启动 SRS 5.0（Windows）

1. 从 GitHub Releases 下载 SRS 5.0 Windows 版本：
   `https://github.com/ossrs/srs/releases/tag/v5.0-r0`
   选择 `srs-server-win64-v5.0-r0.zip`（或类似名称）

2. 解压到任意目录，例如 `D:\srs`

3. 启动直播模式：
   ```powershell
   cd D:\srs
   .\srs-live.bat
   ```

4. 浏览器打开验证服务已启动：
   `http://127.0.0.1:8080/`

---

## 推流地址和播放地址

| 用途 | 协议 | 地址 |
|------|------|------|
| RambosPlayer **推流目标** | RTMP | `rtmp://127.0.0.1/live/livestream` |
| ffplay **验证拉流** | RTMP | `rtmp://127.0.0.1/live/livestream` |
| 浏览器 **Web 播放页** | HTTP-FLV | `http://127.0.0.1:8080/players/srs_player.html?schema=http` |
| 直接拉 FLV 流 | HTTP-FLV | `http://127.0.0.1:8080/live/livestream.flv` |

在 RambosPlayer 的推流配置对话框中，URL 填写：
```
rtmp://127.0.0.1/live/livestream
```

---

## 用 ffplay 拉流（推荐验证方式）

```powershell
ffplay -fflags nobuffer -flags low_delay rtmp://127.0.0.1/live/livestream
```

| 参数 | 作用 |
|------|------|
| `-fflags nobuffer` | 禁用输入侧预缓冲，收到帧立即处理，延迟从默认 1–5 s 降到近实时 |
| `-flags low_delay` | 禁用解码器内部帧重排缓冲，对含 B 帧的 H.264 视频有额外降低延迟效果 |

**推荐用 ffplay 而非浏览器验证 seek 行为**：ffplay 对时间戳不连续的容忍度更高，seek 后无需手动操作，原因见下节。

---

## seek 后为什么 SRS 网页播放器需要重新点 Play

这是 **HTTP-FLV 客户端（FLV.js）的正常行为**，不是推流代码的 bug。

### 推流侧实际发生了什么

seek 时 MuxThread 通过 `videoAccumPts_` 续接时间戳，RTMP 连接**全程保持**，SRS 服务器侧流没有中断。

### 客户端为什么 freeze

HTTP-FLV 是浏览器通过 HTTP 持续下载的 FLV 字节流，底层由 FLV.js 驱动。seek 时有两个现象会触发 FLV.js 暂停：

1. **帧间距突变**：DemuxThread 执行 `av_seek_frame` 期间（几十毫秒），推流队列短暂为空，造成相邻两帧之间的时间戳间距异常大。

2. **B 帧 PTS 非单调**（H.264 含 B 帧的视频）：FLV/RTMP 按 DTS 顺序传输包，DTS 单调递增，但 B 帧的 PTS 天然非单调（如 I B B P 序列的 PTS 顺序是 0 3 1 4 2）。FLV.js 遇到 PTS 回跳会暂停等待"正确"的帧。

**结论**：SRS 服务器和 MuxThread 工作正常；freeze 是浏览器播放器对流不连续的保护性暂停，重新点 Play 后会从当前最新帧继续。

### 如果要消除这个现象

用 `ffplay` 拉 RTMP 而不是浏览器拉 HTTP-FLV。ffplay 对时间戳不连续的容忍度更高，seek 后会自动继续播放，无需手动操作：

```powershell
ffplay -fflags nobuffer -flags low_delay rtmp://127.0.0.1/live/livestream
```

---

## 验证 seek 后 PTS 单调性

录一段含 seek 操作的本地 FLV 文件，录完后用 ffprobe 检测 DTS 是否单调递增：

```powershell
$prev = -1.0
ffprobe -v quiet -show_entries packet=dts_time,flags -select_streams v:0 -of csv out.flv |
ForEach-Object {
    $cols = $_ -split ','
    $dts  = [double]$cols[2]
    $flag = $cols[3]
    if ($prev -ge 0 -and $dts -le $prev) {
        Write-Host "DTS 非单调: $prev -> $dts  flag=$flag"
    }
    $prev = $dts
}
Write-Host "Done"
```

无输出（只有 Done）表示 DTS 单调递增，推流时间戳处理正确。

> 注意：`video PTS non-monotonic` 日志对 B 帧内容是正常现象，不代表推流有问题。
> MuxThread 监控的是 DTS 单调性，PTS 非单调是 B 帧编码特性，FLV 协议本身支持。
