#pragma once

/*
 * AMD Video Coding Engine (AMF) H.264 encoder via amfrt64.dll (loaded
 * dynamically; no SDK link step). Headers live in host/third_party/amf
 * (see fetch.bat).
 *
 * The VCE H.264 encoder accepts shared DX11 surfaces, so each captured BGRA
 * frame is converted to NV12 on the CPU and uploaded (UpdateSubresource) into
 * a small pool of D3D11_RESOURCE_MISC_SHARED NV12 textures, each wrapped once
 * in an AMFSurface at Start(). The crop is set to the real (unaligned) frame
 * size so the encoder encodes the visible area, not the 16-aligned padding.
 *
 * Output is an Annex B access unit; NAL units are split out WITHOUT start
 * codes to match the wire contract. A keyframe is any access unit containing
 * an IDR slice (NAL type 5).
 */

#include <cstdint>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "core/Context.h"
#include "core/Data.h"
#include "core/Factory.h"
#include "core/PropertyStorage.h"
#include "core/PropertyStorageEx.h"
#include "core/Result.h"
#include "core/Surface.h"
#include "core/Version.h"
#include "components/Component.h"
#include "components/VideoEncoderVCE.h"

#include "encode/encoder.h"
#include "util/aligned.h"

namespace twin {

class AmfEncoder : public Encoder {
public:
    AmfEncoder() = default;
    ~AmfEncoder() override { Stop(); }

    AmfEncoder(const AmfEncoder&) = delete;
    AmfEncoder& operator=(const AmfEncoder&) = delete;

    bool Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) override;
    bool Encode(ID3D11Texture2D* frame, EncodedFrame& out) override;
    bool RequestKeyframe() override { keyframe_requested_ = true; return true; }
    bool SetBitrate(uint32_t bitrate) override;
    void SetD3D11Device(ID3D11Device* device) override { device_ = device; }
    const char* Name() const override { return "amf"; }

private:
    bool LoadRuntime();
    void Stop();
    bool CopyFrameToNv12(ID3D11Texture2D* frame);
    bool ReadOutput(EncodedFrame& out);

    void* amf_dll_ = nullptr;
    AMF_RESULT(AMF_CDECL_CALL* amf_init_)(amf_uint64, amf::AMFFactory**) =
        nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr ctx_;
    amf::AMFComponentPtr encoder_;
    std::vector<amf::AMFSurfacePtr> surfaces_; /* one per pool texture */
    size_t surface_index_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> dctx_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> nv12_tex_;

    detail::AlignedBytes nv12_; /* enc_w*enc_h + enc_w*enc_h/2 */
    uint32_t w_ = 0, h_ = 0, fps_ = 30, bitrate_ = 0;
    uint32_t enc_w_ = 0, enc_h_ = 0; /* Width/Height rounded up to 16 */
    bool keyframe_requested_ = true;
};

}  // namespace twin
