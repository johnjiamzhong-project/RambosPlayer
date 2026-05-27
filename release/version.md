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
| 1.1.0 | 2026-05-27 | RambosPlayer v1.1.0 | 新增 HTTP-MPEG-TS 低延迟推流，修复多设备卡顿、seek 重连风暴等问题 |
| 1.0.0 | 2026-05-23 | RambosPlayer v1.0.0 | 基于 FFmpeg + Qt 的 Windows 多媒体播放器首个正式版本 |

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
