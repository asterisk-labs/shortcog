#pragma once

#include <cstdint>
#include <cstddef>

namespace shortcog {

constexpr uint32_t MAGIC   = 0x474F4353;  // 'SCOG' little endian
constexpr uint16_t VERSION = 1;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint32_t image_width;
    uint32_t image_length;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    uint8_t  predictor;
    uint64_t base_tiles_offset;
    uint32_t tile_count;
    // immediately followed by uint32_t tile_byte_counts[tile_count]
};
#pragma pack(pop)

// Register the shortcog GDAL driver. Idempotent.
void register_driver();

}  // namespace shortcog
