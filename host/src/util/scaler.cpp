#include "util/scaler.h"

#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "util/log.h"

namespace twin {

namespace {

const char kBlitShader[] =
    /* Fullscreen triangle: vertex id maps to clip space + uv in one move. */
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    o.uv = float2(float(id & 1), float((id >> 1) & 1));\n"
    "    o.pos = float4(o.uv * 4.0 - 1.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_Target {\n"
    "    return tex.Sample(smp, i.uv);\n"
    "}\n";

bool Compile(const char* entry, const char* target,
             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kBlitShader, std::strlen(kBlitShader), nullptr,
                            nullptr, nullptr, entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        const char* msg =
            err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown";
        Log("GpuScaler: D3DCompile(%s) failed 0x%08x: %s", entry, hr, msg);
        return false;
    }
    return true;
}

}  // namespace

bool GpuScaler::Init(ID3D11Device* device) {
    device_ = device;
    if (!device_) {
        Log("GpuScaler: no device");
        return false;
    }
    device_->GetImmediateContext(&ctx_);
    if (!ctx_) {
        Log("GpuScaler: no immediate context");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    if (!Compile("VSMain", "vs_4_0", vs_blob) ||
        !Compile("PSMain", "ps_4_0", ps_blob))
        return false;
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), nullptr,
                                           &vs_))) {
        Log("GpuScaler: CreateVertexShader failed");
        return false;
    }
    if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                          ps_blob->GetBufferSize(), nullptr,
                                          &ps_))) {
        Log("GpuScaler: CreatePixelShader failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        Log("GpuScaler: CreateSamplerState failed");
        return false;
    }
    Log("GpuScaler: init ok");
    return true;
}

ID3D11Texture2D* GpuScaler::Scale(ID3D11Texture2D* src, uint32_t w, uint32_t h) {
    if (!ctx_ || !src || w == 0 || h == 0)
        return nullptr;

    if (!dst_ || dst_w_ != w || dst_h_ != h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &dst_))) {
            Log("GpuScaler: CreateTexture2D %ux%u failed", w, h);
            return nullptr;
        }
        rtv_.Reset();
        if (FAILED(device_->CreateRenderTargetView(dst_.Get(), nullptr, &rtv_))) {
            Log("GpuScaler: CreateRenderTargetView failed");
            return nullptr;
        }
        dst_w_ = w;
        dst_h_ = h;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(src, nullptr, &srv))) {
        Log("GpuScaler: CreateShaderResourceView failed");
        return nullptr;
    }

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, srv.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->Draw(3, 0);

    /* Unbind so encoders can safely read dst_ (copy/register) next. */
    ID3D11ShaderResourceView* null_srv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[1] = {nullptr};
    ctx_->OMSetRenderTargets(1, null_rtv, nullptr);

    return dst_.Get();
}

}  // namespace twin
