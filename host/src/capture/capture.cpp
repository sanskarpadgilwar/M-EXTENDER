#include "capture/capture.h"

#include <dxgi.h>

#include "util/log.h"

namespace twin {

bool ScreenCapture::Start(HMONITOR hmon) {
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Log("CreateDXGIFactory2 failed 0x%08x", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> found_adapter;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++ai) {
        Microsoft::WRL::ComPtr<IDXGIOutput> out;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &out) != DXGI_ERROR_NOT_FOUND;
             ++oi) {
            DXGI_OUTPUT_DESC d;
            out->GetDesc(&d);
            if (d.Monitor == hmon) {
                out.As(&output_);
                adapter.As(&found_adapter);
                break;
            }
        }
        if (output_) break;
    }
    if (!output_) {
        Log("monitor 0x%p not found among outputs", hmon);
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL level;
    hr = D3D11CreateDevice(found_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           flags, nullptr, 0, D3D11_SDK_VERSION, &device_, &level,
                           &ctx_);
    if (FAILED(hr)) {
        Log("D3D11CreateDevice failed 0x%08x", hr);
        return false;
    }

    hr = output_->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        Log("DuplicateOutput failed 0x%08x", hr);
        return false;
    }

    DXGI_OUTPUT_DESC od;
    output_->GetDesc(&od);
    width_ = static_cast<uint32_t>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    height_ = static_cast<uint32_t>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    offset_x_ = od.DesktopCoordinates.left;
    offset_y_ = od.DesktopCoordinates.top;
    running_ = true;
    Log("capture started: %ux%u at offset (%d,%d)", width_, height_, offset_x_,
        offset_y_);
    return true;
}

void ScreenCapture::Stop() {
    running_ = false;
    if (dup_) {
        dup_->ReleaseFrame();
        dup_.Reset();
    }
    copy_.Reset();
    output_.Reset();
    ctx_.Reset();
    device_.Reset();
}

bool ScreenCapture::Capture(ID3D11Texture2D** out_tex) {
    if (!running_ || !dup_) return false;

    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(16, &info_, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        dup_->ReleaseFrame();
        return false;
    }
    if (FAILED(hr)) {
        dup_->ReleaseFrame();
        if (hr == DXGI_ERROR_ACCESS_LOST && output_) {
            /*
             * The DDA interface dies when the output mode/session changes
             * (resolution change, lock screen, driver reset). Re-acquire it
             * so capture resumes instead of going deaf forever. A locked
             * desktop yields E_ACCESSDENIED, so back off and retry so the
             * host picks up again once the session is unlocked.
             */
            dup_.Reset();
            const DWORD now = ::GetTickCount();
            if (now >= next_reacquire_) {
                Log("capture: access lost; re-acquiring output duplication");
                hr = output_->DuplicateOutput(device_.Get(), &dup_);
                next_reacquire_ = now + 1000; /* at most once per second */
                if (FAILED(hr))
                    Log("capture: re-acquire failed 0x%08x (retry in 1s)", hr);
            }
            return false;
        }
        Log("AcquireNextFrame failed 0x%08x", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> surface;
    res.As(&surface);

    D3D11_TEXTURE2D_DESC sd;
    surface->GetDesc(&sd);
    if (!copy_) {
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = width_;
        cd.Height = height_;
        cd.MipLevels = 1;
        cd.ArraySize = 1;
        cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT; /* GPU-side: encodable by NVENC */
        cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&cd, nullptr, &copy_))) {
            dup_->ReleaseFrame();
            return false;
        }
    }

    /*
     * The duplicated frame covers the whole virtual desktop. Copy only this
     * monitor's region (offset_x_/offset_y_ are its DesktopCoordinates origin).
     * Assumes non-negative offsets (monitors right/below the primary).
     */
    D3D11_BOX box{
        static_cast<UINT>(offset_x_),  static_cast<UINT>(offset_y_), 0,
        static_cast<UINT>(offset_x_ + width_), static_cast<UINT>(offset_y_ + height_),
        1,
    };
    ctx_->CopySubresourceRegion(copy_.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
    dup_->ReleaseFrame();

    *out_tex = copy_.Get();
    return true;
}

}  // namespace twin
