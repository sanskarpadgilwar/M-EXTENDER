#include "audio/aac.h"

#include <windows.h>
#include <wrl/client.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>

#include <cstring>
#include <limits>

#include "util/log.h"

namespace twin {

/* MFSTARTUP_NOSOCK from mfapi.h (undef'd on some SDK revisions). */
#ifndef MFSTARTUP_NOSOCK
#define MFSTARTUP_NOSOCK 0x1
#endif

struct AacEncoder::Impl {
    IMFTransform* mft = nullptr;
    uint32_t rate = 48000;
    uint16_t channels = 2;
    uint32_t block_align = 4;
    LONGLONG sample_time = 0; /* 100ns units */
};

namespace {

using Microsoft::WRL::ComPtr;

uint8_t SamplingFrequencyIndex(uint32_t rate) {
    switch (rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        default: return 3;
    }
}

/* Wraps raw AAC in a 7-byte MPEG-4 AAC-LC ADTS header (no CRC). */
void WriteAdts(uint8_t* hdr, uint32_t frame_len, uint32_t rate, uint16_t channels) {
    const uint8_t profile = 1; /* AAC-LC */
    const uint8_t sf = SamplingFrequencyIndex(rate);
    const uint8_t cc = static_cast<uint8_t>(channels);
    hdr[0] = 0xFF;
    hdr[1] = 0xF1; /* MPEG-4, layer 0, no CRC */
    hdr[2] = static_cast<uint8_t>((profile << 6) | (sf << 2) | (cc >> 2));
    hdr[3] = static_cast<uint8_t>(((cc & 0x03) << 6) | ((frame_len >> 11) & 0x03));
    hdr[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
    hdr[5] = static_cast<uint8_t>(((frame_len & 0x07) << 5) | 0x1F);
    hdr[6] = 0xFC;
}

HRESULT MakePcmType(IMFMediaType** out, uint32_t rate, uint16_t channels) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    *out = t.Detach();
    return S_OK;
}

/* Picks the encoder's supported AAC output type closest to the target rate. */
HRESULT ChooseOutputType(IMFTransform* mft, uint32_t rate, uint16_t channels,
                         uint32_t bitrate_bps, IMFMediaType** out) {
    const UINT32 target_bytes = bitrate_bps / 8;
    int best_diff = std::numeric_limits<int>::max();
    ComPtr<IMFMediaType> best;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
        if (FAILED(hr)) break;
        GUID subtype{};
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        if (subtype != MFAudioFormat_AAC) continue;
        UINT32 r = 0, c = 0, bytes = 0;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
        if (r != rate || c != channels) continue;
        int diff = static_cast<int>(bytes) - static_cast<int>(target_bytes);
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            best = t;
        }
    }
    if (!best) return E_FAIL;
    *out = best.Detach();
    return S_OK;
}

}  // namespace

