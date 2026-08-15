#include "encode/amf.h"

#include <algorithm>
#include <cstring>

#include "encode/color.h"
#include "encode/nal.h"
#include "util/log.h"

#if defined(TWIN_HAVE_AMF)

namespace twin {
namespace {

template <typename T>
bool Resolve(HMODULE dll, const char* name, T& fn) {
    fn = reinterpret_cast<T>(GetProcAddress(dll, name));
    return fn != nullptr;
}

constexpr size_t kSurfacePool = 3;

}  // namespace

bool AmfEncoder::LoadRuntime() {
    HMODULE dll = LoadLibraryA("amfrt64.dll");
    if (!dll) {
        Log("AMF: amfrt64.dll not found");
        return false;
    }
    if (!Resolve(dll, "AMFInit", amf_init_)) {
        Log("AMF: amfrt64.dll does not export AMFInit");
        FreeLibrary(dll);
        return false;
    }
    amf_dll_ = dll;
    Log("AMF: runtime loaded: amfrt64.dll");
    return true;
}

bool AmfEncoder::Start(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) {
    w_ = w;
    h_ = h;
    fps_ = fps ? fps : 30;
    bitrate_ = bitrate;
    keyframe_requested_ = true;
    enc_w_ = (w_ + 15) & ~15u;
    enc_h_ = (h_ + 15) & ~15u;

    if (!device_) {
        Log("AMF: no D3D11 device set (SetD3D11Device)");
        return false;
    }
    if (!LoadRuntime())
        return false;

    AMF_RESULT res = amf_init_(AMF_FULL_VERSION, &factory_);
    if (res != AMF_OK) {
        Log("AMF: AMFInit failed: %d", static_cast<int>(res));
        Stop();
        return false;
    }
    res = factory_->CreateContext(&ctx_);
    if (res != AMF_OK) {
        Log("AMF: CreateContext failed: %d", static_cast<int>(res));
        Stop();
        return false;
    }
    res = ctx_->InitDX11(device_.Get(), amf::AMF_DX11_0);
    if (res != AMF_OK) {
        Log("AMF: InitDX11 failed: %d", static_cast<int>(res));
        Stop();
        return false;
    }
    res = factory_->CreateComponent(ctx_, AMFVideoEncoderVCE_AVC, &encoder_);
    if (res != AMF_OK) {
        Log("AMF: CreateComponent failed: %d", static_cast<int>(res));
        Stop();
        return false;
    }

    /* Rate control + low-latency structure; explicit for determinism. */
    encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE,
                          (amf_int64)AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                          (amf_int64)AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)bitrate_);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE,
                          (amf_int64)bitrate_ * 12 / 10);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                          (amf_int64)AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
                          AMFConstructRate(static_cast<amf_int32>(fps_), 1));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD,
                          (amf_int64)(fps_ * 2)); /* IDR every 2 s */
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,
                          (amf_int64)(fps_ * 2)); /* SPS/PPS with each IDR */
    encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_SCANTYPE,
                          (amf_int64)AMF_VIDEO_ENCODER_SCANTYPE_PROGRESSIVE);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE,
                          (amf_int64)AMF_VIDEO_ENCODER_PROFILE_MAIN);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64)1000);

    res = encoder_->Init(amf::AMF_SURFACE_NV12, (amf_int32)enc_w_, (amf_int32)enc_h_);
    if (res != AMF_OK) {
        Log("AMF: encoder Init failed: %d", static_cast<int>(res));
        Stop();
        return false;
    }

    if (!nv12_.Allocate(static_cast<size_t>(enc_w_) * enc_h_ * 3 / 2)) {
        Stop();
        return false;
    }
    std::memset(nv12_.data(), 0x80, nv12_.size()); /* neutral chroma */

    /* Pool of shared DX11 NV12 textures (VCE requires shared GPU surfaces),
     * each wrapped once in an AMFSurface with the crop set to the real size. */
    for (size_t i = 0; i < kSurfacePool; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = enc_w_;
        d.Height = enc_h_;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = 0;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &tex))) {
            Log("AMF: CreateTexture2D (NV12 shared) failed");
            Stop();
            return false;
        }
        nv12_tex_.push_back(tex);

        amf::AMFSurfacePtr surface;
        res = ctx_->CreateSurfaceFromDX11Native(tex.Get(), &surface, nullptr);
        if (res != AMF_OK) {
            Log("AMF: CreateSurfaceFromDX11Native failed: %d",
                static_cast<int>(res));
            Stop();
            return false;
        }
        surface->SetCrop(0, 0, (amf_int32)w_, (amf_int32)h_);
        surfaces_.push_back(surface);
    }

    Log("AMF: %ux%u @ %u fps, %u bps", w_, h_, fps_, bitrate_);
    return true;
}

