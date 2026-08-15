#include "encode/encoder.h"

#include <cstring>

#include <wrl/client.h>

#include "util/log.h"

namespace twin {

bool NullEncoder::Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
    w_ = w;
    h_ = h;
    Log("NullEncoder: %ux%u @ %u fps (%u bps)", w, h, fps, bitrate);
    return true;
}

bool NullEncoder::Encode(ID3D11Texture2D* frame, EncodedFrame& out) {
    if (!frame) return false;

    if (!staging_) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        frame->GetDevice(&device);
        device->GetImmediateContext(&ctx_);

        D3D11_TEXTURE2D_DESC d{};
        frame->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &staging_))) return false;
    }

    ctx_->CopyResource(staging_.Get(), frame);

    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr = ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) return false;

    out.keyframe = false;
    out.codec = 0;
    out.raw.resize(static_cast<size_t>(h_) * map.RowPitch);
    std::memcpy(out.raw.data(), map.pData, out.raw.size());

    ctx_->Unmap(staging_.Get(), 0);
    return true;
}

}  // namespace twin
