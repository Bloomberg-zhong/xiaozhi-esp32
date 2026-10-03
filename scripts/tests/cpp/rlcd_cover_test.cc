#include "music_cover_model.h"

#include <cassert>
#include <vector>

int main() {
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0, 0,  4,    0,   0,    0xff, 0xc0,
                            0,    17,   8,    0,    64, 0,    128, 3,    1,    0x11,
                            0,    2,    0x11, 1,    3,  0x11, 1,   0xff, 0xd9};
    size_t width = 0, height = 0;
    assert(rlcd_cover::JpegDimensions(jpeg, sizeof(jpeg), width, height));
    assert(width == 128 && height == 64);
    for (size_t size = 0; size < 27; ++size)
        assert(!rlcd_cover::JpegDimensions(jpeg, size, width, height));
    const uint8_t broken[] = {0xff, 0xd8, 0xff, 0xff, 0xff, 0xff};
    assert(!rlcd_cover::JpegDimensions(broken, sizeof(broken), width, height));
    assert(!rlcd_cover::JpegDimensions(jpeg, 16, width, height));
    auto oversized = std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg));
    oversized[15] = 3;  // 896px wide: reject before allocation
    assert(!rlcd_cover::JpegDimensions(oversized.data(), oversized.size(), width, height));
    oversized = std::vector<uint8_t>(jpeg, jpeg + sizeof(jpeg));
    oversized[9] = 0xc2;  // progressive JPEG not supported by the software decoder
    assert(!rlcd_cover::JpegDimensions(oversized.data(), oversized.size(), width, height));
    std::vector<uint8_t> pixels(8 * 4 * 2, 0xff);
    auto white = rlcd_cover::Monochrome(pixels.data(), pixels.size(), 8, 4, 16);
    assert(white.size() == 128 * 128 * 2);
    for (auto byte : white)
        assert(byte == 0xff);
    std::fill(pixels.begin(), pixels.end(), 0);
    auto black = rlcd_cover::Monochrome(pixels.data(), pixels.size(), 8, 4, 16);
    assert(black[0] == 0xff);  // white letterbox, aspect ratio is preserved
    assert(black[(64 * 128 + 64) * 2] == 0);
    for (size_t i = 0; i < pixels.size(); i += 2) {
        pixels[i] = 0x10;
        pixels[i + 1] = 0x84;
    }
    auto gray = rlcd_cover::Monochrome(pixels.data(), pixels.size(), 8, 4, 16);
    size_t dark = 0;
    for (size_t i = 32 * 128 * 2; i < 96 * 128 * 2; i += 2) {
        assert(gray[i] == gray[i + 1]);
        assert(gray[i] == 0 || gray[i] == 0xff);
        dark += gray[i] == 0;
    }
    assert(dark > 3000 && dark < 5000);
    assert(rlcd_cover::Monochrome(pixels.data(), 1, 8, 4, 16).empty());
    assert(rlcd_cover::Monochrome(pixels.data(), pixels.size(), 0, 4, 16).empty());
}
