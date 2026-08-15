#include "encode/nal.h"

namespace twin {

std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> nals;
    size_t i = 0;
    size_t payload_begin = 0;
    bool have_nal = false;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            size_t prev_end =
                (i > 0 && data[i - 1] == 0) ? i - 1 : i; /* 4-byte start code */
            if (have_nal)
                nals.emplace_back(data + payload_begin, data + prev_end);
            payload_begin = i + 3;
            have_nal = true;
            i += 3;
        } else {
            ++i;
        }
    }
    if (have_nal)
        nals.emplace_back(data + payload_begin, data + size);
    return nals;
}

bool HasIdrSlice(const std::vector<std::vector<uint8_t>>& nals) {
    for (const auto& nal : nals) {
        if (!nal.empty() && (nal[0] & 0x1F) == 5)
            return true;
    }
    return false;
}

}  // namespace twin
