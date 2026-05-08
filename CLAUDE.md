# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RambosPlayer is a multithreaded multimedia player built with C++17, Qt 5.14.2, and FFmpeg 4.x. The goal is to learn audio/video sync, hardware decoding, and real-time streaming. The implementation plan covers 10 phases; **no source code exists yet** — all phases are pending.

- Master plan (all 10 phases): `docs/DEVPLAN.md`
- Detailed TDD execution plan (Phase 1–6, with full code): `docs/superpowers/plans/2026-04-26-rambos-player-core.md`

## Environment

| Item | Value |
|------|-------|
| IDE | Visual Studio 2017 |
| UI | Qt 5.14.2 |
| FFmpeg | vcpkg, path `D:\vcpkg\installed\x64-windows` |
| Build system | qmake → nmake (or open .sln in VS2017) |

## Build & Test Commands

```bash
# Generate VS solution from qmake project
qmake RambosPlayer.pro

# Build (from VS2017 Developer Command Prompt)
nmake

# Build and run tests
cd tests
qmake tests.pro
nmake
tests.exe
```

To run a single test class, pass the class name:
```bash
tests.exe TstFrameQueue
tests.exe TstAVSync
tests.exe TstDemuxThread
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
