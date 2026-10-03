#include "music_cover_model.h"

#include <algorithm>

namespace rlcd_cover {
bool JpegDimensions(const uint8_t* bytes, size_t size, size_t& width, size_t& height) {
    width = height = 0;
    if (!bytes || size < 4 || size > 64 * 1024 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return false;
    size_t offset = 2;
    while (offset + 4 <= size) {
        if (bytes[offset++] != 0xff)
            return false;
        while (offset < size && bytes[offset] == 0xff)
            ++offset;
        if (offset >= size)
            return false;
        const uint8_t marker = bytes[offset++];
        if (marker == 0xda || marker == 0xd9)
            return false;
        if (size - offset < 2)
            return false;
        const size_t length = (bytes[offset] << 8) | bytes[offset + 1];
        if (length < 2 || length > size - offset)
            return false;
        if (marker >= 0xc0 && marker <= 0xc3) {
            if (marker != 0xc0 || length < 8 || bytes[offset + 2] != 8)
                return false;
            height = (bytes[offset + 3] << 8) | bytes[offset + 4];
            width = (bytes[offset + 5] << 8) | bytes[offset + 6];
            return width > 0 && height > 0 && width <= 512 && height <= 512 &&
                   width * height <= 65536;
        }
        offset += length;
    }
    return false;
}

std::vector<uint8_t> Monochrome(const uint8_t* pixels, size_t size, size_t width, size_t height,
                                size_t stride) {
    if (!pixels || !width || !height || width > 512 || height > 512 || width * height > 65536 ||
        stride < width * 2 || stride > size / height)
        return {};
    constexpr size_t edge = 128;
    constexpr uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const size_t drawn_width = width >= height ? edge : std::max<size_t>(1, edge * width / height);
    const size_t drawn_height = height >= width ? edge : std::max<size_t>(1, edge * height / width);
    const size_t left = (edge - drawn_width) / 2, top = (edge - drawn_height) / 2;
    std::vector<uint8_t> result(edge * edge * 2, 0xff);
    for (size_t y = 0; y < drawn_height; ++y) {
        for (size_t x = 0; x < drawn_width; ++x) {
            const auto* pixel =
                pixels + (y * height / drawn_height) * stride + (x * width / drawn_width) * 2;
            const uint16_t rgb = pixel[0] | (pixel[1] << 8);
            const int red = ((rgb >> 11) & 31) * 255 / 31;
            const int green = ((rgb >> 5) & 63) * 255 / 63;
            const int blue = (rgb & 31) * 255 / 31;
            const int luminance = (red * 77 + green * 150 + blue * 29) >> 8;
            const uint8_t value = luminance > bayer[y % 4][x % 4] * 16 + 7 ? 0xff : 0;
            const size_t target = ((y + top) * edge + x + left) * 2;
            result[target] = result[target + 1] = value;
        }
    }
    return result;
}
}  // namespace rlcd_cover
