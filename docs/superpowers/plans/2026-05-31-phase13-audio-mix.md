# Phase 13 — 音频混合（Audio Mix）

**Goal:** 在现有播放器基础上新增"工具 → 音频混合"功能，支持将本地音频或实时录入音频按可调比例混入源视频音频，在 Timeline 可视化区间，支持试播放和多次叠加混合。

**Tech Stack:** C++17, Qt 5.14.2, FFmpeg 4.x（libavfilter amix + adelay）

---

## 文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/audiomixpanel.h/.cpp/.ui` | **新建** | 音频混合主面板（QDockWidget 内容） |
| `src/audiomixworker.h/.cpp` | **新建** | FFmpeg 混音线程（两步：混音→封装） |
| `src/timeline.h/.cpp` | 修改 | 新增紫色音频区间行（底部导轨扩展） |
| `src/mainwindow.ui` | 修改 | 工具菜单增加「音频混合」子菜单 |
| `src/mainwindow.h/.cpp` | 修改 | 新增 `audioMixDock_` / `audioMixPanel_` 及两个 slot |
| `CMakeLists.txt` | 修改 | 追加新源文件 |
| `tests/CMakeLists.txt` | 修复 | 删除失效的 `simplemuxer.cpp` 引用（遗留 bug） |

---

## 架构概览

```
MainWindow
  ├─ 工具 → 音频混合 → 本地音频  ──→ onAudioMixLocalTriggered()
  │                  → 实时录入  ──→ onAudioMixRecordTriggered()
  │
  └─ audioMixDock_ (QDockWidget)
       └─ AudioMixPanel (QWidget)
            ├─ AudioMixWorker (QThread)   混音引擎
            ├─ PlayerController*          播放控制（预览 / 录音静音）
            ├─ Timeline*                  可视化（紫色音频区间行）
            ├─ QAudioInput                录音采集（Qt5::Multimedia）
            └─ QMediaPlayer               试播放附加音频
```

---

## 核心数据结构

```cpp
struct AudioMixRegion {
    QString  sourcePath;       // 音频文件路径（录音为临时 WAV）
    QString  displayName;      // 列表显示名
    int64_t  videoStartUs;     // 贴入视频的时间（秒精度取整，微秒）
    int64_t  audioOffsetUs;    // 从音频文件哪里开始（微秒，默认 0）
    int64_t  audioDurationUs;  // 使用多少时长（微秒，探测或用户输入，>0）
    float    srcVol;           // 该区间源视频音量（0.0–1.0），默认 1.0
    float    mixVol;           // 新增音频音量，srcVol + mixVol = 1.0
    bool     isRecorded;       // true = 录音临时文件，析构时删除
};

struct AudioMixTask {
    QString               originalVideoPath;  // 始终为原始源视频
    QList<AudioMixRegion> regions;
    QString               outputPath;
};
```

**持续混合策略**：`AudioMixPanel::sourceFile_` 始终存储原始源视频；每次导出都从原始源重新混合所有累积区间，避免多代 AAC 编解码质量损失。

---

## AudioMixWorker 两步算法

### 步骤 1：`execMixAudio()` — 混音 → 临时 AAC

使用 FFmpeg avfilter 图，N+1 路输入：
- `[in0]` = 源视频音频
- `[in1..inN]` = 各区间附加音频文件

**滤镜字符串结构**（N 个区间）：

```
[in0]volume=srcVol1:enable='between(t,s1,e1)',
     volume=srcVol2:enable='between(t,s2,e2)',...[src_adj];

[in1]atrim=start=off1:end=off1+dur1,asetpts=PTS-STARTPTS,
     adelay=videoStartMs1|videoStartMs1,apad,volume=mixVol1[add1];

[in2]atrim=...,adelay=...,apad,volume=...[add2];

[src_adj][add1][add2]...amix=inputs=N+1:normalize=0:duration=first[out]
```

| 滤镜 | 作用 |
|------|------|
| `volume=...:enable='between(t,...)'` | 仅在区间时间窗内降低源音量到 srcVol，窗外保持原音量 |
| `atrim=start:end` | 截取附加音频的有效片段（考虑 audioOffsetUs 和 audioDurationUs） |
| `asetpts=PTS-STARTPTS` | trim 后重置时间戳从 0 开始 |
| `adelay=Nms\|Nms` | 将附加音频延迟到视频贴入点（立体声两声道同步） |
| `apad` | 附加音频结束后补充静音，防止 amix 提前 EOF |
| `amix=normalize=0:duration=first` | 直接叠加（不归一化），输出时长与源音频等长 |

**进度报告**：0–50% 对应音频混合阶段。

### 步骤 2：`execMuxFinal()` — 封装 → 输出 MP4

```
源视频  →  读取所有视频包（av_read_frame）
           │  av_packet_rescale_ts
           ▼
        av_interleaved_write_frame ─→ 输出 MP4
临时AAC →  读取所有音频包（av_read_frame）
           │  av_packet_rescale_ts
           ▼
        av_interleaved_write_frame ─→ 输出 MP4
```

- 视频流 `-c copy` 直通（零质量损失）
- 音频流直接拷贝临时 AAC（已编码，无需再编）
- `movflags=faststart` 便于网络播放

**进度报告**：60% 进入封装，80% 视频写完，100% 完成。

---

## UI 布局（AudioMixPanel）

