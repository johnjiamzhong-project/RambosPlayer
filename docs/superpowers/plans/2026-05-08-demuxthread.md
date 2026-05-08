# DemuxThread 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `DemuxThread`，用 FFmpeg `av_read_frame` 循环将媒体文件的视频/音频包分发到两条 `FrameQueue<AVPacket*>`，支持线程安全的 stop 和 seek。

**Architecture:** DemuxThread 继承 QThread，`open()` 初始化 AVFormatContext，`run()` 循环读包并按流索引 push 到对应队列；`stop()` 通过原子标志 + `queue->abort()` 唤醒阻塞线程；`seek()` 通过原子 double 传递目标时间，在循环顶部 `handleSeek()` 执行并 pop+free 清空队列残留包。

**Tech Stack:** C++17, Qt 5.14.2 (QThread), FFmpeg 4.x (libavformat), FrameQueue<T>（已实现）

---

## 文件结构

| 文件 | 操作 | 职责 |
|------|------|------|
| `src/demuxthread.h` | 新建 | 类声明、成员变量、公共接口 |
| `src/demuxthread.cpp` | 新建 | open/stop/seek/run/handleSeek 实现 |
| `tests/tst_demuxthread.cpp` | 新建 | 三个单元测试用例 |
| `tests/data/sample.mp4` | 生成 | 2 秒测试视频（ffmpeg 命令生成） |

---

## Task 1: 生成测试视频

**Files:**
- Create: `tests/data/sample.mp4`

- [ ] **Step 1: 创建目录并生成测试视频**

在项目根目录执行（需要本机装有 ffmpeg 或 vcpkg 里的 ffmpeg.exe）：

```powershell
New-Item -ItemType Directory -Force -Path "tests\data"
ffmpeg -f lavfi -i "testsrc=duration=2:size=320x240:rate=25" `
       -f lavfi -i "sine=frequency=440:duration=2" `
       -c:v libx264 -c:a aac -shortest tests\data\sample.mp4
```

- [ ] **Step 2: 确认文件存在且大小合理（应在 20–100 KB 之间）**

```powershell
Get-Item tests\data\sample.mp4 | Select-Object Name, Length
```

期望输出：`sample.mp4` 文件存在，Length > 0。

---

## Task 2: 写失败测试

**Files:**
- Create: `tests/tst_demuxthread.cpp`

- [ ] **Step 1: 创建测试文件**

```cpp
// tests/tst_demuxthread.cpp
#include <QtTest>
#include "demuxthread.h"
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class TstDemuxThread : public QObject {
    Q_OBJECT
private slots:
    void open_validFile_returnsTrue() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(dt.open("tests/data/sample.mp4", &vq, &aq));
        QVERIFY(dt.duration() > 0);
        dt.stop();
        dt.wait(2000);
    }

    void open_invalidFile_returnsFalse() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(!dt.open("nonexistent.mp4", &vq, &aq));
    }

    void run_populatesQueues() {
        FrameQueue<AVPacket*> vq(50), aq(200);
        DemuxThread dt;
        QVERIFY(dt.open("tests/data/sample.mp4", &vq, &aq));
        dt.start();
        QThread::msleep(300);
        QVERIFY(vq.size() > 0);
        QVERIFY(aq.size() > 0);
        dt.stop();
        dt.wait(2000);
        // 清理队列中残留包，避免内存泄漏
        AVPacket* pkt;
        while (vq.tryPop(pkt, 0)) av_packet_free(&pkt);
        while (aq.tryPop(pkt, 0)) av_packet_free(&pkt);
    }
};

QTEST_MAIN(TstDemuxThread)
#include "tst_demuxthread.moc"
```

- [ ] **Step 2: 将测试文件加入 CMakeLists.txt**

打开 `CMakeLists.txt`，找到 `TstDemuxThread` 相关目标（或仿照 TstAVSync 的写法新增）：

```cmake
add_executable(TstDemuxThread tests/tst_demuxthread.cpp)
target_link_libraries(TstDemuxThread PRIVATE
    Qt5::Test Qt5::Core
    ${FFMPEG_LIBRARIES}
)
target_include_directories(TstDemuxThread PRIVATE src)
add_test(NAME TstDemuxThread COMMAND TstDemuxThread)
```

- [ ] **Step 3: 尝试编译，确认因 `demuxthread.h` 不存在而失败**

```powershell
cmake --build build --config Debug --target TstDemuxThread 2>&1 | Select-String "error"
```

期望：编译错误，提示 `demuxthread.h: No such file`。

---

## Task 3: 实现头文件

**Files:**
- Create: `src/demuxthread.h`

- [ ] **Step 1: 创建头文件**

