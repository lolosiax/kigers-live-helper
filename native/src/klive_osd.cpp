#include "klive_osd.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

const char* kShaderSource = R"(
cbuffer OsdParams : register(b0) {
    float4 gRect; // xy = 左上角（NDC），zw = 宽高（NDC）
};

Texture2D gText : register(t0);
SamplerState gSampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, (id >> 1) & 1);
    VSOut o;
    // NDC 的 y 轴朝上，uv.y = 0 对应面板上边缘
    o.pos = float4(gRect.x + uv.x * gRect.z, gRect.y - uv.y * gRect.w, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    return gText.Sample(gSampler, i.uv);
}
)";

constexpr wchar_t kFontFace[] = L"Microsoft YaHei UI";
/* 面板底色不透明度：AR 眼镜上画面常被压暗，太淡会让白字失去对比 */
constexpr float kPanelAlpha = 0.62f;

/* 按窗口宽度算字号：1920 宽约 48px，最多再按面板宽度缩到能放下整句。 */
uint32_t initial_font_px(uint32_t window_width) {
    return std::max<uint32_t>(window_width / 40, 22);
}

HFONT make_font(uint32_t font_px) {
    return CreateFontW(-static_cast<int>(font_px), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFontFace);
}

/**
 * 把一句话画成面板位图（BGRA，预乘 alpha）。
 *
 * 先在 32 位 DIB 上画白字黑底，再把文字亮度当作覆盖率与半透明黑底合成：
 * rgb = 覆盖率（预乘后的白色），a = 覆盖率 + (1 - 覆盖率) * 面板不透明度。
 */
bool rasterize(const std::wstring& text, uint32_t window_width, std::vector<uint8_t>& pixels,
               uint32_t& out_width, uint32_t& out_height) {
    if (text.empty() || window_width == 0) return false;

    const uint32_t min_width = std::max<uint32_t>(window_width / 10, 120);
    const uint32_t max_width = std::max<uint32_t>(window_width / 7, min_width);

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) return false;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    uint32_t font_px = initial_font_px(window_width);
    HFONT font = make_font(font_px);
    if (font == nullptr) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_font = SelectObject(dc, font);

    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);

    uint32_t padding = std::max<uint32_t>(font_px * 2 / 5, 6);
    uint32_t available =
        max_width > 2 * padding ? max_width - 2 * padding : std::max<uint32_t>(max_width / 2, 16);
    if (size.cx > 0 && static_cast<uint32_t>(size.cx) > available) {
        font_px = std::max<uint32_t>(static_cast<uint32_t>(static_cast<uint64_t>(font_px) *
                                                          available / static_cast<uint32_t>(size.cx)),
                                     16);
        SelectObject(dc, old_font);
        DeleteObject(font);
        font = make_font(font_px);
        if (font == nullptr) {
            DeleteDC(dc);
            return false;
        }
        SelectObject(dc, font);
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        padding = std::max<uint32_t>(font_px * 2 / 5, 6);
    }

    const uint32_t panel_width =
        std::clamp<uint32_t>(static_cast<uint32_t>(std::max<LONG>(size.cx, 1)) + 2 * padding,
                             min_width, max_width);
    const uint32_t panel_height =
        static_cast<uint32_t>(std::max<LONG>(size.cy, 1)) + 2 * padding;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(panel_width);
    // 负高度 = 自上而下，与 D3D 纹理的行序一致
    bmi.bmiHeader.biHeight = -static_cast<LONG>(panel_height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

    const size_t pixel_bytes = static_cast<size_t>(panel_width) * panel_height * 4;
    memset(bits, 0, pixel_bytes);
    RECT rect{0, 0, static_cast<LONG>(panel_width), static_cast<LONG>(panel_height)};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    auto* px = static_cast<uint8_t*>(bits);
    const uint32_t radius = std::max<uint32_t>(panel_height / 6, 4);
    for (uint32_t y = 0; y < panel_height; ++y) {
        for (uint32_t x = 0; x < panel_width; ++x) {
            uint8_t* p = px + (static_cast<size_t>(y) * panel_width + x) * 4;
            const uint8_t luminance = std::max({p[0], p[1], p[2]});
            const float coverage = static_cast<float>(luminance) / 255.0f;
            float alpha = coverage + (1.0f - coverage) * kPanelAlpha;

            // 圆角遮罩：角落方块内超出半径的像素透明
            int dx = 0;
            int dy = 0;
            if (x < radius) {
                dx = static_cast<int>(radius - x);
            } else if (x + radius >= panel_width) {
                dx = static_cast<int>(x) - static_cast<int>(panel_width - radius) + 1;
            }
            if (y < radius) {
                dy = static_cast<int>(radius - y);
            } else if (y + radius >= panel_height) {
                dy = static_cast<int>(y) - static_cast<int>(panel_height - radius) + 1;
            }
            if (dx * dx + dy * dy > static_cast<int>(radius) * static_cast<int>(radius)) alpha = 0.0f;

            p[0] = p[1] = p[2] = luminance;
            p[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }
    GdiFlush();

    pixels.resize(pixel_bytes);
    memcpy(pixels.data(), bits, pixel_bytes);
    out_width = panel_width;
    out_height = panel_height;

    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

}  // namespace

void OsdOverlay::set_text(const wchar_t* text, uint32_t window_width) {
    text_ = text != nullptr ? text : L"";
    window_width_ = window_width;
    texture_.Reset();
    view_.Reset();
    failed_ = false;
}

void OsdOverlay::hide() {
    text_.clear();
    texture_.Reset();
    view_.Reset();
}

void OsdOverlay::reset() {
    texture_.Reset();
    view_.Reset();
    vertex_.Reset();
    pixel_.Reset();
    blend_.Reset();
    sampler_.Reset();
    rasterizer_.Reset();
    params_.Reset();
    device_.Reset();
    window_width_ = 0;
    failed_ = false;
}

bool OsdOverlay::create_resources(ID3D11Device* device) {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "klive_osd.hlsl", nullptr,
                            nullptr, "VSMain", "vs_5_0", flags, 0, &vs_blob, nullptr);
    if (FAILED(hr)) return false;
    hr = D3DCompile(kShaderSource, strlen(kShaderSource), "klive_osd.hlsl", nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, &ps_blob, nullptr);
    if (FAILED(hr)) return false;

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                          nullptr, &vertex_)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                         nullptr, &pixel_))) {
        return false;
    }

    // 预乘 alpha：颜色直接相加，目标只被 (1 - src.a) 遮挡
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend_desc, &blend_))) return false;

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler_))) return false;

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer_))) return false;

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(float) * 4;
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&buffer_desc, nullptr, &params_));
}

