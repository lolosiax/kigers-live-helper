#ifndef KLIVE_FRAME_SOURCE_H
#define KLIVE_FRAME_SOURCE_H

#include "internal.h"

#include <d3d11.h>

#include <memory>

/**
 * 采集后端抽象。
 *
 * grab() 里已经把帧拷进自己的中转纹理（DXGI 帧池 / 桌面复制的纹理生命期都很短），
 * 上层只拿 ID3D11Texture2D 去画，不关心后端差异。
 */
class FrameSource {
public:
    virtual ~FrameSource() = default;

    /** 有新帧时把纹理写入 out 并返回 true；本轮没有新帧返回 false。 */
    virtual bool grab(ID3D11DeviceContext* context, ComPtr<ID3D11Texture2D>& out) = 0;

    /** 最近一帧的着色器视图，grab 成功后有效。 */
    virtual ID3D11ShaderResourceView* view() const = 0;

    virtual const char* name() const = 0;

    /** KLIVE_OK 表示正常，其它值表示采集已失败（之后的 grab 会一直返回 false）。 */
    int32_t error() const { return error_; }

protected:
    int32_t error_ = KLIVE_OK;
};

/** 中转纹理：把短命的帧纹理在 GPU 内拷一份，渲染时随便用。 */
class FrameCopy {
public:
    bool ensure(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source_desc);

    ID3D11Texture2D* copy_from(ID3D11DeviceContext* context, ID3D11Texture2D* source);

    ID3D11ShaderResourceView* view() const { return view_.Get(); }

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

/** 把 DXGI / Win32 错误码转成中文提示并记录。 */
void klive_set_dxgi_error(const char* stage, HRESULT hr);

std::unique_ptr<FrameSource> klive_create_duplication_source(const KliveOutput& output,
                                                             ID3D11Device* device);

/* hdr = true 时申请 FP16 帧池，WGC 会给出 scRGB 线性帧（1.0 = 80 nits）。 */
std::unique_ptr<FrameSource> klive_create_wgc_monitor_source(HMONITOR monitor,
                                                             ID3D11Device* device, bool hdr);

std::unique_ptr<FrameSource> klive_create_wgc_window_source(HWND window, ID3D11Device* device,
                                                            bool hdr);

#endif /* KLIVE_FRAME_SOURCE_H */
