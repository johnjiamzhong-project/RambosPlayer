# Phase 8 — 视频滤镜编辑器 设计规格

**日期:** 2026-05-11
**状态:** 已确认

## 架构决策

### 滤镜插入点：VideoDecodeThread 内（方案 A）

解码帧在推入 videoFrameQ 之前通过 FilterGraph 处理。VideoRenderer 无改动，滤镜不阻塞 GUI 线程。

### 跨线程参数传递：atomic + dirtyFlag

沿用 seek/flush 的同款模式——FilterPanel 写 atomic 变量 + 设 dirtyFlag，VideoDecodeThread::run() 循环顶检测并重建 FilterGraph。

## 组件

### FilterGraph (`src/filtergraph.h/.cpp`)

封装 libavfilter，对外三个接口：

```
init(width, height, pixFmt, timeBase, filterDesc) → bool
process(AVFrame* in, AVFrame** out) → int          // 0=成功，out 调用方释放
rebuild(newFilterDesc) → bool
close()
```

- filterDesc 为空时是直通模式（passthrough），process 直接 av_frame_clone 返回入帧
- rebuild 内部分为 close() + init()，重建整个滤镜链
- 依赖：libavfilter (buffersrc → 用户滤镜链 → buffersink)

### VideoDecodeThread 改动

新增 atomic 参数 + FilterGraph 成员：

```cpp
std::atomic<bool>   filterEnabled_{false};
std::atomic<bool>   filterDirty_{false};
std::atomic<float>  brightness_{0.0f};
std::atomic<float>  contrast_{1.0f};
std::atomic<float>  saturation_{1.0f};
QMutex              watermarkMutex_;
QString             watermarkPath_;    // 空串=禁用
FilterGraph         filterGraph_;
```

run() 循环流程：
1. 检查 flush → 执行
2. 检查 filterDirty_ → rebuild FilterGraph
3. 取包 → send_packet → receive_frame
4. 若 filterEnabled_：filterGraph_.process(srcFrame, &filteredFrame)，用 filteredFrame 替代
5. clone → push 入 videoFrameQ

对外接口（FilterPanel 调用，线程安全）：
```cpp
void setBrightness(float v);
void setContrast(float v);
void setSaturation(float v);
void setWatermark(const QString& path);
void setFilterEnabled(bool on);
```

### FilterPanel (`src/filterpanel.h/.cpp/.ui`)

QDockWidget，所有控件在 .ui 中绘制：

```
控                 范围       默认值
─────────────────────────────────
☑ 启用滤镜         bool        false
亮度 QSlider      -1.0..1.0    0.0
对比度 QSlider     0.5..2.0    1.0
饱和度 QSlider     0.0..3.0    1.0
水印 QLineEdit    路径字符串   空
```

- 所有滑块变化 → 调 VideoDecodeThread 对应 setter → 写 atomic + 设 filterDirty_
- 水印 Browse 按钮 → QFileDialog::getOpenFileName（图片）
- 菜单栏加"视图 → 滤镜面板" toggle action，控制 FilterPanel 显示/隐藏

### MainWindow 改动

- 创建 FilterPanel，传入 VideoDecodeThread 指针
- 菜单栏加 `actionFilterPanel`（checkable）
- `addDockWidget(Qt::RightDockWidgetArea, filterPanel_)`

## 滤镜字符串生成

FilterPanel 的参数变化时，由 VideoDecodeThread::run() 拼出 FFmpeg 滤镜描述：

```
eq=brightness=<b>:contrast=<c>:saturation=<s>[,movie=<path>[wm];[in][wm]overlay=10:10]
```

## 测试

### Task 14 单元测试 (tst_filtergraph.cpp)

1. init 空 desc → 直通模式
2. init 亮度滤镜 → process 一帧 → 验证输出非空且宽高不变
3. init 无效 desc → 返回 false

### 端对端验证

- 打开视频，启用滤镜面板，调节亮度/对比度/饱和度滑块 → 画面实时变化
- 添加水印图片 → overlay 显示在左上角
- 禁用滤镜 → 画面恢复原始效果
- 滤镜面板关闭/显示 → 不影响播放

## 依赖

- FFmpeg libavfilter（vcpkg 已包含）
- 无需新增第三方库