```cpp
// src/demuxthread.h
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavformat/avformat.h>
}

class DemuxThread : public QThread {
    Q_OBJECT
public:
    ~DemuxThread() override;

    bool open(const QString& path,
              FrameQueue<AVPacket*>* videoQueue,
              FrameQueue<AVPacket*>* audioQueue);
    void stop();
    void seek(double seconds);  // 秒；原子存储，run() 下次循环顶部生效

    int64_t duration()          const { return duration_; }   // 微秒
    int     videoStreamIdx()    const { return videoIdx_; }
    int     audioStreamIdx()    const { return audioIdx_; }
    AVFormatContext* formatContext() const { return fmtCtx_; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVFormatContext*       fmtCtx_     = nullptr;
    FrameQueue<AVPacket*>* videoQueue_ = nullptr;
    FrameQueue<AVPacket*>* audioQueue_ = nullptr;
    int                    videoIdx_   = -1;
    int                    audioIdx_   = -1;
    int64_t                duration_   = 0;
    std::atomic<bool>      abort_{false};
    std::atomic<double>    seekTarget_{-1.0};

    void handleSeek();
};
```

---

## Task 4: 实现源文件

**Files:**
- Create: `src/demuxthread.cpp`

- [ ] **Step 1: 创建源文件**

```cpp
// src/demuxthread.cpp
#include "demuxthread.h"

extern "C" {
#include <libavutil/avutil.h>
}

DemuxThread::~DemuxThread() {
    stop();
    wait();
    if (fmtCtx_) avformat_close_input(&fmtCtx_);
}

bool DemuxThread::open(const QString& path,
                        FrameQueue<AVPacket*>* videoQueue,
                        FrameQueue<AVPacket*>* audioQueue) {
    if (avformat_open_input(&fmtCtx_,
                            path.toUtf8().constData(),
                            nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0)
        return false;

    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        auto type = fmtCtx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoIdx_ < 0) videoIdx_ = (int)i;
        if (type == AVMEDIA_TYPE_AUDIO && audioIdx_ < 0) audioIdx_ = (int)i;
    }

    duration_    = fmtCtx_->duration;
    videoQueue_  = videoQueue;
    audioQueue_  = audioQueue;
    return true;
}

void DemuxThread::stop() {
    abort_ = true;
    if (videoQueue_) videoQueue_->abort();
    if (audioQueue_) audioQueue_->abort();
}

void DemuxThread::seek(double seconds) {
    seekTarget_.store(seconds, std::memory_order_relaxed);
}

void DemuxThread::handleSeek() {
    double target = seekTarget_.exchange(-1.0, std::memory_order_relaxed);
    if (target < 0.0) return;

    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);

    // pop+free：不能只调 clear()，会泄漏 AVPacket*
    AVPacket* p;
    while (videoQueue_->tryPop(p, 0)) av_packet_free(&p);
    while (audioQueue_->tryPop(p, 0)) av_packet_free(&p);
}

void DemuxThread::run() {
    AVPacket* pkt = av_packet_alloc();

    while (!abort_) {
        handleSeek();

        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret < 0) break;  // EOF 或读取错误

        if (pkt->stream_index == videoIdx_) {
            AVPacket* p = av_packet_clone(pkt);
            videoQueue_->push(p);
        } else if (pkt->stream_index == audioIdx_) {
            AVPacket* p = av_packet_clone(pkt);
            audioQueue_->push(p);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    emit finished();
}
```

---

## Task 5: 运行测试，确认全部通过

**Files:** 无新文件

- [ ] **Step 1: 编译**

```powershell
cmake --build build --config Debug --target TstDemuxThread
```

期望：编译无错误。

- [ ] **Step 2: 运行测试**

```powershell
.\build\Debug\TstDemuxThread.exe
```

期望输出：

```
PASS   TstDemuxThread::open_validFile_returnsTrue
PASS   TstDemuxThread::open_invalidFile_returnsFalse
PASS   TstDemuxThread::run_populatesQueues
Totals: 3 passed, 0 failed, 0 skipped
```

- [ ] **Step 3: 若有失败，常见原因排查**

| 症状 | 原因 | 处理 |
|------|------|------|
| `open_validFile` FAIL | sample.mp4 路径不对 | 确认工作目录是项目根目录，或用绝对路径 |
| `run_populatesQueues` 队列为 0 | 300ms 不够 | 改为 500ms |
| 编译找不到 `av_packet_clone` | FFmpeg 版本 < 3.2 | 检查 vcpkg 版本 |

---

## Task 6: Commit

**Files:** 无新文件

- [ ] **Step 1: 确认 git 状态**

```powershell
git status
```

期望：看到 `src/demuxthread.h`、`src/demuxthread.cpp`、`tests/tst_demuxthread.cpp`、`tests/data/sample.mp4` 为未追踪或已修改状态。

- [ ] **Step 2: 暂存并提交**

```powershell
git add src/demuxthread.h src/demuxthread.cpp tests/tst_demuxthread.cpp tests/data/sample.mp4
git commit -m "feat: Task 4 DemuxThread 解复用线程（TDD）"
```

- [ ] **Step 3: 更新 readme.md 进度表**

将 `readme.md` 中 Task 4 一行由 `📋` 改为 `✅ 完成`：

```markdown
| Task 4 — DemuxThread 解复用线程（TDD） | ✅ 完成 |
```

```powershell
git add readme.md
git commit -m "docs: 更新 Task 4 完成进度"
```
