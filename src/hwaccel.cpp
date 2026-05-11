#include "hwaccel.h"

// 创建指定类型的硬件加速设备（如 AV_HWDEVICE_TYPE_D3D11VA）。
// 成功时 deviceCtx_ 非空，失败时返回 false 并保持 deviceCtx_ 为 null。
bool HWAccel::create(AVHWDeviceType type) {
    destroy();
    int ret = av_hwdevice_ctx_create(&deviceCtx_, type, nullptr, nullptr, 0);
    return ret >= 0;
}

// 释放硬件设备上下文，可重复调用。
void HWAccel::destroy() {
    if (deviceCtx_) {
        av_buffer_unref(&deviceCtx_);
        deviceCtx_ = nullptr;
    }
}
