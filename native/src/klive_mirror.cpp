#include "internal.h"
#include "frame_source.h"
#include "klive_osd.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>

namespace {

constexpr wchar_t kWindowClass[] = L"KliveMirrorWindow";
constexpr int kHotkeyId = 0xC1;
/* 动态档位重新解析采集源的间隔：太短会把设备重建拖成抖动，太长跟随会迟钝。 */
constexpr auto kSourceProbeInterval = std::chrono::milliseconds(250);
/* 切档提示在镜像画面上停留多久；太长会挡住画面，太短来不及扫一眼。 */
constexpr auto kOsdDuration = std::chrono::milliseconds(1800);

/* OSD 文案：镜像窗口在另一块屏上时，面板与系统通知都可能看不到。 */
const wchar_t* mode_label(int32_t mode) {
    switch (mode) {
        case KLIVE_MODE_POINTER_MONITOR:
            return L"\u8ddf\u968f\u9f20\u6807\u5c4f\u5e55";
        case KLIVE_MODE_POINTER_WINDOW:
            return L"\u9f20\u6807\u4e0b\u7a97\u53e3";
        case KLIVE_MODE_FOCUS_WINDOW:
            return L"\u524d\u53f0\u7a97\u53e3";
        case KLIVE_MODE_ZOOM_2X:
            return L"2\u00d7 \u7f29\u653e";
        case KLIVE_MODE_ZOOM_3X:
            return L"3\u00d7 \u7f29\u653e";
        case KLIVE_MODE_ZOOM_4X:
            return L"4\u00d7 \u7f29\u653e";
        default:
            return L"\u5168\u5c4f\u955c\u50cf";
    }
}

const char* kShaderSource = R"(
cbuffer Params : register(b0) {
    float2 gScale;    // 四边形缩放，负责等比填充 / 拉伸铺满
    float2 gUvScale;  // 采样范围，缩放档位时 = 1/倍数
    float2 gUvOffset; // 采样起点，按鼠标位置居中后夹取到边缘内
    float2 gHdr;      // x = 1 表示输入是 scRGB 线性帧；y = SDR 白电平（scRGB 单位）
};

Texture2D gSource : register(t0);
Texture1D gToneCurve : register(t1);
SamplerState gSampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    // id 0..3 → 三角带 (0,0) (1,0) (0,1) (1,1)
    float2 uv = float2(id & 1, (id >> 1) & 1);
    VSOut o;
    o.pos = float4((uv * 2.0 - 1.0) * gScale, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/*
 * HDR → SDR：输入是 WGC 的 scRGB 线性帧（1.0 = 80 nits）。
 * 先按系统 SDR 白电平归一化，再用面板给出的色调曲线（0..2 线性输入 → 0..1 线性输出）
 * 查表，最后 sRGB 编码。曲线纹理由 CPU 端单调三次插值生成，默认是 0.9 起的 C1 shoulder。
 */
float3 HdrToSdr(float3 c) {
    c = max(c, 0.0) / max(gHdr.y, 1e-3);
    float3 t = saturate(c * 0.5);
    float3 mapped = float3(
        gToneCurve.SampleLevel(gSampler, t.x, 0).r,
        gToneCurve.SampleLevel(gSampler, t.y, 0).r,
        gToneCurve.SampleLevel(gSampler, t.z, 0).r);
    return LinearToSrgb(mapped);
}

float4 PSMain(VSOut i) : SV_TARGET {
    float4 color = gSource.Sample(gSampler, i.uv * gUvScale + gUvOffset);
    if (gHdr.x > 0.5) return float4(HdrToSdr(color.rgb), 1.0);
    return color;
}
)";

struct ScaleParams {
    float scale[2];
    float uv_scale[2];
    float uv_offset[2];
    float hdr[2];
};

struct Pipeline {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Texture1D> curve_texture;
    ComPtr<ID3D11ShaderResourceView> curve_view;

    void reset() {
        vertex.Reset();
        pixel.Reset();
        sampler.Reset();
        params.Reset();
        rasterizer.Reset();
        curve_texture.Reset();
        curve_view.Reset();
    }

