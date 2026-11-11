#include "../tier5-vaapi-daemon/codec2-component/InputHandleLayout.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    android::InputFrameLayout result{};
    std::array<uint32_t, 39> handle{};
    // cros width/height/stride must not shift when fd count changes.
    handle[84 / 4] = 720;
    handle[88 / 4] = 1280;
    handle[32 / 4] = 2880;
    for (int fds : {1, 2, 5}) {
        assert(android::readInputFrameLayout(handle.data(), sizeof(handle), fds, true, &result));
        assert(result.width == 720 && result.height == 1280);
        assert(result.stride == 2880 && result.size == 3686400);
    }
    assert(!android::readInputFrameLayout(handle.data(), 88, 1, true, &result));
    assert(!android::readInputFrameLayout(nullptr, sizeof(handle), 1, true, &result));
    assert(!android::readInputFrameLayout(handle.data(), sizeof(handle), 0, true, &result));
    handle[32 / 4] = 4;
    assert(!android::readInputFrameLayout(handle.data(), sizeof(handle), 1, true, &result));
    handle[32 / 4] = std::numeric_limits<uint32_t>::max();
    assert(!android::readInputFrameLayout(handle.data(), sizeof(handle), 1, true, &result));
    // Retain the original GBM offsets, including its padded RGBA stride.
    for (int fds : {1, 2}) {
        handle.fill(0);
        handle[5 + fds] = 720;
        handle[6 + fds] = 1280;
        handle[9 + fds] = 2944;
        assert(android::readInputFrameLayout(handle.data(), sizeof(handle), fds, false, &result));
        assert(result.width == 720 && result.height == 1280 && result.stride == 2944);
    }
}
