#pragma once
#include <QString>
#include <libavutil/rational.h>

struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
enum AVPixelFormat;

// FilterGraph 封装 libavfilter 滤镜链，在 VideoDecodeThread 解码线程中使用。
// 每帧解码后调用 process 过滤镜；滤镜参数变化时通过 rebuild 在线重建整条链。
class FilterGraph {
public:
    ~FilterGraph();

    bool init(int width, int height, AVPixelFormat pixFmt,
              AVRational timeBase, const QString& filterDesc);
    // 将 in 送入滤镜链，结果写入 out（out 必须已由调用方 av_frame_alloc 分配）。
    // 成功返回 0；直通模式（desc 为空）时直接 av_frame_ref 复制。
    int process(AVFrame* in, AVFrame* out);
    bool rebuild(const QString& newFilterDesc);
    void close();

    bool isEmpty() const { return desc_.isEmpty(); }

private:
    AVFilterGraph* graph_ = nullptr;   // libavfilter 滤镜图
    AVFilterContext* src_ = nullptr;   // buffersrc 入口
    AVFilterContext* sink_ = nullptr;  // buffersink 出口
    QString desc_;                     // 当前滤镜描述字串
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat pixFmt_;
    AVRational timeBase_;
};