bool AacEncoder::Init(uint32_t sample_rate, uint16_t channels, uint32_t bitrate_bps) {
    Shutdown();
    if (sample_rate != 44100 && sample_rate != 48000) {
        Log("aac: unsupported rate %u (want 44100/48000)", sample_rate);
        return false;
    }

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCK);
    if (FAILED(hr)) {
        Log("aac: MFStartup failed 0x%08x", hr);
        return false;
    }

    impl_ = new Impl;
    impl_->rate = sample_rate;
    impl_->channels = channels;
    impl_->block_align = channels * 2;

    /* Find an AAC encoder by capability, not CLSID (the documented
     * CLSID_CMSAACEncMFT is not registered on all SDK revisions). */
    const MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Audio, MFAudioFormat_PCM};
    const MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT,
                   &in_info, &out_info, &acts, &count);
    if (FAILED(hr) || count == 0) {
        Log("aac: no AAC encoder MFT found (0x%08x)", hr);
        if (acts) CoTaskMemFree(acts);
        Shutdown();
        return false;
    }
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(&impl_->mft));
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) {
        Log("aac: AAC encoder activation failed 0x%08x", hr);
        Shutdown();
        return false;
    }

    ComPtr<IMFMediaType> in_type;
    hr = MakePcmType(&in_type, sample_rate, channels);
    if (FAILED(hr)) {
        Log("aac: MFCreateMediaType failed 0x%08x", hr);
        Shutdown();
        return false;
    }
    hr = impl_->mft->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        Log("aac: SetInputType failed 0x%08x", hr);
        Shutdown();
        return false;
    }

    ComPtr<IMFMediaType> out_type;
    hr = ChooseOutputType(impl_->mft, sample_rate, channels, bitrate_bps, &out_type);
    if (FAILED(hr)) {
        Log("aac: no suitable AAC output type (0x%08x)", hr);
        Shutdown();
        return false;
    }
    UINT32 chosen_bps = 0;
    out_type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &chosen_bps);
    hr = impl_->mft->SetOutputType(0, out_type.Get(), 0);
    if (FAILED(hr)) {
        Log("aac: SetOutputType failed 0x%08x", hr);
        Shutdown();
        return false;
    }

    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    Log("aac: %u Hz %u ch -> %u bps", sample_rate, channels, chosen_bps * 8);
    return true;
}

void AacEncoder::Shutdown() {
    if (!impl_) return;
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->Release();
    }
    delete impl_;
    impl_ = nullptr;
    MFShutdown();
}

bool AacEncoder::Encode(const int16_t* pcm, size_t samples, std::vector<uint8_t>& out) {
    if (!impl_ || !impl_->mft || !pcm) return false;
    const size_t start = out.size();
    const DWORD bytes = static_cast<DWORD>(samples) * impl_->block_align;

    ComPtr<IMFSample> input;
    if (FAILED(MFCreateSample(&input))) return false;
    ComPtr<IMFMediaBuffer> ibuf;
    if (FAILED(MFCreateMemoryBuffer(bytes, &ibuf))) return false;
    BYTE* data = nullptr;
    if (FAILED(ibuf->Lock(&data, nullptr, nullptr))) return false;
    std::memcpy(data, pcm, bytes);
    ibuf->Unlock();
    ibuf->SetCurrentLength(bytes);
    input->AddBuffer(ibuf.Get());
    const LONGLONG duration =
        static_cast<LONGLONG>(samples) * 10000000LL / impl_->rate;
    input->SetSampleDuration(duration);
    input->SetSampleTime(impl_->sample_time);

    HRESULT hr = impl_->mft->ProcessInput(0, input.Get(), 0);
    if (FAILED(hr)) {
        Log("aac: ProcessInput failed 0x%08x", hr);
        return false;
    }
    impl_->sample_time += duration;

    MFT_OUTPUT_STREAM_INFO info{};
    impl_->mft->GetOutputStreamInfo(0, &info);
    const DWORD buf_size = info.cbSize ? info.cbSize : 4096;

    while (true) {
        ComPtr<IMFMediaBuffer> obuf;
        if (FAILED(MFCreateMemoryBuffer(buf_size, &obuf))) return false;
        ComPtr<IMFSample> osample;
        if (FAILED(MFCreateSample(&osample))) return false;
        osample->AddBuffer(obuf.Get());

        MFT_OUTPUT_DATA_BUFFER od{};
        od.pSample = osample.Get();
        DWORD status = 0;
        hr = impl_->mft->ProcessOutput(0, 1, &od, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) {
            Log("aac: ProcessOutput failed 0x%08x", hr);
            return false;
        }

        BYTE* obp = nullptr;
        DWORD length = 0;
        if (FAILED(obuf->Lock(&obp, nullptr, &length))) return false;
        const UINT32 frame_len = length + 7;
        out.resize(out.size() + frame_len);
        WriteAdts(out.data() + out.size() - frame_len, frame_len,
                  impl_->rate, impl_->channels);
        std::memcpy(out.data() + out.size() - length, obp, length);
        obuf->Unlock();
    }

    return out.size() > start;
}

}  // namespace twin