    bool create(ID3D11Device* device) {
        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        ComPtr<ID3DBlob> errors;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

        HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "klive.hlsl", nullptr, nullptr,
                                "VSMain", "vs_5_0", flags, 0, &vs_blob, &errors);
        if (FAILED(hr)) {
            klive_set_message("顶点着色器编译失败：%s",
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
            return false;
        }
        errors.Reset();

        hr = D3DCompile(kShaderSource, strlen(kShaderSource), "klive.hlsl", nullptr, nullptr,
                        "PSMain", "ps_5_0", flags, 0, &ps_blob, &errors);
        if (FAILED(hr)) {
            klive_set_message("像素着色器编译失败：%s",
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                              nullptr, &vertex)) ||
            FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                             nullptr, &pixel))) {
            klive_set_message("创建着色器失败");
            return false;
        }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler))) {
            klive_set_message("创建采样器失败");
            return false;
        }

        D3D11_BUFFER_DESC buffer_desc{};
        buffer_desc.ByteWidth = sizeof(ScaleParams);
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &params))) {
            klive_set_message("创建常量缓冲失败");
            return false;
        }

        // 全屏四边形没有固定绕序，关掉背面剔除
        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &rasterizer))) {
            klive_set_message("创建光栅化状态失败");
            return false;
        }

        float curve[KLIVE_TONE_CURVE_SIZE];
        klive_default_tone_curve(curve);
        D3D11_TEXTURE1D_DESC curve_desc{};
        curve_desc.Width = KLIVE_TONE_CURVE_SIZE;
        curve_desc.MipLevels = 1;
        curve_desc.ArraySize = 1;
        curve_desc.Format = DXGI_FORMAT_R32_FLOAT;
        curve_desc.Usage = D3D11_USAGE_DEFAULT;
        curve_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA curve_data{};
        curve_data.pSysMem = curve;
        if (FAILED(device->CreateTexture1D(&curve_desc, &curve_data, &curve_texture)) ||
            FAILED(device->CreateShaderResourceView(curve_texture.Get(), nullptr, &curve_view))) {
            klive_set_message("创建色调曲线纹理失败");
            return false;
        }
        return true;
    }

    void update_curve(ID3D11DeviceContext* context, const float* curve) {
        if (!curve_texture) return;
        context->UpdateSubresource(curve_texture.Get(), 0, nullptr, curve, 0, 0);
    }
};

// 窗口消息与渲染循环同线程，这里的状态只被这一个线程读写。
struct WindowState {
    uint32_t width = 0;
    uint32_t height = 0;
    bool resized = false;
};

WindowState g_window;
bool g_class_registered = false;

LRESULT CALLBACK mirror_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_window.width = LOWORD(lp);
                g_window.height = HIWORD(lp);
                g_window.resized = true;
            }
            return 0;
        case WM_KEYDOWN:
            // 窗口拿到焦点时 Esc 也能退出
            if (wp == VK_ESCAPE) PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_window_class(HINSTANCE instance) {
    if (g_class_registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mirror_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;

    if (RegisterClassExW(&wc) == 0) {
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    g_class_registered = true;
    return true;
}

const char* dxgi_error_text(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_DEVICE_REMOVED:
            return "显卡设备被移除";
        case DXGI_ERROR_SESSION_DISCONNECTED:
            return "当前会话已断开（远程桌面或锁屏）";
        default:
            return nullptr;
    }
}

void set_dxgi_error(const char* stage, HRESULT hr) {
    const char* text = dxgi_error_text(hr);
    if (text != nullptr) {
        klive_set_message("%s 失败：%s (0x%08lX)", stage, text, static_cast<unsigned long>(hr));
    } else {
        klive_set_message("%s 失败 (0x%08lX)", stage, static_cast<unsigned long>(hr));
    }
}

bool create_device(IDXGIAdapter* adapter, ComPtr<ID3D11Device>& device,
                   ComPtr<ID3D11DeviceContext>& context, const char* stage) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        set_dxgi_error(stage, hr);
        return false;
    }
    return true;
}

bool is_desktop_window(HWND window) {
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
           wcscmp(name, L"Shell_TrayWnd") == 0;
}

/* 鼠标 / 前台窗口 → 顶层窗口；桌面、本进程窗口、不可采集窗口返回 false。 */
bool resolve_window_source(HWND window, int64_t& out) {
    if (window == nullptr) return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) root = window;
    if (is_desktop_window(root)) return false;
    if (!klive_is_capturable_window(root)) return false;
    out = static_cast<int64_t>(reinterpret_cast<intptr_t>(root));
    return true;
}

int32_t find_output_by_monitor(const std::vector<KliveOutput>& outputs, HMONITOR monitor) {
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].desc.Monitor == monitor) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t zoom_factor_for_mode(int32_t mode) {
    switch (mode) {
        case KLIVE_MODE_ZOOM_2X:
            return 2;
        case KLIVE_MODE_ZOOM_3X:
            return 3;
        case KLIVE_MODE_ZOOM_4X:
            return 4;
        default:
            return 1;
    }
}

