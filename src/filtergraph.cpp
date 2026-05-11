#include "filtergraph.h"
#include <QDebug>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

FilterGraph::~FilterGraph() { close(); }

// 构建 buffersrc → 用户滤镜链 → buffersink 的滤镜图。
// filterDesc 为空时只记录参数不创建图，process 将走直通（ref）路径。
bool FilterGraph::init(int width, int height, AVPixelFormat pixFmt,
                        AVRational timeBase, const QString& filterDesc) {
    width_     = width;
    height_    = height;
    pixFmt_    = pixFmt;
    timeBase_  = timeBase;
    desc_      = filterDesc;

    if (filterDesc.isEmpty()) return true;

    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;

    const AVFilter* bufSrc = avfilter_get_by_name("buffer");
    char args[256];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             width, height, (int)pixFmt,
             timeBase.num, timeBase.den);
    if (avfilter_graph_create_filter(&src_, bufSrc, "in", args, nullptr, graph_) < 0) {
        close(); return false;
    }

    const AVFilter* bufSink = avfilter_get_by_name("buffersink");
    if (avfilter_graph_create_filter(&sink_, bufSink, "out", nullptr, nullptr, graph_) < 0) {
        close(); return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = src_;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    AVFilterInOut* inputs  = avfilter_inout_alloc();
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = sink_;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    int ret = avfilter_graph_parse_ptr(graph_, filterDesc.toUtf8().constData(),
                                       &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0) {
        qWarning() << "FilterGraph: avfilter_graph_parse_ptr failed for" << filterDesc;
        close(); return false;
    }

    if (avfilter_graph_config(graph_, nullptr) < 0) {
        qWarning() << "FilterGraph: avfilter_graph_config failed";
        close(); return false;
    }

    return true;
}

// 将帧送入滤镜链，结果写入 out。out 由调用方预先 av_frame_alloc 分配。
// 直通模式（desc 为空）时直接用 av_frame_ref 复制。
int FilterGraph::process(AVFrame* in, AVFrame* out) {
    if (desc_.isEmpty()) {
        AVFrame* cloned = av_frame_clone(in);
        if (!cloned) return -1;
        av_frame_move_ref(out, cloned);
        av_frame_free(&cloned);
        return 0;
    }

    if (av_buffersrc_add_frame_flags(src_, in, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
        return -1;

    return av_buffersink_get_frame(sink_, out);
}

// 在线重建滤镜图：先销毁旧图，再以新描述重新 init。
bool FilterGraph::rebuild(const QString& newFilterDesc) {
    close();
    return init(width_, height_, pixFmt_, timeBase_, newFilterDesc);
}

// 释放所有 libavfilter 资源
void FilterGraph::close() {
    if (graph_) { avfilter_graph_free(&graph_); src_ = nullptr; sink_ = nullptr; }
    desc_.clear();
}
