#include "audio/capture.h"

#include <mfapi.h>

#include <cstring>

#include "util/log.h"

namespace twin {

namespace {

constexpr REFERENCE_TIME kDefaultPeriod = 0; /* let the audio engine decide */

inline int16_t FloatToS16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}

inline int16_t LerpS16(int16_t a, int16_t b, double f) {
    double v = a + (b - a) * f;
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    return static_cast<int16_t>(v);
}

/* Converts one WASAPI buffer of `frames` input samples to stereo s16. */
void ConvertToStereo(const BYTE* src, size_t frames, const AudioCapture::CaptureFormat& fmt,
                     int16_t* out) {
    if (fmt.is_float) {
        const float* p = reinterpret_cast<const float*>(src);
        for (size_t i = 0; i < frames; ++i) {
            float l = p[i * fmt.channels];
            float r = (fmt.channels > 1) ? p[i * fmt.channels + 1] : l;
            out[i * 2] = FloatToS16(l);
            out[i * 2 + 1] = FloatToS16(r);
        }
        return;
    }
    if (fmt.bits == 16) {
        const int16_t* p = reinterpret_cast<const int16_t*>(src);
        for (size_t i = 0; i < frames; ++i) {
            int16_t l = p[i * fmt.channels];
            int16_t r = (fmt.channels > 1) ? p[i * fmt.channels + 1] : l;
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        return;
    }
    if (fmt.bits == 32) {
        const int32_t* p = reinterpret_cast<const int32_t*>(src);
        for (size_t i = 0; i < frames; ++i) {
            int32_t l = p[i * fmt.channels] >> 16;
            int32_t r = (fmt.channels > 1) ? p[i * fmt.channels + 1] >> 16 : l;
            out[i * 2] = static_cast<int16_t>(l);
            out[i * 2 + 1] = static_cast<int16_t>(r);
        }
        return;
    }
    if (fmt.bits == 24) {
        for (size_t i = 0; i < frames; ++i) {
            const BYTE* l = src + (i * fmt.channels) * 3;
            int32_t lv = (l[0] | (l[1] << 8) | (l[2] << 16));
            if (l[2] & 0x80) lv |= 0xFF000000; /* sign extend */
            int32_t rv = lv;
            if (fmt.channels > 1) {
                const BYTE* r = l + 3;
                rv = (r[0] | (r[1] << 8) | (r[2] << 16));
                if (r[2] & 0x80) rv |= 0xFF000000;
            }
            out[i * 2] = static_cast<int16_t>(lv >> 8);
            out[i * 2 + 1] = static_cast<int16_t>(rv >> 8);
        }
        return;
    }
    /* Unsupported depth: silence. */
    std::memset(out, 0, frames * 2 * sizeof(int16_t));
}

/* Linear stereo resampler. Keeps phase_ across calls for packet continuity. */
class StereoResampler {
public:
    explicit StereoResampler(double ratio) : ratio_(ratio) {}

    size_t Process(const int16_t* in, size_t frames, int16_t* out, size_t out_cap) {
        size_t written = 0;
        while (phase_ < static_cast<double>(frames) && written < out_cap) {
            size_t i0 = static_cast<size_t>(phase_);
            size_t i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
            double frac = phase_ - static_cast<double>(i0);
            out[written * 2] = LerpS16(in[i0 * 2], in[i1 * 2], frac);
            out[written * 2 + 1] = LerpS16(in[i0 * 2 + 1], in[i1 * 2 + 1], frac);
            ++written;
            phase_ += ratio_;
        }
        phase_ -= static_cast<double>(frames);
        return written;
    }

private:
    double ratio_;
    double phase_ = 0.0;
};

bool GetMixFormat(IAudioClient* client, AudioCapture::CaptureFormat& fmt,
                  WAVEFORMATEX** out_mix) {
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return false;
    fmt.rate = mix->nSamplesPerSec;
    fmt.channels = mix->nChannels;
    fmt.bits = mix->wBitsPerSample;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        fmt.is_float = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        fmt.is_float = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    }
    *out_mix = mix; /* caller CoTaskMemFree after Initialize */
    return true;
}

}  // namespace

