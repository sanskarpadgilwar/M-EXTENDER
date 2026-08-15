#pragma once

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

namespace twin {

/*
 * D3D11 GPU bilinear downscaler used for adaptive resolution: under link
 * congestion the host captures at native resolution but encodes a scaled-down
 * copy, cutting the bitrate needed per frame.
 *
 * The HLSL fullscreen-triangle blit is compiled at runtime via D3DCompile
 * (d3dcompiler_47.dll ships with Windows 10+), so there is no shader binary
 * checked in. The source texture must be B8G8R8A8_UNORM and the device must
 * match the one the encoders use (so the scaled texture is encodable by all
 * hardware paths).
 */
class GpuScaler {
public:
    GpuScaler() = default;
    ~GpuScaler() = default;

    GpuScaler(const GpuScaler&) = delete;
    GpuScaler& operator=(const GpuScaler&) = delete;

    /* Compiles the blit shaders; returns false if D3DCompile fails. */
    bool Init(ID3D11Device* device);

    /* Returns a texture of size (w, h) containing a bilinear-scaled copy of
     * `src`. The texture is owned by the scaler and reused across calls; it is
     * valid until the next Scale() call (or until w/h change, which recreates
     * it). Returns nullptr on failure. */
    ID3D11Texture2D* Scale(ID3D11Texture2D* src, uint32_t w, uint32_t h);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> dst_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    uint32_t dst_w_ = 0, dst_h_ = 0;
};

}  // namespace twin
