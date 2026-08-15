#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace twin {

struct EncodedFrame {
    bool keyframe = false;
    int codec = 0;             /* SP_CODEC_H264 */
    std::vector<std::vector<uint8_t>> nals; /* zero nals if raw */
    std::vector<uint8_t> raw;               /* raw RGBA when nals empty */
};

/*
 * Abstract encoder. Implementations: NullEncoder (bring-up), then NVENC / QSV /
 * AMF / MF-HW.
 */
class Encoder {
public:
    virtual ~Encoder() = default;
    virtual bool Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) = 0;
    virtual bool Encode(ID3D11Texture2D* frame, EncodedFrame& out) = 0;
    virtual bool RequestKeyframe() = 0;
    /* Adaptive bitrate: re-target the encoder; false if unsupported. */
    virtual bool SetBitrate(uint32_t /*bitrate*/) { return false; }
    /* Hands the encoder the capture's D3D11 device. Encoders that create
     * their own device (NVENC) ignore this; AMF needs it for InitDX11. */
    virtual void SetD3D11Device(ID3D11Device* /*device*/) {}
    virtual const char* Name() const = 0;
};

/*
 * Copies the frame to CPU memory as raw RGBA (row pitch = w*4). Used to prove
 * the capture + transport path before any hardware encoder is integrated.
 */
class NullEncoder : public Encoder {
public:
    bool Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) override;
    bool Encode(ID3D11Texture2D* frame, EncodedFrame& out) override;
    bool RequestKeyframe() override { return true; }
    const char* Name() const override { return "null"; }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    uint32_t w_ = 0, h_ = 0;
};

}  // namespace twin
