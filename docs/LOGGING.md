# RambosPlayer 日志系统

## 概述

RambosPlayer 的日志系统通过拦截 Qt 消息处理，将所有应用日志写入带时间戳的日志文件，同时在 Windows 下支持崩溃时的 minidump 转储。每条日志立即刷新到磁盘，保证进程异常退出前的日志不丢失。

## 工作原理

### 消息拦截链

```
应用代码调用 Qt 日志函数
    ├─ qDebug()
    ├─ qInfo()
    ├─ qWarning()
    ├─ qCritical()
    └─ qFatal()
              ↓
      qInstallMessageHandler()
              ↓
      Logger::messageHandler()
              ↓
      写入日志文件 + 输出到 stderr
```

### 初始化流程

日志系统在 `main.cpp` 中 `QApplication` 构造后立即初始化：

```cpp
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Logger::install();  // 必须在 QApplication 之后调用
    // ... 其他初始化
}
```

调用 `Logger::install()` 会：
1. 创建日志目录（默认 `<exe>/logs/`）
2. 创建日志文件：`rambos_yyyyMMdd_HHmmss.log`
3. 注册 Qt 消息处理器
4. 在 Windows 上注册 SEH 崩溃处理器
5. 设置日志级别规则

## 日志文件

### 位置

- **Windows/Linux/macOS**：`<exe所在目录>/logs/rambos_yyyyMMdd_HHmmss.log`
- **开发环境**：由 `launch.json` 设置 cwd 为项目根目录，日志位置为 `logs/rambos_*.log`

### 日志格式

```
[2026-05-23 14:30:45.123] [INFO][mainwindow.cpp:42] 播放器启动成功
[2026-05-23 14:30:46.512] [WARNING][demuxthread.cpp:78] 跳过不支持的流类型
[2026-05-23 14:30:47.890] [CRITICAL][videodecodethread.cpp:105] 硬件解码初始化失败
```

格式说明：
- `[时间戳]`：精确到毫秒（yyyy-MM-dd HH:mm:ss.zzz）
- `[级别]`：DEBUG / INFO / WARNING / CRITICAL / FATAL
- `[文件:行号]`：源代码位置（仅保留文件名，去掉路径）
- 消息：日志内容

### 日志级别

| 级别 | 用途 | 默认启用 |
|------|------|--------|
| DEBUG | 详细开发信息 | ✗ |
| INFO | 常规信息（启动、完成等） | ✓ |
| WARNING | 警告（可恢复的问题） | ✓ |
| CRITICAL | 严重错误（功能不可用） | ✓ |
| FATAL | 致命错误（进程终止） | ✓ |

## 使用方法

### 基础日志输出

在代码中使用 Qt 日志函数：

```cpp
#include <QDebug>

// 通常信息
qInfo() << "播放器启动成功，文件：" << filename;

// 警告
qWarning() << "音频缓冲不足，当前容量：" << queue.size();

// 严重错误
qCritical() << "解码器初始化失败，错误码：" << ret;

// 致命错误（会终止进程）
qFatal("内存分配失败");
```

### Trace 日志（调试模式）

用于关键路径的详细跟踪，默认不输出：

```cpp
#include "logger.h"

// 在关键路径添加 trace
qCDebug(lcTrace) << "VideoDecodeThread::run() 开始处理包" << pkt->pts;
qCDebug(lcTrace) << "AVSync::videoDelay() 计算延迟" << delay << "ms";
```

启用 Trace：

```powershell
# PowerShell（开发环境）
$env:QT_LOGGING_RULES="rambos.trace=true"
.\build\Debug\RambosPlayer.exe

# 或在 launch.json 中设置
"env": {
    "QT_LOGGING_RULES": "rambos.trace=true"
}
```

启用后，所有 `qCDebug(lcTrace)` 的日志会输出到日志文件。

### 日志查看

#### 实时查看（开发）

```powershell
# 持续跟踪最新日志
tail -f logs/rambos_*.log

# 或搜索特定关键词
Get-Content logs/rambos_*.log | Select-String "WARNING"
```

#### 筛选日志

```powershell
# 查看所有错误和警告
Get-Content logs/rambos_*.log | Select-String "CRITICAL|WARNING"

# 查看特定模块的日志（如视频解码线程）
Get-Content logs/rambos_*.log | Select-String "videodecodethread"

# 查看时间范围内的日志
Get-Content logs/rambos_*.log | Select-String "14:30:"
```

## 崩溃处理

### Windows minidump

当程序因未捕获异常崩溃时，Logger 会生成 minidump 文件供调试：

```
logs/
├─ rambos_20260523_143045.log
└─ crash_20260523_143050.dmp  ← 崩溃转储
```

### 查看崩溃堆栈

#### 使用 WinDbg

```powershell
# 1. 打开 WinDbg
# 2. File → Open Crash Dump → 选择 crash_*.dmp
# 3. 加载符号文件（pdb）
.sympath+ C:\path\to\build\Debug
.reload

# 4. 查看崩溃时的调用栈
~* k  # 所有线程
k     # 当前线程
```

#### 使用 Visual Studio

```
1. 菜单：Debug → Open Crash Dump
2. 选择 crash_*.dmp 文件
3. 点击"Debug with Native Only"
4. 在 Call Stack 窗口查看崩溃堆栈
```

### 排查崩溃的步骤

1. **查看日志文件**（必须）
   - 打开 `logs/rambos_*.log`
   - 找到时间戳最接近崩溃时刻的最后一条日志
   - 理解崩溃前在做什么

