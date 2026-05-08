# RambosPlayer 核心播放器实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建可用的多媒体播放器，支持本地视频/音频文件播放、进度条拖拽 Seek、音量控制和全屏切换。

**Architecture:** 解复用线程将包推入两条包队列，视频/音频解码线程分别消费包队列并产出帧；音频解码线程以 QAudioSink 推送 PCM 并维护音频时钟，视频渲染器依据音频时钟计算延迟后 QPainter 绘帧，PlayerController 串联全部组件并向 MainWindow 暴露简单控制接口。

| 阶段 | 组件 | 作用 |
|------|------|------|
| 1️⃣ 解复用 | DemuxThread | 读取视频文件，分离视频/音频包 → 推入 2 条队列 |
| 2️⃣ 解码 | VideoDecodeThread + AudioDecodeThread | 消费包队列，解码成帧 |
| 3️⃣ 同步 | AudioDecodeThread + AVSync | 音频输出推动主时钟，视频帧依音频延迟决定绘制时间 |
| 4️⃣ 渲染 | VideoRenderer + MainWindow | 计算延迟后用 QPainter 绘帧，暴露播放/暂停/音量等控制 |

**核心设计**：音频是主时钟，视频跟音频同步。

**Tech Stack:** C++17, VS2017, Qt 5.14.2 (QThread / QAudioSink / QPainter / QSlider), FFmpeg 4.x via vcpkg (libavformat / libavcodec / libswresample / libswscale)

---

## 范围说明

本计划仅覆盖 **初级功能1 + 初级功能2**（核心播放器 + 带控件的完整 UI）。
进阶功能（硬件加速 / 滤镜 / 推流 / 剪辑）各自独立，待本计划完成后另建计划。

---

## 文件结构

```
RambosPlayer/
├── RambosPlayer.pro          # qmake 项目文件（或 .vcxproj）
├── src/
│   ├── main.cpp
│   ├── framequeue.h          # 线程安全模板队列（仅头文件）
│   ├── avsync.h/.cpp         # 音频时钟 + 视频延迟计算
│   ├── demuxthread.h/.cpp    # 解复用线程
│   ├── videodecodethread.h/.cpp  # 视频解码线程
│   ├── audiodecodethread.h/.cpp  # 音频解码 + QAudioSink 推流
│   ├── videorenderer.h/.cpp  # QPainter 渲染部件 + 帧定时
│   ├── playercontroller.h/.cpp  # 组合以上所有组件
│   ├── mainwindow.h/.cpp     # Qt UI：进度条 / 音量 / 全屏
│   └── mainwindow.ui         # Qt Designer 布局
└── tests/
    ├── tests.pro
    ├── tst_framequeue.cpp
    ├── tst_avsync.cpp
    └── tst_demuxthread.cpp   # 需要一个 2 秒测试视频 tests/data/sample.mp4
```

**各文件职责：**

| 文件 | 职责 |
|------|------|
| `framequeue.h` | 生产者/消费者阻塞队列，上限可配，abort 解锁全部等待线程 |
| `avsync.h/.cpp` | 原子量存储音频时钟 PTS，`videoDelay()` 返回视频应等待的毫秒数 |
| `demuxthread.h/.cpp` | `av_read_frame` 循环，按流索引分发包到两条 `FrameQueue<AVPacket*>` |
| `videodecodethread.h/.cpp` | `avcodec_send_packet` / `avcodec_receive_frame` 循环，产出 `AVFrame*` |
| `audiodecodethread.h/.cpp` | 解码 + `swr_convert` 重采样为 S16 → `QAudioSink::write`，更新音频时钟 |
| `videorenderer.h/.cpp` | 持有帧队列，`QTimer` 每 1 ms 检查是否到渲染时间，`paintEvent` 用 QPainter 绘图 |
| `playercontroller.h/.cpp` | 持有并生命周期管理全部组件；对外暴露 open/play/pause/stop/seek/setVolume |
| `mainwindow.h/.cpp` | Qt MainWindow，`QSlider` 进度 + 音量，播放/暂停按钮，双击全屏 |

---

## Task 1: 项目脚手架 ✅

**Files:**
- Create: `RambosPlayer.pro`
- Create: `src/main.cpp`
- Create: `tests/tests.pro`

- [x] **Step 1: 创建 qmake 主项目文件**

```pro
# RambosPlayer.pro
QT += core gui widgets multimedia

CONFIG += c++17
TARGET = RambosPlayer
TEMPLATE = app

# vcpkg FFmpeg（Windows x64）
VCPKG = D:/vcpkg/installed/x64-windows
INCLUDEPATH += $$VCPKG/include
LIBS += -L$$VCPKG/lib \
        -lavformat -lavcodec -lavutil \
        -lswresample -lswscale

SOURCES += src/main.cpp \
           src/mainwindow.cpp \
           src/playercontroller.cpp \
           src/demuxthread.cpp \
           src/videodecodethread.cpp \
           src/audiodecodethread.cpp \
           src/videorenderer.cpp \
           src/avsync.cpp

HEADERS += src/mainwindow.h \
           src/playercontroller.h \
           src/demuxthread.h \
           src/videodecodethread.h \
           src/audiodecodethread.h \
           src/videorenderer.h \
           src/framequeue.h \
           src/avsync.h

FORMS   += src/mainwindow.ui
```

