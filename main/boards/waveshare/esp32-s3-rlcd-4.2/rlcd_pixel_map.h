#ifndef RLCD_PIXEL_MAP_H_
#define RLCD_PIXEL_MAP_H_

#include <cstdint>

// Maps a pixel to its byte and bit in the ST7305 frame buffer, where every byte
// holds a 2x4 (landscape) or 4x2 (portrait) block of pixels. It is plain
// arithmetic on purpose: the lookup tables it replaces lived in PSRAM and every
// pixel of a flush needed two cache-missing reads.
//
// Landscape (width 400, height 300): columns are grouped in pairs and the image
// is flipped vertically. Portrait: rows are grouped in pairs and columns in
// fours. `mask` is the single bit to set or clear in the returned byte.
inline void RlcdPixelPosition(bool landscape, int width, int height, int x, int y, uint32_t& index,
                              uint8_t& mask) {
    if (landscape) {
        const int inverted_y = height - 1 - y;
        const int block_y = inverted_y >> 2;
        const int local_y = inverted_y & 3;
        const int local_x = x & 1;
        index = static_cast<uint32_t>((x >> 1) * (height >> 2) + block_y);
        mask = static_cast<uint8_t>(1u << (7 - ((local_y << 1) | local_x)));
    } else {
        const int local_y = y & 1;
        const int local_x = x & 3;
        index = static_cast<uint32_t>((y >> 1) * (width >> 2) + (x >> 2));
        mask = static_cast<uint8_t>(1u << (7 - ((local_x << 1) | local_y)));
    }
}

#endif  // RLCD_PIXEL_MAP_H_
