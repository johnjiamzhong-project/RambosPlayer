# Phase 13 音频混合 — 代码审核报告

**审核日期:** 2026/05/31  
**审核范围:** `audiomixpanel.h/.cpp/.ui`, `audiomixworker.h/.cpp`, `timeline.h/.cpp` 修改, `mainwindow.h/.cpp` 集成, `CMakeLists.txt`, `tests/CMakeLists.txt`  
**状态:** 编译通过，待用户测试验收

---

## 一、架构评价

整体设计清晰，职责划分合理：

| 组件 | 职责 | 评价 |
|------|------|------|
| `AudioMixPanel` | UI 交互 + 录音采集 + 试播放 | ✅ 单一职责，不碰 FFmpeg 编解码 |
| `AudioMixWorker` | FFmpeg 混音 + 封装（QThread） | ✅ 两步算法清晰：混音→封装 |
| `Timeline` 扩展 | 紫色音频区间行 | ✅ 最小侵入，仅新增 `setAudioRegions()` + `drawBottomBar()` 末尾追加 |
| `MainWindow` 集成 | Dock + 菜单 + openFile 联动 | ✅ 复用已有的自定义标题栏模式 |

**持续混合策略**值得肯定：`sourceFile_` 始终指向原始源视频，每次导出从原始源重新混合所有区间，避免多代 AAC 编解码质量损失。

---

## 二、问题清单

### 🔴 高优先级（建议修复后再验收）

#### 1. `execMuxFinal` 先写全部视频包再写全部音频包 — 交错问题

`audiomixworker.cpp:450-467`

```cpp
// 写入所有视频包
while (av_read_frame(videoCtx, pkt) >= 0) { ... av_interleaved_write_frame(...); }
// 写入所有音频包
while (av_read_frame(audioCtx, pkt) >= 0) { ... av_interleaved_write_frame(...); }
```

当前实现先消耗完所有视频包，再消耗所有音频包。`av_interleaved_write_frame` 内部有交错缓冲（通常 1 秒），但对于长视频（>几秒），音频包全部排在视频包之后，可能导致：
- 播放器在视频播放初期无音频数据可读
- 某些严格解复用器报交错错误

**建议：** 改为双源交替读取，或使用 `av_interleaved_write_frame` 的自动交错特性（当前已使用，短期视频可能无感知问题，但长视频风险增大）。至少应在验收时用 >30 秒视频验证音画同步。

#### 2. `probeDurationUs` 未检查 `avformat_find_stream_info` 返回值

`audiomixpanel.cpp:489`

```cpp
avformat_find_stream_info(ctx, nullptr);  // 返回值被忽略
int64_t dur = ctx->duration;
```

若 `avformat_find_stream_info` 失败，`ctx->duration` 可能为 `AV_NOPTS_VALUE`（即 `INT64_MIN`），后续 `dur - offsetUs` 产生未定义行为。

**建议：**
```cpp
if (avformat_find_stream_info(ctx, nullptr) < 0) {
    avformat_close_input(&ctx);
    return 0;
}
```

#### 3. `prevVolume_` 始终为 1.0f — 录音结束后音量恢复不正确

`audiomixpanel.cpp:285`

```cpp
prevVolume_ = 1.0f;  // PlayerController 无 volume() getter，默认 1.0
```

`PlayerController` 缺少 `volume()` getter，录音前无法保存实际音量。用户在 50% 音量下开始录音，停止后音量会被恢复到 100%。

**建议：** 在 `PlayerController` 中添加 `float volume() const` getter，或从 `MainWindow` 的 `volumeSlider->value()` 传递。

---

### 🟡 中优先级（建议改进）

#### 4. `execMixAudio` 中存在重复的 drain 代码块

`audiomixworker.cpp:311-331` 和 `335-347` 是几乎完全相同的"从 sink 取帧并编码"逻辑。

**建议：** 提取为私有辅助函数 `drainAndEncode()`，减少重复。

#### 5. `writeWav` 的 `dataSize` 使用 `int32_t` — 大文件溢出

`audiomixpanel.cpp:460`

```cpp
int32_t dataSize = pcm.size();
```

`QByteArray::size()` 返回 `int`（最大 ~2GB），`int32_t` 在 PCM 数据超过 2GB 时溢出。虽然实时录音不太可能达到此长度，但作为通用函数应有防护。

**建议：** 添加 `if (pcm.size() > INT32_MAX - 36) return false;` 守卫。

#### 6. `AudioMixWorker` 析构只 wait 3 秒

`audiomixpanel.cpp:77`

```cpp
if (worker_->isRunning()) { worker_->requestInterruption(); worker_->wait(3000); }
```

