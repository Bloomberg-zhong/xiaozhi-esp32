#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rlcd_cover {
// Reject oversized/progressive images before entering the JPEG allocator.
bool JpegDimensions(const uint8_t* bytes, size_t size, size_t& width, size_t& height);
// RGB565 little endian -> 128x128 black/white RGB565 with white letterboxing.
std::vector<uint8_t> Monochrome(const uint8_t* pixels, size_t size, size_t width, size_t height,
                                size_t stride);
}  // namespace rlcd_cover
