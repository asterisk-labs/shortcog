// SPDX-License-Identifier: MIT
// shortcog - fast read path for the shortcog COG profile.

#pragma once

#include "gdal_priv.h"
#include "gdal_pam.h"
#include "cpl_vsi.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace shortcog {

// 'SCOG' in little-endian ASCII.
inline constexpr std::uint32_t MAGIC       = 0x474F4353;
inline constexpr std::uint16_t VERSION     = 1;
inline constexpr std::size_t   HEADER_SIZE = 31;

// GDAL's COG driver brackets every tile payload with a 4-byte leader and
// 4-byte trailer; advance offsets by tile_byte_counts[i] + this.
inline constexpr std::uint64_t COG_TILE_FRAMING = 8;

// On-disk layout of the first HEADER_SIZE bytes. Little-endian, no padding.
// Field semantics live in docs/SPEC.md.
#pragma pack(push, 1)
struct BlobHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint32_t image_width;
    std::uint32_t image_length;
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint16_t samples_per_pixel;
    std::uint8_t  bits_per_sample;
    std::uint8_t  sample_format;
    std::uint8_t  predictor;
    std::uint64_t base_tiles_offset;
};
#pragma pack(pop)

static_assert(sizeof(BlobHeader) == HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<BlobHeader>);

enum class ParseError {
    blob_too_short,
    bad_magic,
    unsupported_version,
    invalid_bits_per_sample,
    invalid_sample_format,
    invalid_predictor,
    invalid_dimensions,
    tile_larger_than_image,
    blob_size_mismatch,
    tile_count_overflow,
    tile_size_overflow,
    non_positive_tile_byte_count,
    non_monotonic_offsets,
};

[[nodiscard]] std::string_view describe(ParseError e) noexcept;

// Validated blob. BlobHeader fields, derived grid quantities, the trailing
// tile_byte_counts array, and the precomputed tile_offsets so tile_offset(i)
// stays O(1) on the hot path.
struct Header {
    std::uint32_t image_width{};
    std::uint32_t image_length{};
    std::uint16_t tile_width{};
    std::uint16_t tile_length{};
    std::uint16_t samples_per_pixel{};
    std::uint8_t  bits_per_sample{};
    std::uint8_t  sample_format{};
    std::uint8_t  predictor{};
    std::uint64_t base_tiles_offset{};

    std::uint32_t tiles_across{};
    std::uint32_t tiles_down{};
    std::uint32_t tile_count{};
    std::size_t   bytes_per_sample{};
    std::size_t   max_tile_size{};
    GDALDataType  gdal_type{GDT_Unknown};

    std::vector<std::uint32_t> tile_byte_counts;  // length == tile_count
    std::vector<std::uint64_t> tile_offsets;      // length == tile_count, prefix sums

    [[nodiscard]] std::uint64_t tile_offset(std::uint32_t i) const noexcept {
        return tile_offsets[i];
    }

    // Physical file order, bands inner-most, matching the GDAL COG driver
    // INTERLEAVE=TILE layout. See SPEC.md "Tile byte counts".
    [[nodiscard]] std::uint32_t tile_index(std::uint32_t row,
                                           std::uint32_t col,
                                           std::uint32_t band) const noexcept {
        return (row * tiles_across + col) * samples_per_pixel + band;
    }
};

// Blob = HEADER_SIZE bytes followed by tile_count little-endian uint32 byte
// counts. Returns the first validation failure encountered.
[[nodiscard]] std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob) noexcept;

// GDT_Unknown for any combination outside the shortcog profile.
[[nodiscard]] GDALDataType infer_gdal_type(std::uint8_t bits_per_sample,
                                           std::uint8_t sample_format) noexcept;

// File I/O is serialized on io_mutex_; decompression runs outside the lock,
// so worker threads can decompress in parallel while reads stay serial.
class TileReader {
public:
    TileReader(std::shared_ptr<VSILFILE> file, const Header& header) noexcept;
    ~TileReader();