/**
 * 一台镜像会话的运行时状态。
 *
 * 设备、交换链、管线与采集源都挂在目标窗口上；动态档位换源时只重建必要部分：
 * 同显卡只换 FrameSource，跨显卡才重建 device / swapchain（窗口本身不动）。
 */
struct MirrorContext {
    const std::vector<KliveOutput>* outputs = nullptr;
    HWND hwnd = nullptr;
    int32_t target_index = -1;
    int32_t source_kind = KLIVE_SOURCE_MONITOR;
    int32_t source_index = -1;
    int64_t window_handle = 0;
    int32_t scale_mode = 0;
    bool vsync = true;
    int32_t capture_backend = KLIVE_CAPTURE_WGC;

    int32_t device_output = -1;
    /*
     * 整个会话只用一个 DXGI 工厂：窗口关联（MakeWindowAssociation）是进程级的，
     * 换卡重建时若换一个新工厂，CreateSwapChainForHwnd 会报 E_ACCESSDENIED。
     */
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
    Pipeline pipeline;
    OsdOverlay osd;
    std::unique_ptr<FrameSource> source;
    int32_t resolved_kind = -1;
    int32_t resolved_index = -1;
    int64_t resolved_window = 0;
    /* 当前采集源所属显示器的 SDR 白电平（nits），HDR 帧归一化用 */
    float source_sdr_white_nits = 80.0f;
    /* 已上传到曲线纹理的 LUT 版本；设备重建后置回哨兵值强制重传 */
    uint32_t curve_version = 0xFFFFFFFFu;
    bool clear_next = false;
};

