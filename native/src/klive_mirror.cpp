#include "internal.h"
#include "frame_source.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>

namespace {

constexpr wchar_t kWindowClass[] = L"KliveMirrorWindow";
constexpr int kHotkeyId = 0xC1;

const char* kShaderSource = R"(
cbuffer Params : register(b0) {
    float2 gScale;
    float2 gPad;
};

Texture2D gSource : register(t0);
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

float4 PSMain(VSOut i) : SV_TARGET {
    return gSource.Sample(gSampler, i.uv);
}
)";

struct ScaleParams {
    float scale[2];
    float pad[2];
};

struct Pipeline {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11RasterizerState> rasterizer;

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
        return true;
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

int32_t run_mirror(KliveMirrorSession& session, const klive_mirror_config& config) {
    using Clock = std::chrono::steady_clock;
    std::vector<KliveOutput> outputs = klive_collect_outputs();
    if (outputs.empty()) {
        klive_set_message("没有检测到可用显示器");
        return KLIVE_E_NO_MONITOR;
    }

    const bool window_source = config.source_kind == KLIVE_SOURCE_WINDOW;
    if (config.target_index < 0 || config.target_index >= static_cast<int32_t>(outputs.size())) {
        klive_set_message("目标显示器编号无效");
        return KLIVE_E_INVALID_ARG;
    }
    if (window_source) {
        if (config.window_handle == 0) {
            klive_set_message("没有选择采集窗口");
            return KLIVE_E_INVALID_ARG;
        }
    } else if (config.source_index < 0 ||
               config.source_index >= static_cast<int32_t>(outputs.size())) {
        klive_set_message("源显示器编号无效");
        return KLIVE_E_INVALID_ARG;
    }

    const KliveOutput& target = outputs[config.target_index];
    // 窗口采集时，设备建在目标显示器所在显卡上（WGC 会在我们的设备上建帧纹理）
    const KliveOutput& source = window_source ? target : outputs[config.source_index];

    // 设备建在源显示器所在显卡上；即使目标显示器挂在另一块卡上，
    // 向它的窗口 Present 也由 DWM 完成跨卡合成（实测可用）。
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!create_device(source.adapter.Get(), device, context, "创建 D3D11 设备")) {
        return KLIVE_E_DEVICE;
    }

