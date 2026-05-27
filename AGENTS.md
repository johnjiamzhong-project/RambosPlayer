# AGENTS.md

本文件为 Codex 等编码代理提供项目级工作约定。除非用户有明确新指令，否则修改本仓库时遵守以下规则。

## Project Overview

RambosPlayer 是一个基于 C++17、Qt 5.14.2 和 FFmpeg 4.x 的多线程多媒体处理工具箱。项目目标是覆盖 FFmpeg 全链路实战，包括播放、硬件解码、滤镜、推流、本地录制和无损剪辑。

当前仓库已有完整源码、测试、文档和打包脚本，不是空项目。项目历史上使用 Claude Code 开发，`CLAUDE.md` 中的规则仍有参考价值，但其中“no source code exists yet / all phases pending”的项目状态已过期。

## Key Documents

- `readme.md`：项目功能、架构、环境和使用说明。
- `CLAUDE.md`：历史开发约定和 Claude Code 工作流说明。
- `docs/DEVPLAN.md`：阶段计划与任务状态。
- `docs/BUGFIX-LOG.md`：计划外 bug 修复记录。
- `docs/superpowers/README.md`：TDD / Superpowers 相关计划索引。
- `docs/interview-claude-code-workflow.md`：开发过程、架构决策和面试说明。

## Build And Test

项目主要在 Windows + VSCode + CMake Tools 环境下构建。

- Configure：`cmake --preset default`
- Build Debug：`cmake --build build --config Debug`
- Build Release：`cmake --build build --config Release`
- Run tests：`ctest --test-dir build --config Debug --output-on-failure`

关键环境：

- Qt：`E:\Qt\Qt5.14.2\5.14.2\msvc2017_64`
- FFmpeg：vcpkg toolchain，注意实际路径可能在不同文档中出现 `D:\vcpkg` 或 `E:\vcpkg`，以本机 `CMakePresets.json` 和用户环境为准。
- Compiler：MSVC 2017 x64
- CMake：3.16+

## Source Layout

- `src/`：主程序源码、Qt UI、资源和前端播放脚本。
- `tests/`：Qt Test 单元测试和测试数据。
- `docs/`：设计文档、架构图、计划、日志和文章资料。
- `release/`：打包脚本、发布说明和分发依赖。
- `build/`：CMake 构建输出，通常不应手动编辑。
- `logs/`：运行日志，排查崩溃和运行问题时优先查看最新日期。

## Coding Rules

- UI 控件布局必须在 `.ui` 文件中用 Qt Designer 维护，`.cpp` 只负责信号连接和业务逻辑。
- 新增 `.cpp`、`.h` 或 `.ui` 文件时，必须同步更新 `RambosPlayer.pro` 的 `SOURCES`、`HEADERS` 或 `FORMS`，并检查 CMake 配置是否也需要同步。
- 头文件中每个类定义前必须有简短中文注释，说明职责、驱动方式和关键行为。
- 头文件 `private:` 区块中每个成员变量必须有行尾中文注释，说明用途。
- 源文件中每个函数定义前必须有中文注释，说明职责和关键行为；非显而易见的逻辑要解释原因。
- 代码风格优先跟随现有文件，不做无关重构。
- 不要改动用户已有的未提交变更，除非该变更与当前任务直接相关且必须协同修改。

## Bug And Crash Rules

- 开发计划之外发现并修复的问题，修复完成后必须追加到 `docs/BUGFIX-LOG.md`，按已有模板填写日期、现象、根因、修复方案和涉及文件。
- 程序崩溃时，第一步读取 `<exe>/logs/rambos_*.log`，根据崩溃前最后一条日志缩小范围。
- 如果同目录存在 `crash_*.dmp`，提示用户用 WinDbg 或 Visual Studio 加载，并匹配 `.pdb` 查看调用栈。
- 不要在未读日志的情况下直接猜测崩溃原因并改代码。

## Diagram Rules

生成 HTML/SVG 图表时遵守：

- 不使用 bash heredoc 传 Python 代码。
- SVG 文本节点必须转义：`&` -> `&amp;`，`<` -> `&lt;`，`>` -> `&gt;`。
- 生成后必须用 XML parser 验证 SVG 合法性。
- 临时脚本用完即删，不留在 `docs/` 目录。

## Git Rules

- 不主动 commit，只有用户明确说“提交”时才提交。
- 用户验收后统一 commit，不按小步骤拆分。
- commit 信息必须使用中文。
- 提交前确认代码、测试、进度文档和面试文档是否需要同步更新。

