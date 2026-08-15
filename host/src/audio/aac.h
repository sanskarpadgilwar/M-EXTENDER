#pragma once

/*
 * Media Foundation AAC-LC encoder (CLSID_CMSAACEncMFT). Takes 1024-sample
 * stereo s16 frames and produces self-contained AAC ADTS frames (codec=1 on
 * the wire). Falls back to the caller being unable to send audio if the MFT
 * is missing; no third-party encoder is needed.
 */

#include <cstdint>
#include <vector>

namespace twin {

class AacEncoder {
public:
    AacEncoder() = default;
    ~AacEncoder() { Shutdown(); }

    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    /* Initializes the encoder for a fixed sample rate/channels/bitrate. */
    bool Init(uint32_t sample_rate, uint16_t channels, uint32_t bitrate_bps);
    void Shutdown();

    /* Encodes one frame (1024 stereo s16 samples) and appends the resulting
     * ADTS frame(s) to [out]. Returns false on encoder failure. */
    bool Encode(const int16_t* pcm, size_t samples, std::vector<uint8_t>& out);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace twin