    TileReader(const TileReader&)            = delete;
    TileReader& operator=(const TileReader&) = delete;
    TileReader(TileReader&&)                 = delete;
    TileReader& operator=(TileReader&&)      = delete;

    // Preconditions.
    //   out.size() == header.max_tile_size.
    //   compressed_scratch is caller-owned, reused across calls on the same
    //   thread, and grows as needed.
    // On failure, reports via CPLError and returns false.
    [[nodiscard]] bool read_tile(std::uint32_t tile_idx,
                                 std::span<std::byte> out,
                                 std::vector<std::byte>& compressed_scratch) const;

    [[nodiscard]] std::shared_ptr<VSILFILE> file() const noexcept { return file_; }

private:
    std::shared_ptr<VSILFILE> file_;
    const Header&             header_;
    mutable std::mutex        io_mutex_;

    void apply_horizontal_predictor(std::span<std::byte> tile) const noexcept;
};

// Defined in the .cpp so this header carries no specific pool dependency.
class ThreadPool;

class Dataset;

// One Band per sample (samples_per_pixel bands total).
class Band : public GDALPamRasterBand {
public:
    Band(Dataset* dataset, int band_index) noexcept;
    ~Band() override = default;

    Band(const Band&)            = delete;
    Band& operator=(const Band&) = delete;

    CPLErr IReadBlock(int x_block, int y_block, void* image) override;

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     GSpacing pixel_space, GSpacing line_space,
                     GDALRasterIOExtraArg* extra_arg) override;

private:
    Dataset* dataset_{nullptr};
};

// Serves tiles from a shortcog-compliant COG using the header blob,
// bypassing the IFD entirely.
class Dataset : public GDALPamDataset {
    friend class Band;

public:
    // Required. Base64-encoded header blob.
    static constexpr std::string_view OPEN_OPTION_HEADER  = "SHORTCOG_HEADER";
    // Integer or "ALL_CPUS". Defaults to 1.
    static constexpr std::string_view OPEN_OPTION_THREADS = "NUM_THREADS";

    Dataset();
    ~Dataset() override;

    Dataset(const Dataset&)            = delete;
    Dataset& operator=(const Dataset&) = delete;

    static GDALDataset* Open(GDALOpenInfo* open_info);
    static int          Identify(GDALOpenInfo* open_info);

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     int band_count, int* band_map,
                     GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                     GDALRasterIOExtraArg* extra_arg) override;

    [[nodiscard]] const Header& header() const noexcept { return header_; }

private:
    Header                      header_{};
    std::unique_ptr<TileReader> tile_reader_;
    std::shared_ptr<int>        validity_token_{std::make_shared<int>(0)};
    std::unique_ptr<ThreadPool> thread_pool_;

    CPLErr read_single_pixel(int x_off, int y_off,
                             int band_count, const int* band_map,
                             void* data, GDALDataType buf_type,
                             GSpacing pixel_space, GSpacing band_space);

    CPLErr read_tile_aligned(int x_off, int y_off, int x_size, int y_size,
                             void* data, int buf_x_size, int buf_y_size,
                             GDALDataType buf_type,
                             int band_count, const int* band_map,
                             GSpacing pixel_space, GSpacing line_space, GSpacing band_space);

    CPLErr read_resampled(int x_off, int y_off, int x_size, int y_size,
                          void* data, int buf_x_size, int buf_y_size,
                          GDALDataType buf_type,
                          int band_count, const int* band_map,
                          GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                          GDALRasterIOExtraArg* extra_arg);

    [[nodiscard]] bool is_tile_aligned_request(int x_off, int y_off,
                                               int x_size, int y_size,
                                               int buf_x_size, int buf_y_size) const noexcept;
};

// Idempotent.
void register_driver();

}  // namespace shortcog

extern "C" {
    // GDAL plugin discovery entry point.
    void CPL_DLL GDALRegister_SHORTCOG();
}