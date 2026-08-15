#include "encode/color.h"

namespace twin {
namespace {

inline uint8_t ClipByte(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

void BgraToNv12(const uint8_t* bgra, uint32_t bgra_pitch, uint8_t* nv12,
                uint32_t nv12_pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = bgra + static_cast<size_t>(y) * bgra_pitch;
        uint8_t* dst_y = nv12 + static_cast<size_t>(y) * nv12_pitch;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t b = src[4 * x + 0];
            const uint8_t g = src[4 * x + 1];
            const uint8_t r = src[4 * x + 2];
            dst_y[x] = ClipByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    const uint32_t h2 = h / 2;
    uint8_t* dst_uv = nv12 + static_cast<size_t>(h) * nv12_pitch;
    for (uint32_t y = 0; y < h2; ++y) {
        const uint8_t* r0 = bgra + static_cast<size_t>(2 * y) * bgra_pitch;
        const uint8_t* r1 = r0 + bgra_pitch;
        uint8_t* uv = dst_uv + static_cast<size_t>(y) * nv12_pitch;
        for (uint32_t x = 0; x < w; x += 2) {
            const uint8_t* p00 = r0 + 4 * x;
            const uint8_t* p01 = p00 + 4;
            const uint8_t* p10 = r1 + 4 * x;
            const uint8_t* p11 = p10 + 4;
            const int rs = p00[2] + p01[2] + p10[2] + p11[2];
            const int gs = p00[1] + p01[1] + p10[1] + p11[1];
            const int bs = p00[0] + p01[0] + p10[0] + p11[0];
            uv[2 * (x >> 1) + 0] =
                ClipByte(128 + ((112 * bs - 74 * gs - 38 * rs + 512) >> 10));
            uv[2 * (x >> 1) + 1] =
                ClipByte(128 + ((112 * rs - 94 * gs - 18 * bs + 512) >> 10));
        }
    }
}

}  // namespace twin
