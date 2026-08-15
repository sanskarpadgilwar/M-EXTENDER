#pragma once

#include <cstdint>

namespace twin {

/* Converts a BGRA (byte order B,G,R,A) frame into an NV12 plane. BT.601
 * limited-range luma/ chroma coefficients; U/V are 2x2 box-sampled. Writes
 * enc_w*enc_h Y bytes followed by enc_w*enc_h/2 interleaved UV bytes, with row
 * pitch `nv12_pitch` for the Y plane. */
void BgraToNv12(const uint8_t* bgra, uint32_t bgra_pitch, uint8_t* nv12,
                uint32_t nv12_pitch, uint32_t w, uint32_t h);

}  // namespace twin
