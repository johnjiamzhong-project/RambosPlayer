# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RambosPlayer is a multithreaded multimedia player built with C++17, Qt 5.14.2, and FFmpeg 4.x. The goal is to learn audio/video sync, hardware decoding, and real-time streaming. The implementation plan covers 10 phases; **no source code exists yet** — all phases are pending.

- Master plan (all 10 phases): `docs/DEVPLAN.md`
- Detailed TDD execution plan (Phase 1–6, with full code): `docs/superpowers/plans/2026-04-26-rambos-player-core.md`

## Rules

- **Qt UI 控件布局**：所有界面控件必须在 `.ui` 文件中用 Qt Designer 绘制，不在 `.cpp` 中用代码动态创建控件，保持布局可视化可调整。`.cpp` 只负责信号连接和逻辑，不做布局。
- **头文件类注释**：每个类定义前必须有简短的中文注释，说明该类的职责、驱动方式和关键行为（3–5 行）。
- **头文件私有成员注释**：`private:` 区块中每个成员变量必须有行尾注释，说明其用途。
- **源文件函数注释**：每个函数定义前必须有一行或多行中文注释，说明该函数的职责与关键行为（非显而易见的逻辑需说明原因）。
- **Bug 修复记录**：开发计划之外发现并修复的问题，修复完成后必须追加到 `docs/BUGFIX-LOG.md`，按已有模板填写日期、现象、根因、修复方案和涉及文件。
- **崩溃排查**：程序崩溃时，**第一步必须读取日志文件**（`<exe>/logs/rambos_*.log`）定位崩溃前最后一条消息；若同目录存在 `crash_*.dmp`，提示用户用 WinDbg / Visual Studio 加载并附上 `.pdb` 查看调用栈。根据日志内容缩小范围后再改代码，不得在未读日志的情况下盲目猜测原因。
- **画 HTML/SVG 图表**：生成 SVG 时必须遵守以下规则，否则会反复报错：
  1. **不用 bash heredoc 传 Python 代码**：heredoc 遇到 SVG 特殊字符会直接报错，必须先用 Write 工具将 Python 脚本写入文件，再用 `python xxx.py` 执行。
  2. **SVG 文本节点必须转义**：`&` → `&amp;`，`<` → `&lt;`，`>` → `&gt;`，重点检查代码片段（如 `avio_closep(&pb)`）和泛型类型名（如 `FrameQueue<T>`）。
  3. **生成后必须验证 XML**：执行 `python -c "import xml.etree.ElementTree as ET; ET.parse('file.svg')"` 确认合法，有错误必须修复后再写入 HTML。
  4. **临时脚本用完即删**：生成结束后删除 `gen_diagram.py` 等中间文件，不留在 `docs/` 目录。

- **Git commit 信息必须用中文描述**
- **commit 时机与内容**：代码由用户自行测试验收后手动触发 commit，Claude 不主动提交。每个模块只提交一次，Claude 的职责是在用户说"提交"之前准备好所有内容：
  1. 代码 + 测试实现完成（用户自行运行测试验收）
  2. 更新进度文档：`readme.md`、`docs/DEVPLAN.md`、`docs/superpowers/README.md`
  3. 更新面试文档：`docs/interview-claude-code-workflow.md`（补充该模块的设计决策、TDD 案例、Q&A）
  4. 用户确认后说"提交"，Claude 执行**一次性统一 commit**，不拆分

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
  └─ videoFrameQueue         ├─ QAudioOutput (PCM out)
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
  audiodecodethread.h / .cpp  # swr_convert → S16 stereo 44100 → QAudioOutput
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