bool resize_render_target(MirrorContext& ctx) {
    ctx.render_target.Reset();
    if (!ctx.swap_chain || g_window.width == 0 || g_window.height == 0) return false;
    if (FAILED(ctx.swap_chain->ResizeBuffers(0, g_window.width, g_window.height, DXGI_FORMAT_UNKNOWN,
                                             0))) {
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(ctx.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    if (FAILED(ctx.device->CreateRenderTargetView(back_buffer.Get(), nullptr, &ctx.render_target))) {
        return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_window.width);
    viewport.Height = static_cast<float>(g_window.height);
    viewport.MaxDepth = 1.0f;
    ctx.context->RSSetViewports(1, &viewport);
    return true;
}

bool ensure_factory(MirrorContext& ctx) {
    if (ctx.factory) return true;
    ComPtr<IDXGIFactory1> factory1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))) ||
        FAILED(factory1.As(&ctx.factory)) || !ctx.factory) {
        klive_set_message("获取 DXGI 工厂失败");
        return false;
    }
    ctx.factory->MakeWindowAssociation(ctx.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool create_swap_chain(MirrorContext& ctx) {
    if (!ensure_factory(ctx)) return false;

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = g_window.width;
    swap_desc.Height = g_window.height;
    swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    const HRESULT hr = ctx.factory->CreateSwapChainForHwnd(ctx.device.Get(), ctx.hwnd, &swap_desc,
                                                            nullptr, nullptr, &ctx.swap_chain);
    if (FAILED(hr)) {
        set_dxgi_error("创建交换链", hr);
        return false;
    }
    return resize_render_target(ctx);
}

/* 设备建在 output_index 这台显示器所属显卡上；跨卡换源时才重建整套呈现资源。 */
bool ensure_device(MirrorContext& ctx, int32_t output_index, const char* stage) {
    if (ctx.device && ctx.device_output == output_index) return true;

    ctx.source.reset();
    ctx.resolved_kind = -1;
    ctx.resolved_index = -1;
    ctx.resolved_window = 0;
    /*
     * 一个 HWND 同一时刻只能挂一个 flip 模型交换链，而 D3D11 会延迟销毁对象：
     * 必须先释放旧视图 / 交换链，再 ClearState + Flush 强制销毁，最后才能
     * CreateSwapChainForHwnd 建新的；顺序反了会报 E_ACCESSDENIED（0x80070005）。
     */
    ctx.render_target.Reset();
    ctx.pipeline.reset();
    ctx.osd.reset();
    ctx.swap_chain.Reset();
    if (ctx.context) {
        ctx.context->ClearState();
        ctx.context->Flush();
    }
    ctx.context.Reset();
    ctx.device.Reset();
    ctx.device_output = -1;

    const KliveOutput& output = (*ctx.outputs)[static_cast<size_t>(output_index)];
    if (!create_device(output.adapter.Get(), ctx.device, ctx.context, stage)) return false;
    if (!create_swap_chain(ctx)) return false;
    if (!ctx.pipeline.create(ctx.device.Get())) {
        klive_set_message("初始化渲染管线失败");
        return false;
    }

    ctx.device_output = output_index;
    ctx.curve_version = 0xFFFFFFFFu;
    ctx.clear_next = true;
    return true;
}

/* 采集源所属显示器的 HDR 参数；窗口源按窗口当前所在显示器判断。 */
const KliveOutput* source_output(const MirrorContext& ctx, int32_t kind, int32_t index,
                                 int64_t handle) {
    HMONITOR monitor = nullptr;
    if (kind == KLIVE_SOURCE_WINDOW) {
        monitor = MonitorFromWindow(reinterpret_cast<HWND>(static_cast<intptr_t>(handle)),
                                    MONITOR_DEFAULTTONEAREST);
    } else if (index >= 0 && index < static_cast<int32_t>(ctx.outputs->size())) {
        monitor = (*ctx.outputs)[static_cast<size_t>(index)].desc.Monitor;
    }
    if (monitor == nullptr) return nullptr;
    const int32_t found = find_output_by_monitor(*ctx.outputs, monitor);
    return found >= 0 ? &(*ctx.outputs)[static_cast<size_t>(found)] : nullptr;
}

std::unique_ptr<FrameSource> create_source(MirrorContext& ctx, int32_t kind, int32_t index,
                                           int64_t handle) {
    const KliveOutput* output = source_output(ctx, kind, index, handle);
    const bool hdr = output != nullptr && output->hdr;

    if (kind == KLIVE_SOURCE_WINDOW) {
        return klive_create_wgc_window_source(reinterpret_cast<HWND>(static_cast<intptr_t>(handle)),
                                              ctx.device.Get(), hdr);
    }

    const KliveOutput& target = (*ctx.outputs)[static_cast<size_t>(index)];
    /*
     * HDR 显示器上桌面复制给的表面颜色编码不可靠（实测是未标注的 HDR 数据），
     * 直接按 sRGB 画会发灰发暗；统一改用 WGC 的 FP16 帧 + 色调映射。
     */
    if (ctx.capture_backend == KLIVE_CAPTURE_WGC || hdr) {
        return klive_create_wgc_monitor_source(target.desc.Monitor, ctx.device.Get(), hdr);
    }
    return klive_create_duplication_source(target, ctx.device.Get());
}

/* 返回值：KLIVE_OK 成功；KLIVE_E_DEVICE 致命；其它为本次换源失败（下一轮重试）。 */
int32_t switch_source(MirrorContext& ctx, int32_t kind, int32_t index, int64_t handle) {
    const bool same = ctx.source != nullptr && ctx.resolved_kind == kind &&
                      (kind == KLIVE_SOURCE_WINDOW ? ctx.resolved_window == handle
                                                   : ctx.resolved_index == index);
    if (same) return KLIVE_OK;

    const int32_t device_output = kind == KLIVE_SOURCE_WINDOW ? ctx.target_index : index;
    if (!ensure_device(ctx, device_output, "切换采集源")) return KLIVE_E_DEVICE;

    std::unique_ptr<FrameSource> next = create_source(ctx, kind, index, handle);
    if (!next) {
        if (kind == KLIVE_SOURCE_WINDOW) return KLIVE_E_WINDOW;
        return ctx.capture_backend == KLIVE_CAPTURE_WGC ? KLIVE_E_CAPTURE : KLIVE_E_DUPLICATION;
    }

    const KliveOutput* output = source_output(ctx, kind, index, handle);
    ctx.source_sdr_white_nits = output != nullptr ? output->sdr_white_nits : 80.0f;

    ctx.source = std::move(next);
    ctx.resolved_kind = kind;
    ctx.resolved_index = kind == KLIVE_SOURCE_MONITOR ? index : -1;
    ctx.resolved_window = kind == KLIVE_SOURCE_WINDOW ? handle : 0;
    // 换源后旧画面不再代表当前内容，先清成黑屏，等新源出第一帧
    ctx.clear_next = true;
    return KLIVE_OK;
}

/* 按档位解析当前应该采集谁；解析不出来（鼠标在目标屏 / 没有可用窗口）返回 false。 */
bool resolve_source(const MirrorContext& ctx, int32_t mode, int32_t& kind, int32_t& index,
                    int64_t& handle) {
    kind = KLIVE_SOURCE_MONITOR;
    index = -1;
    handle = 0;

    switch (mode) {
        case KLIVE_MODE_FULLSCREEN:
            kind = ctx.source_kind;
            index = ctx.source_index;
            handle = ctx.window_handle;
            return true;

        case KLIVE_MODE_POINTER_MONITOR:
        case KLIVE_MODE_ZOOM_2X:
        case KLIVE_MODE_ZOOM_3X:
        case KLIVE_MODE_ZOOM_4X: {
            POINT cursor{};
            GetCursorPos(&cursor);
            const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
            const int32_t found = find_output_by_monitor(*ctx.outputs, monitor);
            // 目标屏上显示的就是镜像本身，采它会形成无限套娃
            if (found < 0 || found == ctx.target_index) return false;
            index = found;
            return true;
        }

        case KLIVE_MODE_POINTER_WINDOW: {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (!resolve_window_source(WindowFromPoint(cursor), handle)) return false;
            kind = KLIVE_SOURCE_WINDOW;
            return true;
        }

        case KLIVE_MODE_FOCUS_WINDOW: {
            if (!resolve_window_source(GetForegroundWindow(), handle)) return false;
            kind = KLIVE_SOURCE_WINDOW;
            return true;
        }

        default:
            return false;
    }
}

/* 把镜像画面画到当前 render target；着色器参数由调用方先写入常量缓冲。 */
void draw_mirror_quad(MirrorContext& ctx, ID3D11ShaderResourceView* view) {
    ctx.context->OMSetRenderTargets(1, ctx.render_target.GetAddressOf(), nullptr);
    ctx.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx.context->IASetInputLayout(nullptr);
    ctx.context->RSSetState(ctx.pipeline.rasterizer.Get());
    ctx.context->VSSetShader(ctx.pipeline.vertex.Get(), nullptr, 0);
    ctx.context->VSSetConstantBuffers(0, 1, ctx.pipeline.params.GetAddressOf());
    ctx.context->PSSetShader(ctx.pipeline.pixel.Get(), nullptr, 0);
    // 像素着色器要用 gUvScale / gUvOffset 做缩放，常量缓冲必须也绑到 PS 阶段
    ctx.context->PSSetConstantBuffers(0, 1, ctx.pipeline.params.GetAddressOf());
    ctx.context->PSSetShaderResources(0, 1, &view);
    ctx.context->PSSetShaderResources(1, 1, ctx.pipeline.curve_view.GetAddressOf());
    ctx.context->PSSetSamplers(0, 1, ctx.pipeline.sampler.GetAddressOf());
    ctx.context->Draw(4, 0);
}

int32_t run_mirror(KliveMirrorSession& session, const klive_mirror_config& config) {
    using Clock = std::chrono::steady_clock;
    std::vector<KliveOutput> outputs = klive_collect_outputs();
    if (outputs.empty()) {
        klive_set_message("没有检测到可用显示器");
        return KLIVE_E_NO_MONITOR;
    }

    const int32_t initial_mode = session.mode.load();
    const bool dynamic = initial_mode != KLIVE_MODE_FULLSCREEN;
    if (config.target_index < 0 || config.target_index >= static_cast<int32_t>(outputs.size())) {
        klive_set_message("目标显示器编号无效");
        return KLIVE_E_INVALID_ARG;
    }
    // 动态档位由鼠标 / 前台窗口决定源，固定 source_index / window_handle 不参与校验
    if (!dynamic) {
        if (config.source_kind == KLIVE_SOURCE_WINDOW) {
            if (config.window_handle == 0) {
                klive_set_message("没有选择采集窗口");
                return KLIVE_E_INVALID_ARG;
            }
        } else if (config.source_index < 0 ||
                   config.source_index >= static_cast<int32_t>(outputs.size())) {
            klive_set_message("源显示器编号无效");
            return KLIVE_E_INVALID_ARG;
        }
    }

    const KliveOutput& target = outputs[config.target_index];

    // 镜像窗口：无边框 + 置顶，直接铺满目标显示器；TOOLWINDOW 让它不进任务栏 / Alt+Tab
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!register_window_class(instance)) {
        klive_set_message("注册镜像窗口类失败");
        return KLIVE_E_WINDOW;
    }

    const RECT& rect = target.desc.DesktopCoordinates;
    const int window_width = rect.right - rect.left;
    const int window_height = rect.bottom - rect.top;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"klive", WS_POPUP,
                                rect.left, rect.top, window_width, window_height, nullptr, nullptr,
                                instance, nullptr);
    if (hwnd == nullptr) {
        klive_set_message("创建镜像窗口失败（错误码 %lu）", static_cast<unsigned long>(GetLastError()));
        return KLIVE_E_WINDOW;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, window_width, window_height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    g_window.width = static_cast<uint32_t>(window_width);
    g_window.height = static_cast<uint32_t>(window_height);
    g_window.resized = false;

    MirrorContext ctx;
    ctx.outputs = &outputs;
    ctx.hwnd = hwnd;
    ctx.target_index = config.target_index;
    ctx.source_kind = config.source_kind;
    ctx.source_index = config.source_index;
    ctx.window_handle = config.window_handle;
    ctx.scale_mode = config.scale_mode;
    ctx.vsync = config.vsync != 0;
    ctx.capture_backend = config.capture_backend;

    // 初始源：动态档位解析失败（例如鼠标就在目标屏上）时退回全屏档的固定源
    int32_t want_kind = KLIVE_SOURCE_MONITOR;
    int32_t want_index = -1;
    int64_t want_handle = 0;
    if (!resolve_source(ctx, initial_mode, want_kind, want_index, want_handle)) {
        want_kind = config.source_kind;
        want_index = config.source_index;
        want_handle = config.window_handle;
        // 动态档位解析失败时退回固定源，但绝不能退回目标屏自己
        if (want_kind == KLIVE_SOURCE_MONITOR && want_index == config.target_index) {
            want_index = -1;
            for (const KliveOutput& output : outputs) {
                if (output.index != config.target_index) {
                    want_index = output.index;
                    break;
                }
            }
        }
    }
    if (want_kind == KLIVE_SOURCE_MONITOR && want_index < 0) {
        klive_set_message("至少需要两台显示器");
        DestroyWindow(hwnd);
        return KLIVE_E_NO_MONITOR;
    }
    int32_t code = switch_source(ctx, want_kind, want_index, want_handle);
    if (code != KLIVE_OK) {
        DestroyWindow(hwnd);
        return code;
    }
    session.source_index = ctx.resolved_index;

    RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    int32_t result = KLIVE_OK;
    auto last_fps_sample = Clock::now();
    auto last_probe = Clock::now() - kSourceProbeInterval;
    int64_t frames_since_sample = 0;
    // 与初始档位一致，启动时不要弹一条「全屏镜像」
    int32_t last_mode = initial_mode;
    // 缩放中心：鼠标离开当前源显示器时保留最后一次有效位置，避免画面跳边
    float zoom_u = 0.5f;
    float zoom_v = 0.5f;
    bool quit = false;
    // 切档提示：静态画面不会出新帧，靠 need_present 主动补一帧显示 / 擦除 OSD
    bool osd_visible = false;
    auto osd_deadline = Clock::now();
    bool need_present = false;
    bool scene_drawn = false;

    while (!quit && !session.stop.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;

        if (g_window.resized) {
            g_window.resized = false;
            resize_render_target(ctx);
        }

        const int32_t mode = session.mode.load();
        const auto now = Clock::now();
        const bool mode_changed = mode != last_mode;
        if (mode_changed || now - last_probe >= kSourceProbeInterval) {
            last_mode = mode;
            last_probe = now;
            if (resolve_source(ctx, mode, want_kind, want_index, want_handle)) {
                code = switch_source(ctx, want_kind, want_index, want_handle);
                if (code == KLIVE_E_DEVICE) {
                    result = code;
                    break;
                }
                if (code == KLIVE_OK) session.source_index = ctx.resolved_index;
            }
        }
        if (mode_changed) {
            ctx.osd.set_text(mode_label(mode), g_window.width);
            osd_visible = true;
            osd_deadline = now + kOsdDuration;
            need_present = true;
        }
        if (osd_visible && now >= osd_deadline) {
            osd_visible = false;
            ctx.osd.hide();
            // 暂停中的画面没有新帧，靠这一帧把 OSD 擦掉
            need_present = true;
        }

        if (!ctx.source) {
            // 没有采集源时也要把刚切档的 OSD 画出来
            if (need_present && ctx.render_target) {
                const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                ctx.context->ClearRenderTargetView(ctx.render_target.Get(), clear);
                ctx.context->OMSetRenderTargets(1, ctx.render_target.GetAddressOf(), nullptr);
                if (osd_visible) {
                    ctx.osd.draw(ctx.context.Get(), ctx.device.Get(), g_window.width, g_window.height);
                }
                ctx.swap_chain->Present(ctx.vsync ? 1 : 0, 0);
                scene_drawn = false;
                need_present = false;
            }
            Sleep(10);
            continue;
        }

        // 面板调完曲线后重传 LUT；静态画面没有新帧，靠 need_present 补一帧让它立刻可见
        const uint32_t curve_version = session.curve_version.load();
        if (curve_version != ctx.curve_version) {
            float curve[KLIVE_TONE_CURVE_SIZE];
            {
                SrwExclusiveLock lock(session.curve_lock);
                std::copy(std::begin(session.tone_curve), std::end(session.tone_curve), curve);
            }
            ctx.pipeline.update_curve(ctx.context.Get(), curve);
            ctx.curve_version = curve_version;
            need_present = true;
        }

        ComPtr<ID3D11Texture2D> frame_texture;
        const bool has_frame = ctx.source->grab(ctx.context.Get(), frame_texture);
        if (ctx.source->error() != KLIVE_OK) {
            if (mode == KLIVE_MODE_FULLSCREEN) {
                result = ctx.source->error();
                break;
            }
            // 动态源失效（窗口关闭 / WGC 会话断开）：丢掉它，下一轮重新解析
            ctx.source.reset();
            ctx.resolved_kind = -1;
            ctx.resolved_index = -1;
            ctx.resolved_window = 0;
            ctx.clear_next = true;
            session.source_index = -1;
            continue;
        }

        if (!has_frame) {
            if (ctx.render_target && (ctx.clear_next || need_present)) {
                const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                ctx.context->ClearRenderTargetView(ctx.render_target.Get(), clear);
                ctx.context->OMSetRenderTargets(1, ctx.render_target.GetAddressOf(), nullptr);
                // 旧画面仍代表当前内容时才补画；换源后必须先黑屏等新帧
                if (!ctx.clear_next && scene_drawn) {
                    ID3D11ShaderResourceView* frame_view = ctx.source->view();
                    if (frame_view != nullptr) draw_mirror_quad(ctx, frame_view);
                }
                if (osd_visible) {
                    ctx.osd.draw(ctx.context.Get(), ctx.device.Get(), g_window.width, g_window.height);
                }
                ctx.swap_chain->Present(ctx.vsync ? 1 : 0, 0);
                if (ctx.clear_next) scene_drawn = false;
                ctx.clear_next = false;
                need_present = false;
            }
            continue;
        }

        D3D11_TEXTURE2D_DESC desc{};
        frame_texture->GetDesc(&desc);

        const UINT swap_width = g_window.width;
        const UINT swap_height = g_window.height;
        if (!ctx.render_target || swap_width == 0 || swap_height == 0) continue;

        float scale_x = 1.0f;
        float scale_y = 1.0f;
        if (ctx.scale_mode == 0 && desc.Width > 0 && desc.Height > 0) {
            const float fit = std::min(static_cast<float>(swap_width) / static_cast<float>(desc.Width),
                                       static_cast<float>(swap_height) / static_cast<float>(desc.Height));
            scale_x = static_cast<float>(desc.Width) * fit / static_cast<float>(swap_width);
            scale_y = static_cast<float>(desc.Height) * fit / static_cast<float>(swap_height);
        }

        float uv_scale_x = 1.0f;
        float uv_scale_y = 1.0f;
        float uv_offset_x = 0.0f;
        float uv_offset_y = 0.0f;
        const int32_t zoom = zoom_factor_for_mode(mode);
        if (zoom > 1 && ctx.resolved_kind == KLIVE_SOURCE_MONITOR && ctx.resolved_index >= 0) {
            const RECT& source_rect =
                outputs[static_cast<size_t>(ctx.resolved_index)].desc.DesktopCoordinates;
            POINT cursor{};
            GetCursorPos(&cursor);
            const float u = static_cast<float>(cursor.x - source_rect.left) /
                            static_cast<float>(source_rect.right - source_rect.left);
            const float v = static_cast<float>(cursor.y - source_rect.top) /
                            static_cast<float>(source_rect.bottom - source_rect.top);
            if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                zoom_u = u;
                zoom_v = v;
            }
            const float span = 1.0f / static_cast<float>(zoom);
            uv_scale_x = span;
            uv_scale_y = span;
            uv_offset_x = std::clamp(zoom_u - span * 0.5f, 0.0f, 1.0f - span);
            uv_offset_y = std::clamp(zoom_v - span * 0.5f, 0.0f, 1.0f - span);
        }

        // WGC 的 HDR 帧池是 R16G16B16A16_FLOAT（scRGB 线性），需要着色器里转 SDR
        const bool hdr_frame = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        ScaleParams params{{scale_x, scale_y},
                           {uv_scale_x, uv_scale_y},
                           {uv_offset_x, uv_offset_y},
                           {hdr_frame ? 1.0f : 0.0f,
                            std::max(ctx.source_sdr_white_nits / 80.0f, 1.0f)}};
        ctx.context->UpdateSubresource(ctx.pipeline.params.Get(), 0, nullptr, &params, 0, 0);

        ID3D11ShaderResourceView* frame_view = ctx.source->view();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx.context->ClearRenderTargetView(ctx.render_target.Get(), clear);
        draw_mirror_quad(ctx, frame_view);
        if (osd_visible) {
            ctx.osd.draw(ctx.context.Get(), ctx.device.Get(), g_window.width, g_window.height);
        }

        const HRESULT hr = ctx.swap_chain->Present(ctx.vsync ? 1 : 0, 0);
        if (FAILED(hr)) {
            set_dxgi_error("呈现画面", hr);
            result = KLIVE_E_DEVICE;
            break;
        }
        ctx.clear_next = false;
        scene_drawn = true;
        need_present = false;

        session.frames.fetch_add(1);
        session.frame_width = static_cast<int32_t>(desc.Width);
        session.frame_height = static_cast<int32_t>(desc.Height);
        ++frames_since_sample;

        const auto sample_now = Clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(sample_now - last_fps_sample);
        if (elapsed.count() >= 500) {
            session.fps = static_cast<double>(frames_since_sample) * 1000.0 /
                          static_cast<double>(elapsed.count());
            frames_since_sample = 0;
            last_fps_sample = sample_now;
        }
    }

    UnregisterHotKey(nullptr, kHotkeyId);
    session.fps = 0.0;
    session.source_index = -1;
    DestroyWindow(hwnd);
    return result;
}

