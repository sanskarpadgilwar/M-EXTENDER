#include "encode/qsv.h"

#include <algorithm>

#include "encode/color.h"
#include "encode/nal.h"
#include "util/log.h"

#if defined(TWIN_HAVE_QSV)

namespace twin {
namespace {

template <typename T>
bool Resolve(HMODULE dll, const char* name, T& fn) {
    fn = reinterpret_cast<T>(GetProcAddress(dll, name));
    return fn != nullptr;
}

}  // namespace

bool QsvEncoder::Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
    w_ = w;
    h_ = h;
    fps_ = fps ? fps : 30;
    bitrate_ = bitrate;
    keyframe_requested_ = true;

    if (!LoadRuntime())
        return false;
    if (!InitSession()) {
        UnloadRuntime();
        return false;
    }
    if (!Configure()) {
        Stop();
        return false;
    }
    Log("QSV: %ux%u @ %u fps, %u bps", w_, h_, fps_, bitrate_);
    return true;
}

void QsvEncoder::Stop() {
    if (enc_close_ && session_)
        enc_close_(session_);
    if (mfx_close_ && session_)
        mfx_close_(session_);
    session_ = nullptr;
    configured_ = false;
    UnloadRuntime();
}

bool QsvEncoder::LoadRuntime() {
    static const char* const kCandidates[] = {
        "libmfxhw64.dll", /* Intel driver runtime */
        "libmfx64.dll",   /* legacy dispatcher */
        "vpl.dll",        /* oneVPL dispatcher (exports legacy MFX API) */
    };
    for (const char* name : kCandidates) {
        HMODULE dll = LoadLibraryA(name);
        if (!dll)
            continue;
        if (!Resolve(dll, "MFXInitEx", mfx_init_ex_) ||
            !Resolve(dll, "MFXClose", mfx_close_) ||
            !Resolve(dll, "MFXVideoENCODE_Init", enc_init_) ||
            !Resolve(dll, "MFXVideoENCODE_Close", enc_close_) ||
            !Resolve(dll, "MFXVideoENCODE_EncodeFrameAsync", enc_frame_) ||
            !Resolve(dll, "MFXVideoCORE_SyncOperation", sync_op_)) {
            Log("QSV: %s is missing required exports; skipping", name);
            FreeLibrary(dll);
            continue;
        }
        if (!Resolve(dll, "MFXVideoENCODE_Reset", enc_reset_))
            Log("QSV: %s has no MFXVideoENCODE_Reset; SetBitrate disabled", name);
        qsv_dll_ = dll;
        Log("QSV: runtime loaded: %s", name);
        return true;
    }
    Log("QSV: no Intel Media SDK / oneVPL runtime found");
    return false;
}

void QsvEncoder::UnloadRuntime() {
    if (qsv_dll_) {
        FreeLibrary(static_cast<HMODULE>(qsv_dll_));
        qsv_dll_ = nullptr;
    }
    mfx_init_ex_ = nullptr;
    mfx_close_ = nullptr;
    enc_init_ = nullptr;
    enc_reset_ = nullptr;
    enc_close_ = nullptr;
    enc_frame_ = nullptr;
    sync_op_ = nullptr;
}

bool QsvEncoder::InitSession() {
    mfxInitParam init{};
    init.Implementation = MFX_IMPL_HARDWARE_ANY;
    init.Version.Major = 1;
    init.Version.Minor = 0;
    mfxStatus st = mfx_init_ex_(init, &session_);
    if (st != MFX_ERR_NONE) {
        /* Some legacy runtimes reject a non-zero version; retry with {0,0}. */
        init.Version.Major = 0;
        init.Version.Minor = 0;
        st = mfx_init_ex_(init, &session_);
    }
    if (st != MFX_ERR_NONE) {
        Log("QSV: MFXInitEx failed: %d", static_cast<int>(st));
        session_ = nullptr;
        return false;
    }
    return true;
}