- [x] **Step 2: 创建最小 main.cpp**

```cpp
// src/main.cpp
#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(960, 600);
    w.show();
    return app.exec();
}
```

- [x] **Step 3: 创建测试子项目**

```pro
# tests/tests.pro
QT += testlib core
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = rambos_tests

VCPKG = D:/vcpkg/installed/x64-windows
INCLUDEPATH += $$VCPKG/include ../src
LIBS += -L$$VCPKG/lib -lavformat -lavcodec -lavutil -lswresample -lswscale

SOURCES += tst_framequeue.cpp tst_avsync.cpp tst_demuxthread.cpp
```

- [x] **Step 4: 确认编译通过（空实现）**

在 VS2017 中打开 qmake 生成的 .sln，Build → 应编译无错。

- [x] **Step 5: Commit**

```bash
git add RambosPlayer.pro src/main.cpp tests/tests.pro
git commit -m "chore: 项目脚手架"
```

---

## Task 2: FrameQueue（线程安全帧队列）✅

**Files:**
- Create: `src/framequeue.h`
- Create: `tests/tst_framequeue.cpp`

- [x] **Step 1: 写失败测试**

```cpp
// tests/tst_framequeue.cpp
#include <QtTest>
#include "framequeue.h"
#include <QThread>

class TstFrameQueue : public QObject {
    Q_OBJECT
private slots:
    void pushPop_singleThread() {
        FrameQueue<int> q(10);
        q.push(42);
        int val = -1;
        QVERIFY(q.tryPop(val, 100));
        QCOMPARE(val, 42);
    }

    void tryPop_returnsFlase_whenEmpty() {
        FrameQueue<int> q(10);
        int val = -1;
        QVERIFY(!q.tryPop(val, 50));
    }

    void blocksAt_maxSize() {
        FrameQueue<int> q(2);
        q.push(1);
        q.push(2);
        // 第三次 push 在 abort 前应阻塞；用线程验证
        std::atomic<bool> pushed{false};
        QThread* t = QThread::create([&]{
            q.push(3); // blocks until pop
            pushed = true;
        });
        t->start();
        QThread::msleep(80);
        QVERIFY(!pushed); // 应仍在阻塞
        int v; q.tryPop(v, 50);
        QThread::msleep(50);
        QVERIFY(pushed);
        t->wait();
        delete t;
    }

    void abort_unblocks_pop() {
        FrameQueue<int> q(10);
        std::atomic<bool> done{false};
        QThread* t = QThread::create([&]{
            int v;
            q.tryPop(v, 5000); // would block 5 s
            done = true;
        });
        t->start();
        QThread::msleep(30);
        q.abort();
        t->wait(500);
        QVERIFY(done);
        delete t;
    }

    void size_and_clear() {
        FrameQueue<int> q(10);
        q.push(1); q.push(2); q.push(3);
        QCOMPARE(q.size(), 3);
        q.clear();
        QCOMPARE(q.size(), 0);
    }
};

QTEST_MAIN(TstFrameQueue)
#include "tst_framequeue.moc"
```

- [x] **Step 2: 运行测试，确认 FAIL（类不存在）**

```
qmake tests.pro && nmake
tests.exe   → 编译错误：'FrameQueue' was not declared
```

- [x] **Step 3: 实现 FrameQueue**  ✅ `7 passed, 0 failed, 0 skipped, 252ms`

```cpp
// src/framequeue.h
#pragma once
#include <queue>
#include <QMutex>
#include <QWaitCondition>

template<typename T>
class FrameQueue {
public:
    explicit FrameQueue(int maxSize) : maxSize_(maxSize) {}

    void push(T item) {
        QMutexLocker lk(&mutex_);
        while (!aborted_ && (int)q_.size() >= maxSize_)
            notFull_.wait(&mutex_);
        if (aborted_) return;
        q_.push(std::move(item));
        notEmpty_.wakeOne();
    }

    // returns false on timeout or abort
    bool tryPop(T& out, int timeoutMs) {
        QMutexLocker lk(&mutex_);
        if (q_.empty() && !aborted_)
            notEmpty_.wait(&mutex_, timeoutMs);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        notFull_.wakeOne();
        return true;
    }

    void clear() {
        QMutexLocker lk(&mutex_);
        while (!q_.empty()) q_.pop();
        notFull_.wakeAll();
    }

    void abort() {
        QMutexLocker lk(&mutex_);
        aborted_ = true;
        notEmpty_.wakeAll();
        notFull_.wakeAll();
    }

    void reset() {
        QMutexLocker lk(&mutex_);
        aborted_ = false;
        while (!q_.empty()) q_.pop();
    }

    int size() const {
        QMutexLocker lk(&mutex_);
        return (int)q_.size();
    }

private:
    mutable QMutex mutex_;
    QWaitCondition notEmpty_;
    QWaitCondition notFull_;
    std::queue<T> q_;
    int maxSize_;
    bool aborted_ = false;
};
```