KliveMirrorSession g_session;

}  // namespace

void klive_default_tone_curve(float* out) {
    constexpr float kKnee = 0.9f;
    for (int32_t i = 0; i < KLIVE_TONE_CURVE_SIZE; ++i) {
        const float x = 2.0f * static_cast<float>(i) / static_cast<float>(KLIVE_TONE_CURVE_SIZE - 1);
        const float over = std::max(x - kKnee, 0.0f);
        const float mapped =
            std::min(x, kKnee) + (1.0f - kKnee) * over / std::max(over + (1.0f - kKnee), 1e-3f);
        out[i] = std::clamp(mapped, 0.0f, 1.0f);
    }
}

KliveMirrorSession::KliveMirrorSession() {
    klive_default_tone_curve(tone_curve);
}

KliveMirrorSession& klive_session() {
    return g_session;
}

int32_t klive_mirror_start(const klive_mirror_config& config) {
    KliveMirrorSession& session = klive_session();
    SrwExclusiveLock lock(session.lifecycle);

    if (session.running.load()) {
        klive_set_message("镜像已经在运行");
        return KLIVE_E_ALREADY_RUNNING;
    }
    // 上一轮线程可能已自行结束，但 std::thread 对象还挂着，必须先回收
    if (session.worker.joinable()) {
        session.worker.join();
    }

    if (config.mode < 0 || config.mode >= KLIVE_MODE_COUNT) {
        klive_set_message("镜像档位无效");
        return KLIVE_E_INVALID_ARG;
    }

    // 先在调用线程做一次参数校验，错误可以同步返回给 UI
    std::vector<KliveOutput> outputs = klive_collect_outputs();
    if (outputs.empty()) {
        klive_set_message("没有检测到可用显示器");
        return KLIVE_E_NO_MONITOR;
    }
    if (config.target_index < 0 || config.target_index >= static_cast<int32_t>(outputs.size())) {
        klive_set_message("目标显示器编号无效");
        return KLIVE_E_INVALID_ARG;
    }
    if (config.mode == KLIVE_MODE_FULLSCREEN) {
        if (config.source_kind == KLIVE_SOURCE_WINDOW) {
            if (config.window_handle == 0) {
                klive_set_message("没有选择采集窗口");
                return KLIVE_E_INVALID_ARG;
            }
        } else {
            if (config.source_index < 0 ||
                config.source_index >= static_cast<int32_t>(outputs.size())) {
                klive_set_message("源显示器编号无效");
                return KLIVE_E_INVALID_ARG;
            }
            if (config.source_index == config.target_index) {
                klive_set_message("源显示器和目标显示器不能相同");
                return KLIVE_E_INVALID_ARG;
            }
        }
    }

    session.config = config;
    session.mode = config.mode;
    session.stop = false;
    session.last_error = KLIVE_OK;
    session.frames = 0;
    session.fps = 0.0;
    session.frame_width = 0;
    session.frame_height = 0;
    session.source_index = -1;
    session.running = true;

    session.worker = std::thread([&session] {
        const int32_t code = run_mirror(session, session.config);
        session.last_error = code;
        session.running = false;
    });
    return KLIVE_OK;
}

int32_t klive_mirror_stop() {
    KliveMirrorSession& session = klive_session();
    SrwExclusiveLock lock(session.lifecycle);

    // 线程可能已经因为错误自行退出，这里仍然要 join 掉
    const bool was_running = session.running.load();
    session.stop = true;
    if (session.worker.joinable()) {
        session.worker.join();
    }
    session.running = false;
    return was_running ? KLIVE_OK : KLIVE_E_NOT_RUNNING;
}