bool OsdOverlay::rebuild_bitmap(uint32_t window_width) {
    texture_.Reset();
    view_.Reset();

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!rasterize(text_, window_width, pixels, width, height)) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width * 4;
    if (FAILED(device_->CreateTexture2D(&desc, &data, &texture_))) return false;
    if (FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &view_))) {
        texture_.Reset();
        return false;
    }

    bitmap_width_ = width;
    bitmap_height_ = height;
    window_width_ = window_width;
    return true;
}

bool OsdOverlay::ensure(ID3D11Device* device, uint32_t window_width) {
    if (failed_ || device == nullptr) return false;

    if (device_.Get() != device) {
        reset();
        if (!create_resources(device)) {
            failed_ = true;
            return false;
        }
        device_ = device;
    }
    if (view_ == nullptr || window_width != window_width_) {
        if (!rebuild_bitmap(window_width)) {
            failed_ = true;
            return false;
        }
    }
    return true;
}

void OsdOverlay::draw(ID3D11DeviceContext* context, ID3D11Device* device, uint32_t window_width,
                      uint32_t window_height) {
    if (text_.empty() || context == nullptr || window_width == 0 || window_height == 0) return;
    if (!ensure(device, window_width)) return;

    // 左上角留一点边距，换算成 NDC 的矩形
    const float margin = static_cast<float>(std::max<uint32_t>(window_width / 90, 8));
    const float rect[4] = {
        -1.0f + 2.0f * margin / static_cast<float>(window_width),
        1.0f - 2.0f * margin / static_cast<float>(window_height),
        2.0f * static_cast<float>(bitmap_width_) / static_cast<float>(window_width),
        2.0f * static_cast<float>(bitmap_height_) / static_cast<float>(window_height),
    };
    context->UpdateSubresource(params_.Get(), 0, nullptr, rect, 0, 0);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetInputLayout(nullptr);
    context->RSSetState(rasterizer_.Get());
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, params_.GetAddressOf());
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, view_.GetAddressOf());
    context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    context->Draw(4, 0);
    // 后续帧还要画不透明的镜像四边形，收尾时必须关掉混合
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}
