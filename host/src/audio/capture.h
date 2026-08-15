#pragma once

/*
 * WASAPI loopback audio capture. Grabs whatever the host is playing on the
 * default render device, normalizes it to a fixed-rate stereo s16 stream, and
 * exposes it as a bounded queue of 1024-sample frames (one AAC encoder input
 * each). Runs its own capture thread; PopFrame() is safe from any thread.
 *
 * Requires a running audio endpoint; if none exists Start() returns false and
 * the host simply runs without audio.
 */

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace twin {

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture() { Stop(); }

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /* Starts the capture thread; blocks until init succeeds or fails. */
    bool Start(uint32_t target_rate = 48000);
    void Stop();

    /* Pops the next 1024-sample stereo frame; false when the queue is empty. */
    bool PopFrame(std::vector<int16_t>& out);

    bool IsRunning() const { return running_.load(); }

    struct CaptureFormat {
        uint32_t rate = 48000;
        WORD channels = 2;
        WORD bits = 16;
        bool is_float = false;
    };

private:
    void CaptureLoop(uint32_t target_rate, std::promise<bool>& ready);

    std::atomic<bool> running_{false};
    std::thread thread_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    HANDLE h_event_ = nullptr;

    CaptureFormat fmt_{};
    uint32_t target_rate_ = 48000;

    std::mutex mutex_;
    std::deque<std::vector<int16_t>> frames_;
    static constexpr size_t kFrameSamples = 1024; /* stereo */
    static constexpr size_t kMaxQueuedFrames = 20;
};

}  // namespace twin
