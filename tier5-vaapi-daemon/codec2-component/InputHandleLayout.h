// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace android {

struct InputFrameLayout {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
};

// Offsets include native_handle_t's three int32_t header words. cros/minigbm
// reserves five fd slots regardless of numFds; the old empirical GBM layout
// places its integer fields directly after the occupied fd slots.
inline bool readInputFrameLayout(const void *handle, size_t bytes, int numFds,
                                 bool cros, InputFrameLayout *out) {
    if (!handle || !out || numFds < 1 || numFds > 1024) return false;
    const size_t widthOffset = cros ? 84 : 20 + size_t(numFds) * 4;
    const size_t heightOffset = cros ? 88 : 24 + size_t(numFds) * 4;
    const size_t strideOffset = cros ? 32 : 36 + size_t(numFds) * 4;
    if (bytes < widthOffset + 4 || bytes < heightOffset + 4 || bytes < strideOffset + 4)
        return false;
    const auto *data = static_cast<const unsigned char *>(handle);
    InputFrameLayout result{};
    std::memcpy(&result.width, data + widthOffset, 4);
    std::memcpy(&result.height, data + heightOffset, 4);
    std::memcpy(&result.stride, data + strideOffset, 4);
    if (result.width < 2 || result.width > 2560 || result.height < 2 || result.height > 2560)
        return false;
    if (result.stride < result.width * 4 ||
        uint64_t(result.stride) * result.height > std::numeric_limits<uint32_t>::max())
        return false;
    result.size = result.stride * result.height;
    *out = result;
    return true;
}

}  // namespace android