bool AudioCapture::Start(uint32_t target_rate) {
    if (running_.load()) return true;
    target_rate_ = target_rate;

    std::promise<bool> ready;
    auto fut = ready.get_future();
    thread_ = std::thread(&AudioCapture::CaptureLoop, this, target_rate, std::ref(ready));
    const bool ok = fut.get();
    if (!ok) {
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void AudioCapture::Stop() {
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    running_.store(false);
    if (h_event_) SetEvent(h_event_); /* wake the capture wait */
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    client_.Reset();
    capture_.Reset();
    if (h_event_) {
        CloseHandle(h_event_);
        h_event_ = nullptr;
    }
}

bool AudioCapture::PopFrame(std::vector<int16_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) return false;
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void AudioCapture::CaptureLoop(uint32_t target_rate, std::promise<bool>& ready) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log("audio: CoInitializeEx failed 0x%08x", hr);
        ready.set_value(false);
        return;
    }

    auto finish = [&] {
        if (client_) client_->Stop();
        client_.Reset();
        capture_.Reset();
        if (h_event_) {
            CloseHandle(h_event_);
            h_event_ = nullptr;
        }
        CoUninitialize();
        ready.set_value(false);
    };

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        Log("audio: MMDeviceEnumerator failed 0x%08x", hr);
        finish();
        return;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        Log("audio: no render endpoint (0x%08x)", hr);
        finish();
        return;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) {
        Log("audio: Activate IAudioClient failed 0x%08x", hr);
        finish();
        return;
    }

    WAVEFORMATEX* mix = nullptr;
    if (!GetMixFormat(client_.Get(), fmt_, &mix)) {
        Log("audio: GetMixFormat failed");
        finish();
        return;
    }

    h_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!h_event_) {
        CoTaskMemFree(mix);
        Log("audio: CreateEvent failed");
        finish();
        return;
    }

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             kDefaultPeriod, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        Log("audio: Initialize(loopback) failed 0x%08x", hr);
        finish();
        return;
    }

    hr = client_->SetEventHandle(h_event_);
    if (FAILED(hr)) {
        Log("audio: SetEventHandle failed 0x%08x", hr);
        finish();
        return;
    }

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        Log("audio: GetService IAudioCaptureClient failed 0x%08x", hr);
        finish();
        return;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        Log("audio: IAudioClient::Start failed 0x%08x", hr);
        finish();
        return;
    }

    Log("audio: loopback %u Hz %u ch %s -> %u Hz s16 stereo",
        fmt_.rate, fmt_.channels, fmt_.is_float ? "float" : "int",
        target_rate);
    running_.store(true);
    ready.set_value(true);

    const double ratio = static_cast<double>(target_rate) / fmt_.rate;
    StereoResampler resampler(ratio);
    std::vector<int16_t> acc;
    acc.reserve(kFrameSamples * 2 * 2);

    while (running_.load()) {
        if (WaitForSingleObject(h_event_, 100) != WAIT_OBJECT_0) continue;

        /* Drain every pending packet. */
        UINT32 packets = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packets)) && packets > 0) {
            BYTE* data = nullptr;
            UINT32 frame_count = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frame_count, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                    Log("audio: device invalidated; stopping capture");
                capture_->ReleaseBuffer(frame_count);
                running_.store(false);
                break;
            }
            if (frame_count > 0) {
                std::vector<int16_t> tmp(frame_count * 2);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                    std::memset(tmp.data(), 0, tmp.size() * sizeof(int16_t));
                } else {
                    ConvertToStereo(data, frame_count, fmt_, tmp.data());
                }
                capture_->ReleaseBuffer(frame_count);

                if (fmt_.rate == target_rate) {
                    acc.insert(acc.end(), tmp.begin(), tmp.end());
                } else {
                    std::vector<int16_t> rtmp(
                        static_cast<size_t>(frame_count * ratio) + 8);
                    const size_t n = resampler.Process(tmp.data(), frame_count,
                                                       rtmp.data(), rtmp.size());
                    acc.insert(acc.end(), rtmp.begin(), rtmp.begin() + n);
                }
            } else {
                capture_->ReleaseBuffer(frame_count);
            }
        }

        /* Emit full 1024-sample frames. */
        const size_t per_frame = kFrameSamples * 2;
        while (acc.size() >= per_frame) {
            std::vector<int16_t> frame(acc.begin(), acc.begin() + per_frame);
            acc.erase(acc.begin(), acc.begin() + per_frame);
            std::lock_guard<std::mutex> lock(mutex_);
            if (frames_.size() >= kMaxQueuedFrames) frames_.pop_front();
            frames_.push_back(std::move(frame));
        }
    }

    if (client_) client_->Stop();
    client_.Reset();
    capture_.Reset();
    if (h_event_) {
        CloseHandle(h_event_);
        h_event_ = nullptr;
    }
    CoUninitialize();
}

}  // namespace twin