    // 镜像窗口：无边框 + 置顶，直接铺满目标显示器
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!register_window_class(instance)) {
        klive_set_message("注册镜像窗口类失败");
        return KLIVE_E_WINDOW;
    }

    const RECT& rect = target.desc.DesktopCoordinates;
    const int window_width = rect.right - rect.left;
    const int window_height = rect.bottom - rect.top;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kWindowClass, L"klive", WS_POPUP, rect.left, rect.top,
                                window_width, window_height, nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        klive_set_message("创建镜像窗口失败（错误码 %lu）", static_cast<unsigned long>(GetLastError()));
        return KLIVE_E_WINDOW;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, window_width, window_height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    g_window.width = static_cast<uint32_t>(window_width);
    g_window.height = static_cast<uint32_t>(window_height);
    g_window.resized = false;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> dxgi_adapter;
    ComPtr<IDXGIFactory2> factory;
    device.As(&dxgi_device);
    if (dxgi_device) dxgi_device->GetAdapter(&dxgi_adapter);
    if (dxgi_adapter) dxgi_adapter->GetParent(IID_PPV_ARGS(&factory));

    if (!factory) {
        klive_set_message("获取 DXGI 工厂失败");
        DestroyWindow(hwnd);
        return KLIVE_E_DEVICE;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = static_cast<UINT>(window_width);
    swap_desc.Height = static_cast<UINT>(window_height);
    swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swap_chain;
    HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &swap_desc, nullptr, nullptr,
                                                 &swap_chain);
    if (FAILED(hr)) {
        set_dxgi_error("创建交换链", hr);
        DestroyWindow(hwnd);
        return KLIVE_E_DEVICE;
    }

    ComPtr<ID3D11RenderTargetView> render_target;
    auto recreate_render_target = [&](UINT width, UINT height) -> bool {
        render_target.Reset();
        if (width == 0 || height == 0) return false;
        if (FAILED(swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
        if (FAILED(device->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target))) return false;

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        return true;
    };

    if (!recreate_render_target(static_cast<UINT>(window_width), static_cast<UINT>(window_height))) {
        klive_set_message("创建渲染目标失败");
        DestroyWindow(hwnd);
        return KLIVE_E_DEVICE;
    }

    Pipeline pipeline;
    if (!pipeline.create(device.Get())) {
        klive_set_message("初始化渲染管线失败");
        DestroyWindow(hwnd);
        return KLIVE_E_DEVICE;
    }

    // 采集后端：窗口只能走 WGC；显示器可选桌面复制（独占）或 WGC（共享）
    const bool use_wgc = window_source || config.capture_backend == KLIVE_CAPTURE_WGC;

    std::unique_ptr<FrameSource> frame_source;
    if (window_source) {
        frame_source = klive_create_wgc_window_source(
            reinterpret_cast<HWND>(static_cast<intptr_t>(config.window_handle)), device.Get());
    } else if (use_wgc) {
        frame_source = klive_create_wgc_monitor_source(source.desc.Monitor, device.Get());
    } else {
        frame_source = klive_create_duplication_source(source, device.Get());
    }
    if (!frame_source) {
        if (klive_message()[0] == '\0') klive_set_message("开启帧采集失败");
        DestroyWindow(hwnd);
        return window_source ? KLIVE_E_WINDOW : (use_wgc ? KLIVE_E_CAPTURE : KLIVE_E_DUPLICATION);
    }

    RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    int32_t result = KLIVE_OK;
    auto last_fps_sample = Clock::now();
    int64_t frames_since_sample = 0;
    bool quit = false;

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
            recreate_render_target(g_window.width, g_window.height);
        }

        ComPtr<ID3D11Texture2D> frame_texture;
        if (!frame_source->grab(context.Get(), frame_texture)) {
            if (frame_source->error() != KLIVE_OK) {
                result = frame_source->error();
                break;
            }
            continue;
        }

        D3D11_TEXTURE2D_DESC desc{};
        frame_texture->GetDesc(&desc);

        const UINT swap_width = g_window.width;
        const UINT swap_height = g_window.height;
        if (!render_target || swap_width == 0 || swap_height == 0) continue;

        float scale_x = 1.0f;
        float scale_y = 1.0f;
        if (config.scale_mode == 0 && desc.Width > 0 && desc.Height > 0) {
            const float fit = std::min(static_cast<float>(swap_width) / static_cast<float>(desc.Width),
                                       static_cast<float>(swap_height) / static_cast<float>(desc.Height));
            scale_x = static_cast<float>(desc.Width) * fit / static_cast<float>(swap_width);
            scale_y = static_cast<float>(desc.Height) * fit / static_cast<float>(swap_height);
        }

        ScaleParams params{{scale_x, scale_y}, {0.0f, 0.0f}};
        context->UpdateSubresource(pipeline.params.Get(), 0, nullptr, &params, 0, 0);

        ID3D11ShaderResourceView* frame_view = frame_source->view();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(render_target.Get(), clear);
        context->OMSetRenderTargets(1, render_target.GetAddressOf(), nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->RSSetState(pipeline.rasterizer.Get());
        context->VSSetShader(pipeline.vertex.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, pipeline.params.GetAddressOf());
        context->PSSetShader(pipeline.pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &frame_view);
        context->PSSetSamplers(0, 1, pipeline.sampler.GetAddressOf());
        context->Draw(4, 0);

        hr = swap_chain->Present(config.vsync ? 1 : 0, 0);
        if (FAILED(hr)) {
            set_dxgi_error("呈现画面", hr);
            result = KLIVE_E_DEVICE;
            break;
        }

        session.frames.fetch_add(1);
        session.frame_width = static_cast<int32_t>(desc.Width);
        session.frame_height = static_cast<int32_t>(desc.Height);
        ++frames_since_sample;

        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fps_sample);
        if (elapsed.count() >= 500) {
            session.fps = static_cast<double>(frames_since_sample) * 1000.0 /
                          static_cast<double>(elapsed.count());
            frames_since_sample = 0;
            last_fps_sample = now;
        }
    }

    UnregisterHotKey(nullptr, kHotkeyId);
    session.fps = 0.0;
    DestroyWindow(hwnd);
    return result;
}

KliveMirrorSession g_session;

}  // namespace

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

    // 先在调用线程做一次参数校验，错误可以同步返回给 UI
    std::vector<KliveOutput> outputs = klive_collect_outputs();
    if (outputs.empty()) {
        klive_set_message("没有检测到可用显示器");
        return KLIVE_E_NO_MONITOR;
    }
    const bool window_source = config.source_kind == KLIVE_SOURCE_WINDOW;
    if (config.target_index < 0 || config.target_index >= static_cast<int32_t>(outputs.size())) {
        klive_set_message("目标显示器编号无效");
        return KLIVE_E_INVALID_ARG;
    }
    if (window_source) {
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

    session.config = config;
    session.stop = false;
    session.last_error = KLIVE_OK;
    session.frames = 0;
    session.fps = 0.0;
    session.frame_width = 0;
    session.frame_height = 0;
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
