#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace twin {

/*
 * Captures a monitor via Desktop Duplication API.
 *
 * The duplicated surface lives in *desktop* coordinates. Input events coming
 * back from the tablet are in monitor-local coordinates, so add the monitor's
 * origin (OffsetX/OffsetY) before injecting.
 *
 * The returned texture is a GPU-side (DEFAULT usage) copy of the monitor
 * region — the same texture handed to the encoder (NVENC reads it directly).
 * CPU readers (NullEncoder) must copy to their own staging texture first.
 */
class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture() { Stop(); }

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool Start(HMONITOR hmon);
    void Stop();

    /* Returns true when a new frame is available. The texture is owned by the
     * capture object and valid until the next Capture() call. */
    bool Capture(ID3D11Texture2D** out_tex);

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    int32_t OffsetX() const { return offset_x_; }
    int32_t OffsetY() const { return offset_y_; }
    bool Running() const { return running_; }
    ID3D11Device* Device() const { return device_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_;
    DXGI_OUTDUPL_FRAME_INFO info_{};

    bool running_ = false;
    uint32_t width_ = 0, height_ = 0;
    int32_t offset_x_ = 0, offset_y_ = 0;
};

}  // namespace twin