bool QsvEncoder::Configure() {
    enc_w_ = (w_ + 15) & ~15u;
    enc_h_ = (h_ + 15) & ~15u;

    mfxVideoParam par{};
    par.AsyncDepth = 1;
    par.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    par.mfx.CodecId = MFX_CODEC_AVC;
    par.mfx.TargetUsage = MFX_TARGETUSAGE_1;
    par.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    par.mfx.TargetKbps = static_cast<mfxU16>(bitrate_ / 1000);
    par.mfx.MaxKbps = static_cast<mfxU16>(bitrate_ * 12 / 10 / 1000);
    par.mfx.BufferSizeInKB =
        static_cast<mfxU16>(std::max<uint32_t>(1, bitrate_ / fps_ / 1000));
    par.mfx.GopPicSize = static_cast<mfxU16>(fps_ * 2); /* IDR every 2 s */
    par.mfx.GopOptFlag = MFX_GOP_CLOSED;
    par.mfx.GopRefDist = 1; /* no B-frames */
    par.mfx.NumRefFrame = 1;

    par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    par.mfx.FrameInfo.Width = static_cast<mfxU16>(enc_w_);
    par.mfx.FrameInfo.Height = static_cast<mfxU16>(enc_h_);
    par.mfx.FrameInfo.CropX = 0;
    par.mfx.FrameInfo.CropY = 0;
    par.mfx.FrameInfo.CropW = static_cast<mfxU16>(w_);
    par.mfx.FrameInfo.CropH = static_cast<mfxU16>(h_);
    par.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU16>(fps_);
    par.mfx.FrameInfo.FrameRateExtD = 1;
    par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

    co2_ = {};
    co2_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    co2_.Header.BufferSz = sizeof(mfxExtCodingOption2);
    co2_.RepeatPPS = MFX_CODINGOPTION_ON; /* SPS/PPS in every IDR access unit */
    ext_params_[0] = &co2_.Header;
    par.NumExtParam = 1;
    par.ExtParam = ext_params_;

    mfxStatus st = enc_init_(session_, &par);
    if (st != MFX_ERR_NONE && st != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
        Log("QSV: MFXVideoENCODE_Init failed: %d", static_cast<int>(st));
        return false;
    }
    param_ = par;
    configured_ = true;

    if (!nv12_.Allocate(static_cast<size_t>(enc_w_) * enc_h_ * 3 / 2))
        return false;
    std::memset(nv12_.data(), 0x80, nv12_.size()); /* neutral chroma */
    const size_t bs_cap =
        std::max<size_t>(static_cast<size_t>(bitrate_) / fps_ * 4,
                         static_cast<size_t>(enc_w_) * enc_h_ / 2);
    if (!bs_.Allocate(bs_cap))
        return false;
    return true;
}

bool QsvEncoder::Encode(ID3D11Texture2D* frame, EncodedFrame& out) {
    if (!session_ || !configured_ || !frame)
        return false;
    if (!CopyFrameToNv12(frame))
        return false;

    mfxFrameSurface1 surface{};
    surface.Info = param_.mfx.FrameInfo;
    surface.Data.Y = nv12_.data();
    surface.Data.U = nv12_.data() + static_cast<size_t>(enc_w_) * enc_h_;
    surface.Data.Pitch = static_cast<mfxU16>(enc_w_);
    surface.Data.MemType = MFX_MEMTYPE_SYSTEM_MEMORY;

    mfxBitstream bs{};
    bs.Data = bs_.data();
    bs.MaxLength = static_cast<mfxU32>(bs_.size());

    mfxEncodeCtrl ctrl{};
    if (keyframe_requested_) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        keyframe_requested_ = false;
    }

    mfxSyncPoint sync = nullptr;
    mfxStatus st = enc_frame_(session_, &ctrl, &surface, &bs, &sync);
    if (st == MFX_ERR_MORE_DATA) {
        /* Input accepted and queued; output not ready yet (frame buffering). */
        return false;
    }
    if (st != MFX_ERR_NONE && st != MFX_WRN_IN_EXECUTION) {
        Log("QSV: EncodeFrameAsync failed: %d", static_cast<int>(st));
        return false;
    }
    if (sync) {
        st = sync_op_(session_, sync, MFX_INFINITE);
        if (st != MFX_ERR_NONE) {
            Log("QSV: SyncOperation failed: %d", static_cast<int>(st));
            return false;
        }
    }
    if (bs.DataLength == 0)
        return false;

    out.codec = 0; /* H.264 */
    out.keyframe = false;
    out.raw.clear();
    out.nals = SplitAnnexB(bs.Data + bs.DataOffset, bs.DataLength);
    out.keyframe = HasIdrSlice(out.nals);
    return !out.nals.empty();
}

bool QsvEncoder::SetBitrate(uint32_t bitrate) {
    if (!session_ || !configured_ || !enc_reset_)
        return false;
    mfxVideoParam par = param_;
    par.mfx.TargetKbps = static_cast<mfxU16>(bitrate / 1000);
    par.mfx.MaxKbps = static_cast<mfxU16>(bitrate * 12 / 10 / 1000);
    par.mfx.BufferSizeInKB =
        static_cast<mfxU16>(std::max<uint32_t>(1, bitrate / fps_ / 1000));
    mfxStatus st = enc_reset_(session_, &par);
    if (st != MFX_ERR_NONE) {
        Log("QSV: MFXVideoENCODE_Reset failed: %d", static_cast<int>(st));
        return false;
    }
    param_ = par;
    bitrate_ = bitrate;
    Log("QSV: bitrate -> %u bps", bitrate);
    return true;
}

bool QsvEncoder::CopyFrameToNv12(ID3D11Texture2D* frame) {
    if (!device_) {
        frame->GetDevice(&device_);
        device_->GetImmediateContext(&ctx_);
        if (!device_ || !ctx_)
            return false;
    }
    if (!staging_) {
        D3D11_TEXTURE2D_DESC d{};
        frame->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging_)))
            return false;
    }
    ctx_->CopyResource(staging_.Get(), frame);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    BgraToNv12(static_cast<const uint8_t*>(map.pData), map.RowPitch,
               nv12_.data(), enc_w_, w_, h_);
    ctx_->Unmap(staging_.Get(), 0);
    return true;
}
}  // namespace twin

#endif /* defined(TWIN_HAVE_QSV) */
