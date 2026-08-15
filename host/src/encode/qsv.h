#pragma once

/*
 * Intel Quick Sync Video (QSV) H.264 encoder via the legacy Intel Media SDK /
 * oneVPL runtime. Headers live in host/third_party/qsv (see fetch.bat); the
 * runtime DLL is loaded dynamically, so there is no build-time dependency on
 * the driver:
 *
 *   candidate DLLs (in order): libmfxhw64.dll, libmfx64.dll, vpl.dll
 *
 * The session uses system-memory NV12 input (MFX_IOPATTERN_IN_SYSTEM_MEMORY),
 * so no D3D11 frame allocator is needed. Each captured BGRA frame is copied to
 * a CPU staging texture and converted to NV12 before submission.
 *
 * Output is an Annex B access unit; NAL units are split out WITHOUT start
 * codes to match the wire contract (the receiver prepends 0x00000001). A
 * keyframe is any access unit containing an IDR slice (NAL type 5).
 */

#define MFX_DEPRECATED_OFF

#include <cstdint>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "mfx.h"

#include "encode/encoder.h"
#include "util/aligned.h"

namespace twin {

class QsvEncoder : public Encoder {
public:
    QsvEncoder() = default;
    ~QsvEncoder() override { Stop(); }

    QsvEncoder(const QsvEncoder&) = delete;
    QsvEncoder& operator=(const QsvEncoder&) = delete;

    bool Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) override;
    bool Encode(ID3D11Texture2D* frame, EncodedFrame& out) override;
    bool RequestKeyframe() override { keyframe_requested_ = true; return true; }
    bool SetBitrate(uint32_t bitrate) override;
    const char* Name() const override { return "qsv"; }

private:
    bool LoadRuntime();
    void UnloadRuntime();
    bool InitSession();
    bool Configure();
    void Stop();
    bool CopyFrameToNv12(ID3D11Texture2D* frame);

    void* qsv_dll_ = nullptr;
    mfxSession session_ = nullptr;
    bool configured_ = false;

    mfxStatus(MFX_CDECL* mfx_init_ex_)(mfxInitParam, mfxSession*) = nullptr;
    mfxStatus(MFX_CDECL* mfx_close_)(mfxSession) = nullptr;
    mfxStatus(MFX_CDECL* enc_init_)(mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus(MFX_CDECL* enc_reset_)(mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus(MFX_CDECL* enc_close_)(mfxSession) = nullptr;
    mfxStatus(MFX_CDECL* enc_frame_)(mfxSession, mfxEncodeCtrl*, mfxFrameSurface1*,
                                     mfxBitstream*, mfxSyncPoint*) = nullptr;
    mfxStatus(MFX_CDECL* sync_op_)(mfxSession, mfxSyncPoint, mfxU32) = nullptr;

    uint32_t w_ = 0, h_ = 0, fps_ = 30, bitrate_ = 0;
    uint32_t enc_w_ = 0, enc_h_ = 0; /* Width/Height rounded up to 16 */
    bool keyframe_requested_ = true;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    /* 32-byte aligned buffers (mfxBitstream.Data and frame data require it). */
    detail::AlignedBytes nv12_; /* enc_w*enc_h + enc_w*enc_h/2 */
    detail::AlignedBytes bs_;   /* output bitstream capacity */
    mfxVideoParam param_{};      /* post-Init (negotiated) */
    mfxVideoParam user_param_{}; /* pre-Init (as configured) */
    mfxExtCodingOption2 co2_{};
    mfxExtBuffer* ext_params_[1] = {&co2_.Header};
};

}  // namespace twin