2. **查看 minidump**（如有）
   ```powershell
   # 查看 dmp 文件是否存在
   Get-ChildItem logs/crash_*.dmp
   
   # 用 WinDbg 或 VS 加载
   ```

3. **复现问题**
   - 根据日志推断崩溃条件
   - 尝试重现崩溃（例如用同样的媒体文件）
   - 添加 Trace 日志到可疑代码路径

4. **检查线程安全**
   - 如果涉及多线程，检查是否有竞态条件
   - 查看日志中多线程的交互顺序

## 日志级别规则

### 默认规则

```cpp
// logger.cpp 中设置
QString rules = "*.debug=false";  // DEBUG 日志默认关闭
QLoggingCategory::setFilterRules(rules);
```

这意味着 `qDebug()` 的输出不会出现在日志文件中。

### 自定义规则

通过环境变量 `QT_LOGGING_RULES` 自定义：

```powershell
# 启用 trace 日志
$env:QT_LOGGING_RULES="rambos.trace=true"

# 启用所有 debug 日志
$env:QT_LOGGING_RULES="*.debug=true"

# 禁用某些日志
$env:QT_LOGGING_RULES="qt.network=false"

# 多规则用换行分隔
$env:QT_LOGGING_RULES="rambos.trace=true`nqt.network=false"
```

在 `launch.json` 中配置：

```json
{
    "configurations": [
        {
            "name": "Debug with Trace",
            "type": "cppvsdbg",
            "env": {
                "QT_LOGGING_RULES": "rambos.trace=true"
            }
        }
    ]
}
```

## 最佳实践

### 何时使用各级别

```cpp
// INFO：程序重要事件（启动、完成、模式切换）
qInfo() << "切换到全屏模式";

// WARNING：可恢复的问题（重试、降级、跳过）
qWarning() << "硬件解码不可用，回退到软件解码";

// CRITICAL：功能严重故障，但进程继续运行
qCritical() << "音频输出初始化失败，静音继续播放";

// FATAL：无法继续，必须终止
if (!initializeFFmpeg()) {
    qFatal("FFmpeg 初始化失败，无法继续");
}

// DEBUG：开发调试，生产环境不需要
qDebug() << "当前帧 PTS：" << frame->pts;

// TRACE：关键路径的细粒度跟踪，默认关闭
qCDebug(lcTrace) << "FrameQueue::put() 入队" << pkt;
```

### 日志内容指南

✓ **好的日志**
```cpp
qInfo() << "打开文件成功：" << filepath << "，分辨率：" << width << "x" << height;
qWarning() << "Seek 操作超时（" << elapsed << "ms），可能是大文件";
qCritical() << "直播推流连接失败：" << errorString << "（错误码：" << errno << "）";
```

✗ **差的日志**
```cpp
qInfo() << "OK";  // 太简洁，不知道发生了什么
qInfo() << "处理中...";  // 模糊不清
qWarning() << ctx.file;  // 只输出指针或枚举，无意义
```

### 多线程日志

Logger 内部使用 `QMutex` 保护，多线程安全。各线程可放心使用：

```cpp
// 在 DemuxThread::run() 中
qInfo() << "DemuxThread 开始读取，文件：" << file->fileName();

// 在 VideoDecodeThread::run() 中
qInfo() << "VideoDecodeThread 初始化硬件解码";

// 在主线程 MainWindow 中
qInfo() << "用户点击播放";
```

日志输出顺序反映了线程间的时序关系，有助于排查并发问题。

## 故障排查

### 问题：日志文件中没有预期的日志

**原因 1**：日志级别不匹配
```powershell
# 检查规则设置
$env:QT_LOGGING_RULES  # 是否启用了对应的日志级别

# 解决：显式启用
$env:QT_LOGGING_RULES="*.debug=true"
```

**原因 2**：日志分类名不对
```cpp
// 错误
qCDebug(lcTrace) << "...";  // lcTrace 未定义

// 正确
#include "logger.h"
qCDebug(lcTrace) << "...";
```

**原因 3**：日志还未 flush
- Logger 每条日志都会立即 flush，除非程序异常终止
- 如果看不到日志，检查 `logs/` 目录是否存在

### 问题：无法找到崩溃时的日志

**检查清单**
1. 日志目录是否存在：`logs/rambos_*.log`
2. 文件权限：确保有写权限
3. 磁盘空间：检查是否满盘
4. 崩溃时刻：minidump 时间戳应接近日志的最后一条

### 问题：minidump 文件很大（>100MB）

这是正常的，minidump 包含了进程的内存快照。可以在 WinDbg 中只加载必要的堆栈信息来减少加载时间。

## 文件清理

日志文件会不断增长，可定期清理：

```powershell
# 删除 7 天前的日志
Get-ChildItem logs/rambos_*.log -mtime +7 | Remove-Item

# 删除大于 100MB 的日志
Get-ChildItem logs/rambos_*.log | Where-Object {$_.Length -gt 100MB} | Remove-Item

# 保留最新的 10 个日志，其余删除
Get-ChildItem logs/rambos_*.log | Sort-Object LastWriteTime -Descending | Select-Object -Skip 10 | Remove-Item
```

## 相关代码

- 实现：`src/logger.h`、`src/logger.cpp`
- 初始化：`src/main.cpp`
- 消息拦截：Qt 框架 `qInstallMessageHandler()`
- 崩溃处理（Windows）：`SetUnhandledExceptionFilter()`、`MiniDumpWriteDump()`