void AmfEncoder::Stop() {
    if (encoder_) {
        encoder_->Drain();
        amf::AMFDataPtr data;
        while (encoder_->QueryOutput(&data) == AMF_OK && data)
            data = nullptr;
        encoder_->Terminate();
    }
    surfaces_.clear();
    nv12_tex_.clear();
    encoder_ = nullptr;
    ctx_ = nullptr;
    if (amf_dll_) {
        FreeLibrary(static_cast<HMODULE>(amf_dll_));
        amf_dll_ = nullptr;
    }
    amf_init_ = nullptr;
    factory_ = nullptr;
}

bool AmfEncoder::Encode(ID3D11Texture2D* frame, EncodedFrame& out) {
    if (!encoder_ || !frame)
        return false;
    if (!CopyFrameToNv12(frame))
        return false;

    if (keyframe_requested_) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                              (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
        keyframe_requested_ = false;
    }

    /* Round-robin the surface pool; wait out a full input queue. */
    amf::AMFSurfacePtr& surface =
        surfaces_[surface_index_ % surfaces_.size()];
    surface_index_ = (surface_index_ + 1) % surfaces_.size();
    AMF_RESULT res = AMF_INPUT_FULL;
    for (int tries = 0; tries < 30 && res == AMF_INPUT_FULL; ++tries) {
        res = encoder_->SubmitInput(surface);
        if (res == AMF_INPUT_FULL) {
            EncodedFrame tmp;
            ReadOutput(tmp); /* drain so the encoder can accept input */
            Sleep(1);
        }
    }
    if (res != AMF_OK) {
        Log("AMF: SubmitInput failed: %d", static_cast<int>(res));
        return false;
    }
    return ReadOutput(out);
}

bool AmfEncoder::ReadOutput(EncodedFrame& out) {
    amf::AMFDataPtr data;
    AMF_RESULT res = encoder_->QueryOutput(&data);
    if (res == AMF_REPEAT || res == AMF_EOF)
        return false; /* nothing ready yet; not an error */
    if (res != AMF_OK) {
        Log("AMF: QueryOutput failed: %d", static_cast<int>(res));
        return false;
    }
    if (!data) /* QueryTimeout elapsed, no output yet */
        return false;

    amf::AMFBufferPtr buffer(data); /* QueryInterface to AMFBuffer */
    if (!buffer)
        return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(buffer->GetNative());
    const size_t size = buffer->GetSize();
    if (!bytes || size == 0)
        return false;

    out.codec = 0; /* H.264 */
    out.keyframe = false;
    out.raw.clear();
    out.nals = SplitAnnexB(bytes, size);
    out.keyframe = HasIdrSlice(out.nals);
    return !out.nals.empty();
}

bool AmfEncoder::Resize(uint32_t w, uint32_t h) {
    if (!encoder_)
        return false;
    Stop();
    const uint32_t fps = fps_;
    const uint32_t bitrate = bitrate_;
    return Start(w, h, fps, bitrate);
}

bool AmfEncoder::SetBitrate(uint32_t bitrate) {
    if (!encoder_)
        return false;
    AMF_RESULT res = encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE,
                                           (amf_int64)bitrate);
    if (res == AMF_OK)
        res = encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE,
                                    (amf_int64)bitrate * 12 / 10);
    if (res != AMF_OK) {
        Log("AMF: SetBitrate property failed: %d", static_cast<int>(res));
        return false;
    }
    bitrate_ = bitrate;
    Log("AMF: bitrate -> %u bps", bitrate);
    return true;
}

bool AmfEncoder::CopyFrameToNv12(ID3D11Texture2D* frame) {
    if (!dctx_) {
        frame->GetDevice(&device_);
        device_->GetImmediateContext(&dctx_);
        if (!device_ || !dctx_)
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
    dctx_->CopyResource(staging_.Get(), frame);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(dctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    BgraToNv12(static_cast<const uint8_t*>(map.pData), map.RowPitch,
               nv12_.data(), enc_w_, w_, h_);
    dctx_->Unmap(staging_.Get(), 0);

    /* Upload into the next pool texture (single subresource covers NV12). */
    ID3D11Texture2D* tex = nv12_tex_[surface_index_].Get();
    dctx_->UpdateSubresource(tex, 0, nullptr, nv12_.data(), enc_w_, 0);
    return true;
}

}  // namespace twin

#endif /* defined(TWIN_HAVE_AMF) */
