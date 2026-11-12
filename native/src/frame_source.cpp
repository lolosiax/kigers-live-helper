#include "frame_source.h"

#include <cstdio>

namespace {

const char* dxgi_error_text(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_UNSUPPORTED:
            return "显卡或驱动不支持该采集方式";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
            return "有其他程序正在复制该显示器（桌面复制同一时间只能有一个消费者，"
                   "可改用 WGC 后端或窗口采集）";
        case DXGI_ERROR_ACCESS_LOST:
            return "采集会话失效（切换显示器 / 分辨率变化 / 锁屏）";
        case DXGI_ERROR_ACCESS_DENIED:
            return "没有采集权限（安全桌面 / 受保护内容）";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "显卡设备被移除";
        case DXGI_ERROR_SESSION_DISCONNECTED:
            return "当前会话已断开（远程桌面或锁屏）";
        default:
            return nullptr;
    }
}

}  // namespace

void klive_set_dxgi_error(const char* stage, HRESULT hr) {
    const char* text = dxgi_error_text(hr);
    if (text != nullptr) {
        klive_set_message("%s 失败：%s (0x%08lX)", stage, text, static_cast<unsigned long>(hr));
    } else {
        klive_set_message("%s 失败 (0x%08lX)", stage, static_cast<unsigned long>(hr));
    }
}

bool FrameCopy::ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source_desc) {
    if (texture_ && width_ == source_desc.Width && height_ == source_desc.Height &&
        format_ == source_desc.Format) {
        return true;
    }
    texture_.Reset();
    view_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source_desc.Width;
    desc.Height = source_desc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = source_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture_)) ||
        FAILED(device->CreateShaderResourceView(texture_.Get(), nullptr, &view_))) {
        klive_set_message("创建帧中转纹理失败（%ux%u）", source_desc.Width, source_desc.Height);
        texture_.Reset();
        view_.Reset();
        return false;
    }
    width_ = source_desc.Width;
    height_ = source_desc.Height;
    format_ = source_desc.Format;
    return true;
}

ID3D11Texture2D* FrameCopy::copy_from(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (!texture_ || source == nullptr) return nullptr;
    context->CopyResource(texture_.Get(), source);
    return texture_.Get();
}