`AudioMixWorker::run()` 没有检查 `isInterruptionRequested()`，`requestInterruption()` 实际无效。若 FFmpeg 操作卡住，3 秒后强制销毁 QThread，FFmpeg 资源（AVFormatContext 等）可能泄漏。

**建议：** 在 `execMixAudio` 和 `execMuxFinal` 的主循环中添加 `if (isInterruptionRequested()) goto done;` 检查，或改用更长的 wait 时间。

#### 7. 自定义标题栏代码重复 3 次

`mainwindow.cpp` 中 `mergeDock_`（第 98-141 行）、`audioMixDock_`（第 152-189 行）的自定义标题栏代码几乎完全相同。

**建议：** 提取为 `QDockWidget* createDockWithTitleBar(const QString& title, QWidget* content, QWidget* parent)` 工具函数。

#### 8. 录音 PCM 时长计算截断到整秒

`audiomixpanel.cpp:341`

```cpp
r.audioDurationUs = (pcmData.size() / (44100LL * 2 * 2)) * 1000000LL;
```

整数除法 `pcmData.size() / (44100*2*2)` 截断小数部分。例如 1.7 秒的录音会被计算为 1 秒。

**建议：**
```cpp
r.audioDurationUs = pcmData.size() * 1000000LL / (44100LL * 2 * 2);
```

先乘后除保留精度。

---

### 🟢 低优先级（可选改进）

#### 9. `buildFilterStr` 使用 `sscanf` 解析滤镜名

`audiomixworker.cpp:182`

```cpp
sscanf(cur->name, "in%d", &idx);
```

`sscanf` 不检查缓冲区边界。虽然 `cur->name` 来自 FFmpeg 内部（格式固定为 `in0`/`in1`/...），安全风险极低，但可用 `QString::toInt()` 替代以保持代码风格一致。

#### 10. 试播放时 `stopPreview` 中 `player_->setVolume(1.0f)` 硬编码

`audiomixpanel.cpp:448`

```cpp
player_->setVolume(1.0f);
```

与问题 #3 相关，应恢复到录音/试播放前的实际音量值。

#### 11. `onAudioMixLocalTriggered` / `onAudioMixRecordTriggered` 无文件保护

`mainwindow.cpp:1204-1228`

与 `onTrimModeToggled` 和 `onBrowseClipToggled` 不同，音频混合面板在未打开文件时仍可打开（虽然 `onExport` 有检查）。这不是 bug，但与其他模式的交互一致性略有差异。

---

## 三、代码规范检查

| 检查项 | 结果 |
|--------|------|
| 头文件类注释 | ✅ 每个类前有中文职责说明 |
| 头文件私有成员注释 | ✅ 每个成员有行尾注释 |
| 源文件函数注释 | ✅ 每个函数前有中文注释 |
| UI 控件在 .ui 中定义 | ✅ 不在 .cpp 中动态创建控件 |
| CMakeLists.txt 同步 | ✅ `audiomixworker.cpp` 和 `audiomixpanel.cpp` 已添加 |
| .pro 文件同步 | ❓ 未检查 `.pro` 文件（项目使用 CMake，若仍维护 .pro 需同步） |

---

## 四、测试建议

验收时建议按以下顺序测试：

1. **基本功能**：打开视频 → 工具→音频混合→本地音频 → 选 MP3 → 贴入时间=5s → 混音量=30% → 加入列表 → 导出 → `ffprobe` 确认输出
2. **多段叠加**：添加 2-3 段音频到不同时间点 → 再次导出 → 验证所有区间都被混入
3. **长视频交错**：用 >30 秒视频测试导出，确认音画同步（验证问题 #1）
4. **录音模式**：切换到实时录入 → 录制 → 停止 → 导出验证
5. **srcVol 联动**：拖动源音量 Slider，确认混音量自动互补
6. **Timeline 可视化**：确认紫色区间行正确显示
7. **试播放**：选中区间→试播放 → 确认近似同步
8. **音量恢复**：将播放器音量调到 50% → 开始录音 → 停止 → 确认音量恢复到 50%（当前会恢复到 100%，即问题 #3）

---

## 五、总结

| 维度 | 评分 | 说明 |
|------|------|------|
| 架构设计 | ⭐⭐⭐⭐ | 两步算法清晰，持续混合策略正确 |
| 代码质量 | ⭐⭐⭐⭐ | 注释完整，错误处理覆盖全面 |
| 边界处理 | ⭐⭐⭐ | 存在 probeDurationUs 未检查返回值、PCM 时长截断等小问题 |
| 可维护性 | ⭐⭐⭐⭐ | 标题栏代码可复用提取，drain 逻辑可去重 |

**整体评价：** 实现质量良好，架构清晰。高优先级的 3 个问题建议在验收前修复，中低优先级问题可后续迭代处理。