- [x] **Step 4: 运行测试，确认全部 PASS**

```
tests.exe
PASS   TstFrameQueue::pushPop_singleThread
PASS   TstFrameQueue::tryPop_returnsFlase_whenEmpty
PASS   TstFrameQueue::blocksAt_maxSize
PASS   TstFrameQueue::abort_unblocks_pop
PASS   TstFrameQueue::size_and_clear
```

- [x] **Step 5: Commit**

```bash
git add src/framequeue.h tests/tst_framequeue.cpp
git commit -m "feat: FrameQueue 线程安全队列"
```

---

## Task 3: AVSync（音频时钟 + 视频延迟）

**Files:**
- Create: `src/avsync.h`
- Create: `src/avsync.cpp`
- Create: `tests/tst_avsync.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/tst_avsync.cpp
#include <QtTest>
#include "avsync.h"

class TstAVSync : public QObject {
    Q_OBJECT
private slots:
    void defaultClockIsNegative() {
        AVSync s;
        QVERIFY(s.audioClock() < 0.0);
    }

    void setAndGetClock() {
        AVSync s;
        s.setAudioClock(1.234);
        QVERIFY(qAbs(s.audioClock() - 1.234) < 1e-9);
    }

    // 视频帧 pts 等于音频时钟 → 延迟接近 0
    void videoDelay_onTime() {
        AVSync s;
        s.setAudioClock(2.0);
        double d = s.videoDelay(2.0);
        QVERIFY(d >= 0.0 && d < 10.0); // ≈ 0 ms
    }

    // 视频帧领先音频 → 正延迟（需等待）
    void videoDelay_videoAhead() {
        AVSync s;
        s.setAudioClock(1.0);
        double d = s.videoDelay(1.1); // 视频领先 100 ms
        QVERIFY(d > 80.0 && d < 120.0);
    }

    // 视频帧落后音频超过阈值 → 返回 0（立即丢帧）
    void videoDelay_videoLate() {
        AVSync s;
        s.setAudioClock(2.0);
        double d = s.videoDelay(1.5); // 落后 500 ms
        QCOMPARE(d, 0.0);
    }
};

QTEST_MAIN(TstAVSync)
#include "tst_avsync.moc"
```

- [ ] **Step 2: 运行，确认 FAIL（类不存在）**

- [ ] **Step 3: 实现 AVSync**

```cpp
// src/avsync.h
#pragma once
#include <atomic>

class AVSync {
public:
    void setAudioClock(double pts);  // 单位：秒
    double audioClock() const;
    // 返回视频渲染前应等待的毫秒数；落后超过 DROP_THRESHOLD 则返回 0
    double videoDelay(double videoPts) const;
private:
    std::atomic<double> clock_{-1.0};
    static constexpr double DROP_THRESHOLD = 0.4; // 秒
};
```

```cpp
// src/avsync.cpp
#include "avsync.h"
#include <algorithm>

void AVSync::setAudioClock(double pts) {
    clock_.store(pts, std::memory_order_relaxed);
}

double AVSync::audioClock() const {
    return clock_.load(std::memory_order_relaxed);
}

double AVSync::videoDelay(double videoPts) const {
    double ac = audioClock();
    if (ac < 0.0) return 0.0;
    double diff = videoPts - ac; // 正：视频领先，负：视频落后
    if (diff < -DROP_THRESHOLD) return 0.0;           // 丢帧
    if (diff < 0.0) return 0.0;                       // 轻微落后，立即显示
    return diff * 1000.0;                             // ms
}
```

- [ ] **Step 4: 运行测试，确认全部 PASS**

- [ ] **Step 5: Commit**

```bash
git add src/avsync.h src/avsync.cpp tests/tst_avsync.cpp
git commit -m "feat: AVSync 音频时钟与视频延迟计算"
```

---

## Task 4: DemuxThread（解复用线程）

**Files:**
- Create: `src/demuxthread.h`
- Create: `src/demuxthread.cpp`
- Create: `tests/tst_demuxthread.cpp`
- Require: `tests/data/sample.mp4`（2 秒测试视频，自行准备或用 ffmpeg 生成）

生成测试视频命令（在终端执行一次）：
```bash
ffmpeg -f lavfi -i "testsrc=duration=2:size=320x240:rate=25" \
       -f lavfi -i "sine=frequency=440:duration=2" \
       -c:v libx264 -c:a aac -shortest tests/data/sample.mp4
```

- [ ] **Step 1: 写失败测试**

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
        // 清理队列中的包
        AVPacket* pkt;
        while (vq.tryPop(pkt, 0)) av_packet_free(&pkt);
        while (aq.tryPop(pkt, 0)) av_packet_free(&pkt);
    }
};

QTEST_MAIN(TstDemuxThread)
#include "tst_demuxthread.moc"
```

- [ ] **Step 2: 运行，确认 FAIL**

- [ ] **Step 3: 实现 DemuxThread 头文件**

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
    void seek(double seconds);

    int64_t duration() const { return duration_; }  // 微秒
    int videoStreamIdx() const { return videoIdx_; }
    int audioStreamIdx() const { return audioIdx_; }
    AVFormatContext* formatContext() const { return fmtCtx_; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVFormatContext* fmtCtx_ = nullptr;
    FrameQueue<AVPacket*>* videoQueue_ = nullptr;
    FrameQueue<AVPacket*>* audioQueue_ = nullptr;
    int videoIdx_ = -1;
    int audioIdx_ = -1;
    int64_t duration_ = 0;
    std::atomic<bool> abort_{false};
    std::atomic<double> seekTarget_{-1.0};

    void handleSeek();
};
```

