#include "frame_source.h"

#include <dxgi1_2.h>

namespace {

/**
 * DXGI Desktop Duplication 后端。
 *
 * 独占：同一台显示器同一时间只能有一个 duplication 消费者，
 * 所以它和 OBS 的「显示器采集」不能共存。
 */
class DuplicationSource final : public FrameSource {
public:
    DuplicationSource(ComPtr<IDXGIOutput1> output, ComPtr<ID3D11Device> device)
        : output_(std::move(output)), device_(std::move(device)) {}

    bool open() {
        const HRESULT hr = output_->DuplicateOutput(device_.Get(), &duplication_);
        if (FAILED(hr)) {
            klive_set_dxgi_error("开启桌面复制", hr);
            error_ = KLIVE_E_DUPLICATION;
            return false;
        }
        return true;
    }

    bool grab(ID3D11DeviceContext* context, ComPtr<ID3D11Texture2D>& out) override {
        if (error_ != KLIVE_OK) return false;

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(16, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // 分辨率变化 / 全屏切换后会话会失效，重建一次继续跑
            duplication_.Reset();
            hr = output_->DuplicateOutput(device_.Get(), &duplication_);
            if (FAILED(hr)) {
                klive_set_dxgi_error("重建桌面复制", hr);
                error_ = KLIVE_E_DUPLICATION;
            }
            return false;
        }
        if (FAILED(hr)) {
            klive_set_dxgi_error("获取桌面帧", hr);
            error_ = KLIVE_E_DUPLICATION;
            return false;
        }

        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resource.As(&source))) {
            duplication_->ReleaseFrame();
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);

        bool ok = copy_.ensure(device_.Get(), desc);
        ID3D11Texture2D* frame = ok ? copy_.copy_from(context, source.Get()) : nullptr;

        // 必须先释放帧，再交给上层渲染；中转纹理已经是独立资源
        source.Reset();
        resource.Reset();
        duplication_->ReleaseFrame();

        if (frame == nullptr) {
            error_ = KLIVE_E_DEVICE;
            return false;
        }
        out = frame;
        return true;
    }

    const char* name() const override { return "Desktop Duplication"; }

    ID3D11ShaderResourceView* view() const override { return copy_.view(); }

private:
    ComPtr<IDXGIOutput1> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    FrameCopy copy_;
};

}  // namespace

std::unique_ptr<FrameSource> klive_create_duplication_source(const KliveOutput& output,
                                                             ID3D11Device* device) {
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.output.As(&output1))) {
        klive_set_message("该显示器不支持桌面复制（需要 Windows 8 及以上 / DXGI 1.2）");
        return nullptr;
    }

    ComPtr<ID3D11Device> device_ref = device;
    auto source = std::make_unique<DuplicationSource>(std::move(output1), std::move(device_ref));
    if (!source->open()) return nullptr;
    return source;
}
