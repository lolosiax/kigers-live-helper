#ifndef KLIVE_OSD_H
#define KLIVE_OSD_H

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

/**
 * 镜像画面左上角的 OSD 文字层。
 *
 * 文字先用 GDI 画进 32 位 DIB，再转成预乘 alpha 的 BGRA 纹理，最后由 D3D11
 * 叠在镜像画面上。面板宽度按镜像窗口宽度取比例（1/10 ~ 1/7），字号随之放大，
 * 保证在 AR 眼镜里也看得清。
 */
class OsdOverlay {
public:
    /** 设置要显示的文字；text 为空表示不显示。window_width 是镜像窗口的物理像素宽。 */
    void set_text(const wchar_t* text, uint32_t window_width);

    void hide();

    bool visible() const { return !text_.empty(); }

    /** 设备重建时丢弃 GPU 资源；文字保留，下次 draw 按新设备重建。 */
    void reset();

    /** 在镜像画面上叠加 OSD；未设置文字或资源创建失败时什么都不做。 */
    void draw(ID3D11DeviceContext* context, ID3D11Device* device, uint32_t window_width,
              uint32_t window_height);

private:
    bool ensure(ID3D11Device* device, uint32_t window_width);
    bool create_resources(ID3D11Device* device);
    bool rebuild_bitmap(uint32_t window_width);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> params_;

    std::wstring text_;
    uint32_t bitmap_width_ = 0;
    uint32_t bitmap_height_ = 0;
    uint32_t window_width_ = 0;
    bool failed_ = false;
};

#endif /* KLIVE_OSD_H */