- [ ] **Step 4: 实现 DemuxThread 源文件**

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
    if (avformat_open_input(&fmtCtx_, path.toUtf8().constData(),
                             nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0)
        return false;

    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        auto type = fmtCtx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoIdx_ < 0) videoIdx_ = (int)i;
        if (type == AVMEDIA_TYPE_AUDIO && audioIdx_ < 0) audioIdx_ = (int)i;
    }
    duration_ = fmtCtx_->duration; // AV_TIME_BASE 单位（微秒）
    videoQueue_ = videoQueue;
    audioQueue_ = audioQueue;
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
    videoQueue_->clear();
    audioQueue_->clear();
}

void DemuxThread::run() {
    AVPacket* pkt = av_packet_alloc();
    while (!abort_) {
        handleSeek();
        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret < 0) break; // EOF 或错误
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

- [ ] **Step 5: 运行测试，确认全部 PASS**

- [ ] **Step 6: Commit**

```bash
git add src/demuxthread.h src/demuxthread.cpp tests/tst_demuxthread.cpp tests/data/sample.mp4
git commit -m "feat: DemuxThread 解复用线程"
```

---

## Task 5: VideoDecodeThread（视频解码线程）

**Files:**
- Create: `src/videodecodethread.h`
- Create: `src/videodecodethread.cpp`

此组件依赖真实 FFmpeg 解码，集成测试在 Task 8（PlayerController）中覆盖。
此处仅做编译验证。

- [ ] **Step 1: 实现头文件**

```cpp
// src/videodecodethread.h
#pragma once
#include <QThread>
#include <atomic>
#include "framequeue.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecodeThread : public QThread {
    Q_OBJECT
public:
    ~VideoDecodeThread() override;

    // 调用前需先 open()
    bool init(AVCodecParameters* params);
    void stop();
    void flush(); // Seek 后调用

    void setInputQueue(FrameQueue<AVPacket*>* q)  { inputQueue_  = q; }
    void setOutputQueue(FrameQueue<AVFrame*>* q)  { outputQueue_ = q; }

    int width()  const;
    int height() const;
    AVRational timeBase() const;

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext* codecCtx_ = nullptr;
    FrameQueue<AVPacket*>* inputQueue_ = nullptr;
    FrameQueue<AVFrame*>* outputQueue_ = nullptr;
    std::atomic<bool> abort_{false};
    std::atomic<bool> flush_{false};
    AVRational timeBase_{1, 1};
};
```

- [ ] **Step 2: 实现源文件**

```cpp
// src/videodecodethread.cpp
#include "videodecodethread.h"

extern "C" {
#include <libavutil/avutil.h>
}

VideoDecodeThread::~VideoDecodeThread() {
    stop(); wait();
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool VideoDecodeThread::init(AVCodecParameters* params) {
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;
    return avcodec_open2(codecCtx_, codec, nullptr) >= 0;
}

void VideoDecodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
    if (outputQueue_) outputQueue_->abort();
}

void VideoDecodeThread::flush() { flush_ = true; }

int VideoDecodeThread::width()  const { return codecCtx_ ? codecCtx_->width  : 0; }
int VideoDecodeThread::height() const { return codecCtx_ ? codecCtx_->height : 0; }
AVRational VideoDecodeThread::timeBase() const { return timeBase_; }

void VideoDecodeThread::run() {
    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    while (!abort_) {
        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            outputQueue_->clear();
        }
        if (!inputQueue_->tryPop(pkt, 20)) continue;
        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);
        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            AVFrame* out = av_frame_clone(frame);
            outputQueue_->push(out);
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    emit finished();
}
```

- [ ] **Step 3: 编译确认无错**

- [ ] **Step 4: Commit**

```bash
git add src/videodecodethread.h src/videodecodethread.cpp
git commit -m "feat: VideoDecodeThread 视频解码线程"
```

---

## Task 6: AudioDecodeThread（音频解码 + QAudioSink 推流）

**Files:**
- Create: `src/audiodecodethread.h`
- Create: `src/audiodecodethread.cpp`

- [ ] **Step 1: 实现头文件**

```cpp
// src/audiodecodethread.h
#pragma once
#include <QThread>
#include <QAudioSink>
#include <QAudioFormat>
#include <atomic>
#include "framequeue.h"
#include "avsync.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

class AudioDecodeThread : public QThread {
    Q_OBJECT
public:
    ~AudioDecodeThread() override;

    bool init(AVCodecParameters* params, AVRational timeBase, AVSync* sync);
    void stop();
    void flush();
    void setVolume(float v);  // 0.0–1.0

    void setInputQueue(FrameQueue<AVPacket*>* q) { inputQueue_ = q; }

signals:
    void finished();

protected:
    void run() override;

private:
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    QAudioSink* sink_ = nullptr;
    QIODevice* device_ = nullptr;
    AVSync* sync_ = nullptr;
    AVRational timeBase_{1, 1};
    FrameQueue<AVPacket*>* inputQueue_ = nullptr;
    std::atomic<bool> abort_{false};
    std::atomic<bool> flush_{false};
    std::atomic<float> pendingVolume_{-1.f};
};
```

- [ ] **Step 2: 实现源文件**

```cpp
// src/audiodecodethread.cpp
#include "audiodecodethread.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

AudioDecodeThread::~AudioDecodeThread() {
    stop(); wait();
    if (sink_)     { sink_->stop(); delete sink_; }
    if (swrCtx_)   swr_free(&swrCtx_);
    if (codecCtx_) avcodec_free_context(&codecCtx_);
}

bool AudioDecodeThread::init(AVCodecParameters* params,
                              AVRational timeBase,
                              AVSync* sync) {
    timeBase_ = timeBase;
    sync_ = sync;

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx_, params) < 0) return false;
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) return false;

    // 重采样到 S16 Stereo 44100
    swrCtx_ = swr_alloc();
    av_opt_set_int(swrCtx_, "in_channel_layout",  codecCtx_->channel_layout, 0);
    av_opt_set_int(swrCtx_, "in_sample_rate",     codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt", codecCtx_->sample_fmt, 0);
    av_opt_set_int(swrCtx_, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(swrCtx_, "out_sample_rate",    44100, 0);
    av_opt_set_sample_fmt(swrCtx_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    if (swr_init(swrCtx_) < 0) return false;

    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    sink_ = new QAudioSink(fmt);
    return true;
}

void AudioDecodeThread::stop() {
    abort_ = true;
    if (inputQueue_) inputQueue_->abort();
}

void AudioDecodeThread::flush() { flush_ = true; }

void AudioDecodeThread::setVolume(float v) { pendingVolume_.store(v); }

void AudioDecodeThread::run() {
    device_ = sink_->start();
    AVPacket* pkt = nullptr;
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;

    while (!abort_) {
        float vol = pendingVolume_.exchange(-1.f);
        if (vol >= 0.f) sink_->setVolume(vol);

        if (flush_.exchange(false)) {
            avcodec_flush_buffers(codecCtx_);
            swr_convert(swrCtx_, nullptr, 0, nullptr, 0); // flush swr
        }

        if (!inputQueue_->tryPop(pkt, 20)) continue;

        if (avcodec_send_packet(codecCtx_, pkt) < 0) {
            av_packet_free(&pkt); continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            int outSamples = av_rescale_rnd(
                swr_get_delay(swrCtx_, codecCtx_->sample_rate) + frame->nb_samples,
                44100, codecCtx_->sample_rate, AV_ROUND_UP);

            int needed = outSamples * 2 * 2; // stereo S16
            if (needed > outBufSize) {
                av_free(outBuf);
                outBuf = (uint8_t*)av_malloc(needed);
                outBufSize = needed;
            }
            uint8_t* out[1] = { outBuf };
            int n = swr_convert(swrCtx_, out, outSamples,
                                (const uint8_t**)frame->data, frame->nb_samples);
            if (n > 0 && device_)
                device_->write((const char*)outBuf, n * 4);

            // 更新音频时钟
            if (frame->pts != AV_NOPTS_VALUE) {
                double pts = frame->pts * av_q2d(timeBase_)
                           + (double)frame->nb_samples / codecCtx_->sample_rate;
                sync_->setAudioClock(pts);
            }
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    av_free(outBuf);
    sink_->stop();
    emit finished();
}
```

- [ ] **Step 3: 编译确认无错**

- [ ] **Step 4: Commit**

```bash
git add src/audiodecodethread.h src/audiodecodethread.cpp
git commit -m "feat: AudioDecodeThread 音频解码与 QAudioSink 推流"
```

---

## Task 7: VideoRenderer（QPainter 渲染 + 帧定时）

**Files:**
- Create: `src/videorenderer.h`
- Create: `src/videorenderer.cpp`

- [ ] **Step 1: 实现头文件**

```cpp
// src/videorenderer.h
#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include "framequeue.h"
#include "avsync.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

class VideoRenderer : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderer(QWidget* parent = nullptr);
    ~VideoRenderer() override;

    void init(int width, int height, AVRational timeBase,
              AVSync* sync, FrameQueue<AVFrame*>* frameQueue);
    void startRendering();
    void stopRendering();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTimer();

private:
    QImage currentFrame_;
    QMutex frameMutex_;
    QTimer* timer_ = nullptr;
    AVSync* sync_ = nullptr;
    FrameQueue<AVFrame*>* frameQueue_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    int srcW_ = 0, srcH_ = 0;
    AVRational timeBase_{1, 1};
};
```

- [ ] **Step 2: 实现源文件**

```cpp
// src/videorenderer.cpp
#include "videorenderer.h"
#include <QPainter>
#include <QThread>

extern "C" {
#include <libavutil/imgutils.h>
}

VideoRenderer::VideoRenderer(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setStyleSheet("background: black;");
    timer_ = new QTimer(this);
    timer_->setInterval(1);
    connect(timer_, &QTimer::timeout, this, &VideoRenderer::onTimer);
}

VideoRenderer::~VideoRenderer() {
    stopRendering();
    if (swsCtx_) sws_freeContext(swsCtx_);
}

void VideoRenderer::init(int width, int height, AVRational timeBase,
                          AVSync* sync, FrameQueue<AVFrame*>* frameQueue) {
    srcW_ = width; srcH_ = height;
    timeBase_ = timeBase;
    sync_ = sync;
    frameQueue_ = frameQueue;
    swsCtx_ = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                              width, height, AV_PIX_FMT_RGB32,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    currentFrame_ = QImage(width, height, QImage::Format_RGB32);
}

void VideoRenderer::startRendering() { timer_->start(); }
void VideoRenderer::stopRendering()  { timer_->stop(); }

void VideoRenderer::onTimer() {
    AVFrame* frame = nullptr;
    if (!frameQueue_ || !frameQueue_->tryPop(frame, 0)) return;

    double pts = (frame->pts != AV_NOPTS_VALUE)
                 ? frame->pts * av_q2d(timeBase_) : 0.0;
    double delay = sync_ ? sync_->videoDelay(pts) : 0.0;

    if (delay > 2.0) {
        // 领先太多，把帧放回（简化：直接重新 push）
        frameQueue_->push(frame);
        return;
    }
    if (delay > 0.0) {
        QThread::msleep((unsigned long)delay);
    }

    // YUV → RGB32
    uint8_t* dst[1]   = { currentFrame_.bits() };
    int dstStride[1]  = { currentFrame_.bytesPerLine() };
    {
        QMutexLocker lk(&frameMutex_);
        sws_scale(swsCtx_,
                  (const uint8_t* const*)frame->data, frame->linesize,
                  0, srcH_, dst, dstStride);
    }
    av_frame_free(&frame);
    update();
}

void VideoRenderer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QMutexLocker lk(&frameMutex_);
    if (!currentFrame_.isNull()) {
        QRect r = rect();
        // 保持宽高比居中
        double aspect = (double)srcW_ / srcH_;
        int w = r.width(), h = (int)(w / aspect);
        if (h > r.height()) { h = r.height(); w = (int)(h * aspect); }
        int x = (r.width() - w) / 2, y = (r.height() - h) / 2;
        p.fillRect(r, Qt::black);
        p.drawImage(QRect(x, y, w, h), currentFrame_);
    } else {
        p.fillRect(rect(), Qt::black);
    }
}
```

- [ ] **Step 3: 编译确认无错**

- [ ] **Step 4: Commit**

```bash
git add src/videorenderer.h src/videorenderer.cpp
git commit -m "feat: VideoRenderer QPainter 帧渲染与音视频同步"
```

---

## Task 8: PlayerController（组合所有组件）

**Files:**
- Create: `src/playercontroller.h`
- Create: `src/playercontroller.cpp`

- [ ] **Step 1: 实现头文件**

```cpp
// src/playercontroller.h
#pragma once
#include <QObject>
#include <atomic>
#include "framequeue.h"
#include "avsync.h"
#include "demuxthread.h"
#include "videodecodethread.h"
#include "audiodecodethread.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoRenderer;

class PlayerController : public QObject {
    Q_OBJECT
public:
    explicit PlayerController(VideoRenderer* renderer, QObject* parent = nullptr);
    ~PlayerController() override;

    bool open(const QString& path);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setVolume(float v);  // 0.0–1.0

    int64_t duration() const;        // 毫秒
    bool isPlaying()   const { return playing_; }

signals:
    void durationChanged(int64_t ms);
    void positionChanged(int64_t ms); // 由 timer 触发，100 ms 间隔
    void playbackFinished();

private slots:
    void onDemuxFinished();
    void updatePosition();

private:
    VideoRenderer* renderer_;
    AVSync sync_;
    DemuxThread demux_;
    VideoDecodeThread videoDec_;
    AudioDecodeThread audioDec_;
    FrameQueue<AVPacket*> videoPacketQ_{100};
    FrameQueue<AVPacket*> audioPacketQ_{400};
    FrameQueue<AVFrame*>  videoFrameQ_{15};
    QTimer* posTimer_ = nullptr;
    std::atomic<bool> playing_{false};

    void stopAllThreads();
};
```

- [ ] **Step 2: 实现源文件**

```cpp
// src/playercontroller.cpp
#include "playercontroller.h"
#include "videorenderer.h"
#include <QTimer>

extern "C" {
#include <libavformat/avformat.h>
}

PlayerController::PlayerController(VideoRenderer* renderer, QObject* parent)
    : QObject(parent), renderer_(renderer) {
    posTimer_ = new QTimer(this);
    posTimer_->setInterval(100);
    connect(posTimer_, &QTimer::timeout, this, &PlayerController::updatePosition);
    connect(&demux_, &DemuxThread::finished, this, &PlayerController::onDemuxFinished);
}

PlayerController::~PlayerController() { stop(); }

bool PlayerController::open(const QString& path) {
    stop();
    videoPacketQ_.reset(); audioPacketQ_.reset(); videoFrameQ_.reset();

    if (!demux_.open(path, &videoPacketQ_, &audioPacketQ_))
        return false;

    AVFormatContext* fmt = demux_.formatContext();
    int vi = demux_.videoStreamIdx();
    int ai = demux_.audioStreamIdx();

    if (vi >= 0) {
        AVCodecParameters* vp = fmt->streams[vi]->codecpar;
        AVRational vtb = fmt->streams[vi]->time_base;
        if (!videoDec_.init(vp)) return false;
        videoDec_.setInputQueue(&videoPacketQ_);
        videoDec_.setOutputQueue(&videoFrameQ_);
        renderer_->init(vp->width, vp->height, vtb, &sync_, &videoFrameQ_);
    }
    if (ai >= 0) {
        AVCodecParameters* ap = fmt->streams[ai]->codecpar;
        AVRational atb = fmt->streams[ai]->time_base;
        if (!audioDec_.init(ap, atb, &sync_)) return false;
        audioDec_.setInputQueue(&audioPacketQ_);
    }

    emit durationChanged(duration());
    return true;
}

void PlayerController::play() {
    if (playing_) return;
    playing_ = true;
    demux_.start();
    videoDec_.start();
    audioDec_.start();
    renderer_->startRendering();
    posTimer_->start();
}

void PlayerController::pause() {
    // 简化实现：暂停/继续通过 stop/play 重新 seek 当前位置
    // 生产级实现需在各线程中添加暂停标志
    playing_ = false;
    posTimer_->stop();
    renderer_->stopRendering();
}

void PlayerController::stop() {
    playing_ = false;
    posTimer_->stop();
    renderer_->stopRendering();
    stopAllThreads();
}

void PlayerController::seek(double seconds) {
    demux_.seek(seconds);
    videoDec_.flush();
    audioDec_.flush();
}

void PlayerController::setVolume(float v) { audioDec_.setVolume(v); }

int64_t PlayerController::duration() const {
    return demux_.duration() / 1000; // 微秒 → 毫秒
}

void PlayerController::stopAllThreads() {
    demux_.stop();   demux_.wait(3000);
    videoDec_.stop(); videoDec_.wait(3000);
    audioDec_.stop(); audioDec_.wait(3000);
    videoPacketQ_.clear(); audioPacketQ_.clear(); videoFrameQ_.clear();
}

void PlayerController::onDemuxFinished() {
    // 等待解码线程耗尽队列后发出播放结束信号
    QTimer::singleShot(500, this, [this]{ emit playbackFinished(); });
}

void PlayerController::updatePosition() {
    double ac = sync_.audioClock();
    if (ac >= 0.0)
        emit positionChanged((int64_t)(ac * 1000.0));
}
```

- [ ] **Step 3: 编译确认无错**

- [ ] **Step 4: Commit**

```bash
git add src/playercontroller.h src/playercontroller.cpp
git commit -m "feat: PlayerController 组合解复用/解码/渲染组件"
```

---

## Task 9: MainWindow UI（进度条 + 音量 + 全屏）

**Files:**
- Create: `src/mainwindow.h`
- Create: `src/mainwindow.cpp`
- Create: `src/mainwindow.ui`

- [ ] **Step 1: 设计 .ui 布局（Qt Designer）**

用 Qt Designer 创建 `mainwindow.ui`，布局如下：

```
┌─────────────────────────────────────────────────┐
│  MenuBar: 文件 → 打开                             │
├─────────────────────────────────────────────────┤
│                                                 │
│           VideoRenderer (centralWidget)         │
│                                                 │
├─────────────────────────────────────────────────┤
│  [▶/⏸]  ───────────────●─────── 00:00 / 00:00  │
│          progressSlider                         │
│  🔊 ──────● volumeSlider                        │
└─────────────────────────────────────────────────┘
```

关键控件 objectName：`playPauseBtn`、`progressSlider`、`volumeSlider`、`timeLabel`。

- [ ] **Step 2: 实现头文件**

```cpp
// src/mainwindow.h
#pragma once
#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class VideoRenderer;
class PlayerController;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private slots:
    void onOpenFile();
    void onPlayPause();
    void onSeekSliderMoved(int value);
    void onVolumeChanged(int value);
    void onDurationChanged(int64_t ms);
    void onPositionChanged(int64_t ms);
    void onPlaybackFinished();

private:
    Ui::MainWindow* ui;
    VideoRenderer* renderer_;
    PlayerController* player_;
    int64_t duration_ = 0;
    bool seeking_ = false;
    bool isFullscreen_ = false;

    static QString formatTime(int64_t ms);
};
```

- [ ] **Step 3: 实现源文件**

```cpp
// src/mainwindow.cpp
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "videorenderer.h"
#include "playercontroller.h"
#include <QFileDialog>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);

    // VideoRenderer 作为中心部件
    renderer_ = new VideoRenderer(this);
    setCentralWidget(renderer_);

    // 将控制栏作为停靠区或底部 widget，根据 .ui 设计调整
    player_ = new PlayerController(renderer_, this);

    ui->progressSlider->setRange(0, 1000);
    ui->volumeSlider->setRange(0, 100);
    ui->volumeSlider->setValue(80);
    player_->setVolume(0.8f);

    connect(ui->actionOpen,     &QAction::triggered,          this, &MainWindow::onOpenFile);
    connect(ui->playPauseBtn,   &QPushButton::clicked,        this, &MainWindow::onPlayPause);
    connect(ui->progressSlider, &QSlider::sliderMoved,        this, &MainWindow::onSeekSliderMoved);
    connect(ui->volumeSlider,   &QSlider::valueChanged,       this, &MainWindow::onVolumeChanged);

    connect(player_, &PlayerController::durationChanged, this, &MainWindow::onDurationChanged);
    connect(player_, &PlayerController::positionChanged, this, &MainWindow::onPositionChanged);
    connect(player_, &PlayerController::playbackFinished,this, &MainWindow::onPlaybackFinished);
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::onOpenFile() {
    QString path = QFileDialog::getOpenFileName(
        this, "打开文件", "",
        "视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)");
    if (path.isEmpty()) return;
    player_->stop();
    if (player_->open(path))
        player_->play();
}

void MainWindow::onPlayPause() {
    if (player_->isPlaying()) {
        player_->pause();
        ui->playPauseBtn->setText("▶");
    } else {
        player_->play();
        ui->playPauseBtn->setText("⏸");
    }
}

void MainWindow::onSeekSliderMoved(int value) {
    if (duration_ <= 0) return;
    double seconds = (double)value / 1000.0 * duration_ / 1000.0;
    player_->seek(seconds);
}

void MainWindow::onVolumeChanged(int value) {
    player_->setVolume(value / 100.0f);
}

void MainWindow::onDurationChanged(int64_t ms) {
    duration_ = ms;
    ui->timeLabel->setText("00:00 / " + formatTime(ms));
}

void MainWindow::onPositionChanged(int64_t ms) {
    if (!ui->progressSlider->isSliderDown() && duration_ > 0) {
        ui->progressSlider->setValue((int)((double)ms / duration_ * 1000));
    }
    ui->timeLabel->setText(formatTime(ms) + " / " + formatTime(duration_));
}

void MainWindow::onPlaybackFinished() {
    ui->playPauseBtn->setText("▶");
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent*) {
    isFullscreen_ = !isFullscreen_;
    isFullscreen_ ? showFullScreen() : showNormal();
}

QString MainWindow::formatTime(int64_t ms) {
    int s = (int)(ms / 1000);
    return QString("%1:%2").arg(s / 60, 2, 10, QChar('0'))
                           .arg(s % 60, 2, 10, QChar('0'));
}
```

- [ ] **Step 4: 编译并运行，手动测试**

1. 打开一个 .mp4 文件
2. 确认视频/音频同步播放
3. 拖动进度条 Seek 后能继续播放
4. 音量滑块调整音量有效
5. 双击进入/退出全屏

- [ ] **Step 5: Commit**

```bash
git add src/mainwindow.h src/mainwindow.cpp src/mainwindow.ui
git commit -m "feat: MainWindow UI 进度条/音量/全屏"
```

---

## Task 10: 集成验证与收尾

- [ ] **Step 1: 运行所有单元测试**

```bash
cd tests && qmake tests.pro && nmake && tests.exe
# 期望：所有 PASS，0 FAIL
```

- [ ] **Step 2: 端对端播放测试清单**

| 场景 | 预期结果 |
|------|----------|
| 打开 1080p H.264 mp4 | 视频正常显示，音频同步 |
| 打开仅含视频流的 .avi | 视频正常显示，无崩溃 |
| 拖动进度条到中间 | Seek 后继续播放，无卡顿 |
| 拖动到末尾 | playbackFinished 触发，按钮变 ▶ |
| 音量设为 0 | 静音 |
| 双击 → 全屏 → 双击 → 窗口 | 正常切换 |
| 关闭窗口（播放中） | 无崩溃、无内存泄漏告警 |

- [ ] **Step 3: 用 AddressSanitizer（可选）检查内存问题**

在 .pro 中添加 `QMAKE_CXXFLAGS += -fsanitize=address`（MSVC 需用 `/fsanitize=address`），运行一次完整播放，确认无报告。

- [ ] **Step 4: 最终 Commit**

```bash
git add .
git commit -m "feat: RambosPlayer 初级功能完成（基础播放器 + 完整 UI）"
```

---

## 已知局限与后续计划

| 局限 | 说明 |
|------|------|
| 暂停实现简化 | 当前暂停不挂起线程，需后续添加 pause 标志 |
| Seek 精度 | 使用关键帧 Seek，精确 Seek 需 decode-to-target |
| 无硬件加速 | D3D11VA 见进阶功能1计划 |
| 无滤镜 | libavfilter 见进阶功能2计划 |
| 内存管理 | AVFrame*/AVPacket* 生命周期依赖 clear() 顺序，需仔细验证 |

---

*计划覆盖范围：初级功能1（核心播放器）+ 初级功能2（完整 UI）*
*进阶功能（硬件加速 / 滤镜 / 推流 / 剪辑）各自独立，另建计划*
