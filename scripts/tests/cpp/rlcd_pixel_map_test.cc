// Checks that the arithmetic RLCD pixel mapping matches the lookup tables it
// replaced (copied from the original InitLandscapeLUT / InitPortraitLUT).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "rlcd_pixel_map.h"

namespace {

struct Lut {
    std::vector<uint32_t> index;
    std::vector<uint8_t> mask;
    int width;
    int height;
};

Lut BuildPortrait(int width, int height) {
    Lut lut{std::vector<uint32_t>(width * height), std::vector<uint8_t>(width * height), width,
            height};
    uint16_t W4 = width >> 2;
    for (uint16_t y = 0; y < height; y++) {
        uint16_t byte_y = y >> 1;
        uint8_t local_y = y & 1;
        for (uint16_t x = 0; x < width; x++) {
            uint16_t byte_x = x >> 2;
            uint8_t local_x = x & 3;
            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);
            lut.index[y * width + x] = index;
            lut.mask[y * width + x] = (1 << bit);
        }
    }
    return lut;
}

Lut BuildLandscape(int width, int height) {
    Lut lut{std::vector<uint32_t>(width * height), std::vector<uint8_t>(width * height), width,
            height};
    uint16_t H4 = height >> 2;
    for (uint16_t y = 0; y < height; y++) {
        uint16_t inv_y = height - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t local_y = inv_y & 3;
        for (uint16_t x = 0; x < width; x++) {
            uint16_t byte_x = x >> 1;
            uint8_t local_x = x & 1;
            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);
            lut.index[y * width + x] = index;
            lut.mask[y * width + x] = (1 << bit);
        }
    }
    return lut;
}

int Compare(const char* name, bool landscape, const Lut& lut) {
    int mismatches = 0;
    std::vector<uint8_t> covered((lut.width * lut.height) / 8, 0);
    for (int y = 0; y < lut.height; ++y) {
        for (int x = 0; x < lut.width; ++x) {
            uint32_t index;
            uint8_t mask;
            RlcdPixelPosition(landscape, lut.width, lut.height, x, y, index, mask);
            if (index != lut.index[y * lut.width + x] || mask != lut.mask[y * lut.width + x]) {
                ++mismatches;
            }
            if (index < covered.size()) {
                covered[index] |= mask;
            } else {
                ++mismatches;
            }
        }
    }
    // Every bit of the frame buffer must be reached exactly once.
    for (uint8_t byte : covered) {
        if (byte != 0xFF) {
            ++mismatches;
            break;
        }
    }
    std::printf("%s: %d mismatches\n", name, mismatches);
    return mismatches;
}

}  // namespace

int main() {
    int failures = 0;
    failures += Compare("landscape 400x300", true, BuildLandscape(400, 300));
    failures += Compare("portrait 300x400", false, BuildPortrait(300, 400));
    return failures == 0 ? 0 : 1;
}
