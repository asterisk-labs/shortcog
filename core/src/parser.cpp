#include "shortcog/shortcog.hpp"

#include <cstring>
#include <limits>

namespace shortcog {

std::string_view describe(ParseError e) noexcept
{
    switch (e) {
        case ParseError::blob_too_short:               return "blob shorter than header";
        case ParseError::bad_magic:                    return "bad magic";
        case ParseError::unsupported_version:          return "unsupported version";
        case ParseError::invalid_bits_per_sample:      return "invalid bits per sample";
        case ParseError::invalid_sample_format:        return "invalid sample format";
        case ParseError::invalid_predictor:            return "invalid predictor";
        case ParseError::invalid_dimensions:           return "invalid dimensions";
        case ParseError::tile_larger_than_image:       return "tile dimensions exceed image";
        case ParseError::blob_size_mismatch:           return "blob size does not match tile count";
        case ParseError::tile_count_overflow:          return "tile count overflows uint32";
        case ParseError::tile_size_overflow:           return "tile byte size overflows size_t";
        case ParseError::non_positive_tile_byte_count: return "tile byte count is zero";
        case ParseError::offset_overflow:              return "reconstructed offsets overflow";
    }
    return "unknown parse error";
}

// Float16 / CFloat16 require GDAL >= 3.11 (RFC 100).
GDALDataType infer_gdal_type(std::uint8_t bits_per_sample,
                             std::uint8_t sample_format) noexcept
{
    switch (sample_format) {
        case 1:
            switch (bits_per_sample) {
                case 8:  return GDT_Byte;
                case 16: return GDT_UInt16;
                case 32: return GDT_UInt32;
                case 64: return GDT_UInt64;
            }
            break;
        case 2:
            switch (bits_per_sample) {
                case 8:  return GDT_Int8;
                case 16: return GDT_Int16;
                case 32: return GDT_Int32;
                case 64: return GDT_Int64;
            }
            break;
        case 3:
            switch (bits_per_sample) {
                case 16: return GDT_Float16;
                case 32: return GDT_Float32;
                case 64: return GDT_Float64;
            }
            break;
        case 5:  // bits_per_sample = real + imag widths
            switch (bits_per_sample) {
                case 32: return GDT_CInt16;
                case 64: return GDT_CInt32;
            }
            break;
        case 6:  // bits_per_sample = real + imag widths
            switch (bits_per_sample) {
                case 32:  return GDT_CFloat16;
                case 64:  return GDT_CFloat32;
                case 128: return GDT_CFloat64;
            }
            break;
    }
    return GDT_Unknown;
}

std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob)
{
    if (blob.size() < HEADER_SIZE) {
        return std::unexpected(ParseError::blob_too_short);
    }

    BlobHeader bh;
    std::memcpy(&bh, blob.data(), sizeof(BlobHeader));

    if (bh.magic   != MAGIC)   return std::unexpected(ParseError::bad_magic);
    if (bh.version != VERSION) return std::unexpected(ParseError::unsupported_version);

    if (bh.bits_per_sample != 8  && bh.bits_per_sample != 16 &&
        bh.bits_per_sample != 32 && bh.bits_per_sample != 64 &&
        bh.bits_per_sample != 128) {
        return std::unexpected(ParseError::invalid_bits_per_sample);
    }
    if (bh.sample_format != 1 && bh.sample_format != 2 &&
        bh.sample_format != 3 && bh.sample_format != 5 &&
        bh.sample_format != 6) {
        return std::unexpected(ParseError::invalid_sample_format);
    }
    if (bh.predictor != 1 && bh.predictor != 2) {
        return std::unexpected(ParseError::invalid_predictor);
    }
    // Horizontal differencing across real/imag components is undefined.
    if (bh.predictor == 2 && (bh.sample_format == 5 || bh.sample_format == 6)) {
        return std::unexpected(ParseError::invalid_predictor);
    }

    const GDALDataType gdt = infer_gdal_type(bh.bits_per_sample, bh.sample_format);
    if (gdt == GDT_Unknown) {
        return std::unexpected(ParseError::invalid_sample_format);
    }

    if (bh.image_width == 0 || bh.image_length == 0 ||
        bh.tile_width  == 0 || bh.tile_length  == 0 ||
        bh.samples_per_pixel == 0) {
        return std::unexpected(ParseError::invalid_dimensions);
    }
    // image_width and image_length are uint32 in the blob but become int raster
    // sizes in GDAL. A value too big for int would cast negative, so reject it.
    if (bh.image_width  > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        bh.image_length > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(ParseError::invalid_dimensions);
    }
    if (bh.tile_width > bh.image_width || bh.tile_length > bh.image_length) {
        return std::unexpected(ParseError::tile_larger_than_image);
    }

    Header h;
    h.image_width       = bh.image_width;
    h.image_length      = bh.image_length;
    h.tile_width        = bh.tile_width;
    h.tile_length       = bh.tile_length;
    h.samples_per_pixel = bh.samples_per_pixel;
    h.bits_per_sample   = bh.bits_per_sample;
    h.sample_format     = bh.sample_format;
    h.predictor         = bh.predictor;
    h.base_tiles_offset = bh.base_tiles_offset;
    h.gdal_type         = gdt;
    h.bytes_per_sample  = bh.bits_per_sample / 8u;

    h.tiles_across = (bh.image_width  + bh.tile_width  - 1) / bh.tile_width;
    h.tiles_down   = (bh.image_length + bh.tile_length - 1) / bh.tile_length;

    const auto tile_count_u64 = static_cast<std::uint64_t>(h.tiles_across)
                              * static_cast<std::uint64_t>(h.tiles_down)
                              * static_cast<std::uint64_t>(bh.samples_per_pixel);
    if (tile_count_u64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(ParseError::tile_count_overflow);
    }
    h.tile_count = static_cast<std::uint32_t>(tile_count_u64);

    const auto tile_bytes_u64 = static_cast<std::uint64_t>(bh.tile_width)
                              * static_cast<std::uint64_t>(bh.tile_length)
                              * static_cast<std::uint64_t>(h.bytes_per_sample);
    if (tile_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(ParseError::tile_size_overflow);
    }
    h.max_tile_size = static_cast<std::size_t>(tile_bytes_u64);

    const std::size_t expected_size =
        HEADER_SIZE + static_cast<std::size_t>(h.tile_count) * sizeof(std::uint32_t);
    if (blob.size() != expected_size) {
        return std::unexpected(ParseError::blob_size_mismatch);
    }

    h.tile_byte_counts.resize(h.tile_count);
    std::memcpy(h.tile_byte_counts.data(),
                blob.data() + HEADER_SIZE,
                static_cast<std::size_t>(h.tile_count) * sizeof(std::uint32_t));

    // Prefix sum. With every byte count >= 1, the only failure mode left is
    // u64 wraparound on the running offset.
    h.tile_offsets.resize(h.tile_count);
    std::uint64_t offset = bh.base_tiles_offset;
    for (std::uint32_t i = 0; i < h.tile_count; ++i) {
        if (h.tile_byte_counts[i] == 0) {
            return std::unexpected(ParseError::non_positive_tile_byte_count);
        }
        h.tile_offsets[i] = offset;

        const std::uint64_t step =
            static_cast<std::uint64_t>(h.tile_byte_counts[i]) + COG_TILE_FRAMING;
        if (offset > std::numeric_limits<std::uint64_t>::max() - step) {
            return std::unexpected(ParseError::offset_overflow);
        }
        offset += step;
    }

    return h;
}

}