// HWAccel: 硬件加速设备 RAII 封装
// 封装 FFmpeg AVHWDeviceContext 的创建与销毁。
// create() 尝试创建指定类型的硬件设备（如 D3D11VA），失败时返回 false。
// 析构函数自动释放设备上下文，无需手动清理。
#pragma once

extern "C" {
#include <libavutil/hwcontext.h>
}

class HWAccel {
public:
    ~HWAccel() { destroy(); }

    bool create(AVHWDeviceType type);
    void destroy();

    AVBufferRef* deviceCtx() const { return deviceCtx_; }
    bool isCreated() const { return deviceCtx_ != nullptr; }

private:
    AVBufferRef* deviceCtx_ = nullptr;  // FFmpeg 硬件设备上下文句柄
};
