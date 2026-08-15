#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace twin {

/* Splits an Annex B byte stream into NAL payloads WITHOUT their start codes.
 * Handles both 3- and 4-byte start codes; trailing bytes after the last NAL
 * are discarded. */
std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size);

/* True if any NAL is an H.264 IDR slice (NAL unit type 5). */
bool HasIdrSlice(const std::vector<std::vector<uint8_t>>& nals);

}  // namespace twin
