#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "nvEncodeAPI.h"

#include "encode/encoder.h"

namespace twin {

/*
 * Hardware H.264 encoder using NVIDIA NVENC.
 *
 * Loads nvEncodeAPI64.dll at runtime (shipped by the NVIDIA driver), so the
 * only build-time dependency is the SDK header:
 *
 *     place <NVIDIA Video Codec SDK>/Interface/nvEncodeAPI.h into
 *     host/third_party/nvenc/nvEncodeAPI.h
 *
 * Configuration targets low-latency interactive streaming:
 *   - CBR, no lookahead, no B-frames, 2s GOP (IDR), repeat SPS/PPS on IDR
 *   - sync mode (one frame of latency, simplest correctness)
 *   - Main profile H.264
 *
 * Encode() emits NAL units WITHOUT start codes (raw access-unit payloads),
 * matching the wire contract; the receiver prepends 0x00000001.
 */
class NvEncoder : public Encoder {
public:
    NvEncoder() = default;
    ~NvEncoder() override { Stop(); }

    NvEncoder(const NvEncoder&) = delete;
    NvEncoder& operator=(const NvEncoder&) = delete;

    bool Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) override;
    bool Encode(ID3D11Texture2D* frame, EncodedFrame& out) override;
    bool RequestKeyframe() override { keyframe_requested_ = true; return true; }
    bool SetBitrate(uint32_t bitrate);
    const char* Name() const override { return "nvenc"; }

private:
    bool LoadApi();
    bool OpenSession(ID3D11Device* device);
    bool Configure();
    bool RegisterInput(ID3D11Texture2D* frame);
    bool CreateBitstream();
    void Stop();

    uint32_t w_ = 0, h_ = 0, fps_ = 30, bitrate_ = 0;
    bool keyframe_requested_ = true;

    void* nvenc_dll_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    void* encoder_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST* api_ = nullptr;

    void* registered_input_ = nullptr;
    void* bitstream_buffer_ = nullptr;
    uint32_t frame_index_ = 0;
    bool configured_ = false;
    NV_ENC_INITIALIZE_PARAMS init_params_{};
    NV_ENC_CONFIG config_{};
};

}  // namespace twin