```
QVBoxLayout
├─ QGroupBox "源视频"
│   └─ sourcePathEdit (只读) + openSourceBtn
├─ QGroupBox "音频区间列表"
│   ├─ regionsTable (5列: 音频文件|贴入时间|时长|源音量|混音量)
│   └─ removeRegionBtn
├─ QGroupBox "添加音频"
│   ├─ switchLocalBtn / switchRecordBtn (互斥切换)
│   └─ QStackedWidget
│       ├─ 页0 "本地音频"：audioFileEdit + 贴入时间SpinBox
│       │                   + 使用时长SpinBox + srcVolSlider/mixVolSlider
│       │                   + addRegionBtn
│       └─ 页1 "实时录入"：recStatusLabel + 起始时间SpinBox
│                           + recStartStopBtn
├─ previewBtn "试播放选中区间"
└─ QGroupBox "导出"
    └─ outputEdit + browseOutputBtn + exportBtn + progressBar
```

**Slider 联动约束**：`onSrcVolChanged` 和 `onMixVolChanged` 互相 `blockSignals` 更新对方，保证 `srcVol + mixVol = 100%`。

---

## Timeline 可视化扩展

在已有的绿色剪切区间行（底部导轨）下方新增**紫色音频区间行**：

```
[ 绿色剪切区间行 ]   ← 已有，segments_ 驱动
[ 紫色音频区间行 ]   ← 新增，audioRegions_ 驱动（kAudioBarHeight=20px）
```

接口：
```cpp
// timeline.h
void setAudioRegions(const QList<QPair<int64_t, int64_t>>& regions);

// timeline.cpp
void Timeline::setAudioRegions(...) { audioRegions_ = regions; update(); }
// drawBottomBar() 末尾：遍历 audioRegions_ 绘制紫色色块 + 起始时间标签
```

颜色：背景 `#1c1928`，色块 `QColor(120, 75, 200, 160)`，边框 `QColor(160, 110, 240)`。

---

## 实时录音流程

```
用户点击"开始录入"
  ├─ recordVideoStartUs_ = recVideoStartSpin * 1s
  ├─ player_->seek(startSec)    // 视频跳到录入起始位置
  ├─ player_->setVolume(0.0f)   // 静音，防止麦克风回声
  ├─ player_->play()            // 视频继续播放（无声）
  └─ QAudioInput::start(&recordBuffer_)  // 开始采集 PCM（S16 44100Hz 立体声）

用户点击"停止录入"
  ├─ audioInput_->stop()
  ├─ player_->pause(); player_->setVolume(prevVolume_)
  ├─ writeWav(tempPath, pcmData, 44100, 2, 16)  // 手写 44 字节 RIFF header
  └─ addRegionToList(AudioMixRegion{
         sourcePath=tempPath, isRecorded=true,
         videoStartUs=recordVideoStartUs_,
         audioDurationUs=pcmBytes/(44100×2×2)×1e6
     })
```

WAV 文件在 `AudioMixPanel` 析构时自动删除（`isRecorded=true` 的临时文件）。

---

## 试播放流程

```
选中区间行 → 点击"试播放"
  ├─ player_->seek(region.videoStartUs / 1e6)   // 视频跳到起点
  ├─ player_->setVolume(region.srcVol)           // 源音量按比例
  ├─ player_->play()
  ├─ previewPlayer_ = new QMediaPlayer
  │    .setMedia(region.sourcePath)
  │    .setPosition(region.audioOffsetUs / 1000)  // ms
  │    .setVolume(region.mixVol * 100)
  │    .play()
  └─ QTimer::singleShot(audioDurationUs/1000, stopPreview)  // 到区间结束自动停止
```

注：两路播放器并行，近似同步（误差 <100ms），满足预览精度需求。

---

## 可重用的现有代码

| 已有代码 | 复用点 |
|----------|--------|
| `AudioMixer::exec()` (`audiomixer.cpp`) | amix 滤镜图构建模式（abuffersrc/abuffersink/avfilter_graph_parse_ptr） |
| `MergeWorker::execAudioConcat()` (`mergeworker.cpp`) | SWR+AVAudioFifo+AAC 编码器初始化模式 |
| MergeDock 标题栏 (`mainwindow.cpp`) | 自定义 26px 标题栏（浮动/关闭按钮）完全复用 |
| `Timeline::drawBottomBar()` (`timeline.cpp`) | 在末尾追加音频区间绘制，不破坏原有逻辑 |

---

## 验证清单

1. **本地音频**：打开视频 → 工具→音频混合→本地音频 → 选 MP3 → 贴入时间=5s，混音量=30% → 加入列表 → Timeline 底部出现紫色区间块
2. **试播放**：选中区间→试播放 → 视频从 5s 开始，能听到混合音效（源 70%+新增 30%）
3. **导出**：填写输出路径→导出混合视频 → 进度条走完 → `ffprobe output.mp4` 确认 5s 处有混音区间
4. **多段叠加**：继续添加第二段音频 → 再次导出 → 验证两段都被混入（从原始源重混）
5. **录音**：切换到"实时录入"→ 设置起始时间 → 开始录入（视频无声播放）→ 停止 → 区间列表出现"录音 HH:MM:SS" → 导出验证
6. **srcVol 联动**：拖动源音量 Slider，混音量自动互补（总=100%）

---

## 当前状态

- [x] AudioMixWorker（两步 FFmpeg 混音引擎）
- [x] AudioMixPanel UI（本地音频 / 实时录入两页）
- [x] Timeline 紫色音频区间可视化
- [x] MainWindow 菜单 + Dock + openFile 联动
- [x] 编译通过（Debug）
- [ ] 用户测试验收
