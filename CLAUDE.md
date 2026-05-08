# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RambosPlayer is a multithreaded multimedia player built with C++17, Qt 5.14.2, and FFmpeg 4.x. The goal is to learn audio/video sync, hardware decoding, and real-time streaming. The implementation plan covers 10 phases; **no source code exists yet** — all phases are pending.

- Master plan (all 10 phases): `docs/DEVPLAN.md`
- Detailed TDD execution plan (Phase 1–6, with full code): `docs/superpowers/plans/2026-04-26-rambos-player-core.md`

## Rules

- **Git commit 信息必须用中文描述**
- **模块完成后同步更新文档**：每个 Task/Phase 完成时，在 commit 前同步更新以下三个文件的进度状态：
  - `readme.md` — 功能1进度表中对应任务改为 ✅ 完成
  - `docs/DEVPLAN.md` — 对应 Phase 的 checkbox 全部勾选，"当前进度"末尾标记完成
  - `docs/superpowers/README.md` — 若产出了新的 plan 或 spec 文件，补充到对应表格

## Environment

| Item | Value |
|------|-------|
| IDE | VSCode + CMake Tools 扩展 |
| UI | Qt 5.14.2（`E:\Qt\Qt5.14.2\5.14.2\msvc2017_64`） |
| FFmpeg | vcpkg，toolchain `D:\vcpkg\scripts\buildsystems\vcpkg.cmake` |
| Build system | CMake 3.16+，preset `default`，生成器 Visual Studio 15 2017 x64 |

## Build & Test Commands

```powershell
# Configure（首次或 CMakeLists.txt 修改后）
cmake --preset default

# Build
cmake --build build --config Debug

# Run tests
ctest --test-dir build --config Debug --output-on-failure
```

To run a single test:
```powershell
.\build\Debug\TstFrameQueue.exe
.\build\Debug\TstAVSync.exe
.\build\Debug\TstDemuxThread.exe
```

## Architecture

The pipeline is a producer-consumer chain across QThreads, synchronized through `FrameQueue<T>` bounded queues:

```
DemuxThread
  ├─ videoPacketQueue (FrameQueue<AVPacket*>, cap 100)
  └─ audioPacketQueue (FrameQueue<AVPacket*>, cap 400)
        │
        ▼
VideoDecodeThread          AudioDecodeThread
  └─ videoFrameQueue         ├─ QAudioSink (PCM out)
       (cap 15)              └─ updates AVSync::audioClock()
        │                          │
        └──────── VideoRenderer ◄──┘
                  (QTimer 1ms, QPainter, AVSync-gated)
        │
        └─ PlayerController (owns all components, exposes open/play/pause/stop/seek/setVolume)
        │
        └─ MainWindow (QSlider progress, volume, double-click fullscreen)
```

**Key design rules:**
- Audio clock (`AVSync`) is the master clock. Video frames wait or drop based on `videoDelay(pts)` (drop threshold: 400 ms late).
- `FrameQueue<T>` is the only cross-thread communication mechanism — no direct Qt signal/slot between decode threads.
- `PlayerController` owns all component lifetimes and resets queues on `open()` and `stop()`.
- Seek: `DemuxThread::seek()` stores target atomically; `run()` calls `av_seek_frame` then clears both packet queues; decode threads call `flush()` to reset codec buffers.

## Planned Source Layout

```
src/
  framequeue.h              # header-only, bounded blocking queue
  avsync.h / .cpp           # atomic audio clock, videoDelay()
  demuxthread.h / .cpp      # av_read_frame loop, dispatches by stream index
  videodecodethread.h / .cpp
  audiodecodethread.h / .cpp  # swr_convert → S16 stereo 44100 → QAudioSink
  videorenderer.h / .cpp    # QWidget, sws_scale YUV420P→RGB32, QPainter
  playercontroller.h / .cpp
  mainwindow.h / .cpp / .ui
tests/
  tst_framequeue.cpp
  tst_avsync.cpp
  tst_demuxthread.cpp       # requires tests/data/sample.mp4
```

Generate the test video once with:
```bash
ffmpeg -f lavfi -i "testsrc=duration=2:size=320x240:rate=25" \
       -f lavfi -i "sine=frequency=440:duration=2" \
       -c:v libx264 -c:a aac -shortest tests/data/sample.mp4
```

## Implementation Order

Follow the TDD plan in `docs/superpowers/plans/2026-04-26-rambos-player-core.md` task by task:

1. Project scaffold (`RambosPlayer.pro`, `main.cpp`, `tests/tests.pro`)
2. `FrameQueue<T>` — write tests first, then implement
3. `AVSync` — write tests first, then implement
4. `DemuxThread` — write tests first, then implement
5. `VideoDecodeThread` — compile verification only (integration tested in Task 8)
6. `AudioDecodeThread` — compile verification only
7. `VideoRenderer` — manual integration test
8. `PlayerController` — wires all components
9. `MainWindow` — UI, then manual end-to-end checklist
10. Final integration: run all unit tests + end-to-end checklist
