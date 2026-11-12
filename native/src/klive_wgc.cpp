#include "frame_source.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::SizeInt32;

/**
 * 线程内初始化 COM。镜像 worker 是普通 std::thread，没有现成的 apartment，
 * WGC 的对象都要求在这个线程上创建。
 */
void ensure_apartment() {
    static thread_local bool initialized = false;
    if (initialized) return;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const&) {
        // 已经是别的 apartment 模式，继续用即可
    }
    initialized = true;
}

IDirect3DDevice create_rt_device(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgi_device;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi_device.put())));

    IDirect3DDevice result{nullptr};
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
        dxgi_device.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(result))));
    return result;
}

GraphicsCaptureItem create_item(HMONITOR monitor, HWND window) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    const HRESULT hr =
        (monitor != nullptr)
            ? interop->CreateForMonitor(
                  monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                  winrt::put_abi(item))
            : interop->CreateForWindow(
                  window, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                  winrt::put_abi(item));
    winrt::check_hresult(hr);
    return item;
}

ComPtr<ID3D11Texture2D> texture_from_surface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const& surface) {
    ::IUnknown* unknown = winrt::get_unknown(surface);
    ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    winrt::check_hresult(unknown->QueryInterface(IID_PPV_ARGS(&access)));

    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    return texture;
}

/** WGC 后端：显示器与窗口都能采，且可以与其它程序同时采集同一画面。 */
class WgcSource final : public FrameSource {
public:
    WgcSource(GraphicsCaptureItem item, const char* label)
        : item_(std::move(item)), label_(label) {}

    bool open(ID3D11Device* device) {
        device_ = device;
        try {
            ensure_apartment();
            rt_device_ = create_rt_device(device);

            const SizeInt32 size = item_.Size();
            pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                rt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, kFrameCount, size);
            session_ = pool_.CreateCaptureSession(item_);
            session_.IsCursorCaptureEnabled(true);
            disable_border();
            session_.StartCapture();

            pool_width_ = size.Width;
            pool_height_ = size.Height;
            return true;
        } catch (winrt::hresult_error const& e) {
            fail("开启 WGC 采集", e);
            return false;
        }
    }

    bool grab(ID3D11DeviceContext* context, ComPtr<ID3D11Texture2D>& out) override {
        if (error_ != KLIVE_OK) return false;
        try {
            Direct3D11CaptureFrame frame{nullptr};
            // 没有新帧时小睡一会，避免空转占满一个核
            for (int attempt = 0; attempt < 4 && !frame; ++attempt) {
                frame = pool_.TryGetNextFrame();
                if (!frame) Sleep(2);
            }
            if (!frame) return false;

            const SizeInt32 size = frame.ContentSize();
            if (size.Width != pool_width_ || size.Height != pool_height_) {
                // 窗口 / 显示器尺寸变了：帧池和这帧都作废，下一轮重来
                pool_width_ = size.Width;
                pool_height_ = size.Height;
                frame.Close();
                if (size.Width > 0 && size.Height > 0) {
                    pool_.Recreate(rt_device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, kFrameCount,
                                   size);
                }
                return false;
            }

            ComPtr<ID3D11Texture2D> source = texture_from_surface(frame.Surface());
            if (!source) return false;

            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);

            if (!copy_.ensure(device_.Get(), desc)) {
                error_ = KLIVE_E_DEVICE;
                return false;
            }
            ID3D11Texture2D* frame_texture = copy_.copy_from(context, source.Get());
            if (frame_texture == nullptr) {
                error_ = KLIVE_E_DEVICE;
                return false;
            }
            out = frame_texture;
            return true;
        } catch (winrt::hresult_error const& e) {
            fail("WGC 采集", e);
            return false;
        }
    }

    const char* name() const override { return label_; }

    ID3D11ShaderResourceView* view() const override { return copy_.view(); }

private:
    static constexpr int32_t kFrameCount = 2;

    void disable_border() {
        try {
            // 关掉捕获边框（Windows 11 22000+ 才有，旧系统会抛异常）
            session_.IsBorderRequired(false);
        } catch (...) {
        }
    }

    void fail(const char* stage, winrt::hresult_error const& e) {
        klive_set_message("%s 失败：%ls (0x%08lX)", stage, e.message().c_str(),
                          static_cast<unsigned long>(e.code()));
        error_ = KLIVE_E_CAPTURE;
    }

    GraphicsCaptureItem item_{nullptr};
    const char* label_ = "WGC";
    ComPtr<ID3D11Device> device_;
    IDirect3DDevice rt_device_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    FrameCopy copy_;
    int32_t pool_width_ = 0;
    int32_t pool_height_ = 0;
};

}  // namespace

std::unique_ptr<FrameSource> klive_create_wgc_monitor_source(HMONITOR monitor,
                                                             ID3D11Device* device) {
    if (monitor == nullptr) {
        klive_set_message("该显示器不支持 WGC 采集");
        return nullptr;
    }
    try {
        ensure_apartment();
        auto source = std::make_unique<WgcSource>(create_item(monitor, nullptr), "WGC 显示器");
        if (!source->open(device)) return nullptr;
        return source;
    } catch (winrt::hresult_error const& e) {
        klive_set_message("WGC 显示器采集初始化失败：%ls (0x%08lX)", e.message().c_str(),
                          static_cast<unsigned long>(e.code()));
        return nullptr;
    }
}

std::unique_ptr<FrameSource> klive_create_wgc_window_source(HWND window, ID3D11Device* device) {
    if (window == nullptr || !IsWindow(window)) {
        klive_set_message("窗口已关闭或不可采集");
        return nullptr;
    }
    try {
        ensure_apartment();
        auto source = std::make_unique<WgcSource>(create_item(nullptr, window), "WGC 窗口");
        if (!source->open(device)) return nullptr;
        return source;
    } catch (winrt::hresult_error const& e) {
        klive_set_message("WGC 窗口采集初始化失败：%ls (0x%08lX)", e.message().c_str(),
                          static_cast<unsigned long>(e.code()));
        return nullptr;
    }
}
