#pragma once

#include "shortcog.h"

#include "gdal_priv.h"
#include "cpl_vsi.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace shortcog {

class ThreadPool;

inline constexpr std::uint32_t MAGIC            = 0x333C333C;  // "<3<3"
inline constexpr std::uint16_t VERSION          = 1;
inline constexpr std::size_t   HEADER_SIZE      = 31;
inline constexpr std::uint64_t COG_TILE_FRAMING = 8;          // 4B leader + 4B trailer


// Blob format

#pragma pack(push, 1)
struct BlobHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint32_t image_width;
    std::uint32_t image_length;
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint16_t samples_per_pixel;
    // For complex formats (5, 6) this holds the summed component widths.
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
    offset_overflow,
};

[[nodiscard]] SHORTCOG_API std::string_view describe(ParseError e) noexcept;

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

    std::vector<std::uint32_t> tile_byte_counts;
    std::vector<std::uint64_t> tile_offsets;

    [[nodiscard]] std::uint64_t tile_offset(std::uint32_t i) const noexcept {
        return tile_offsets[i];
    }

    [[nodiscard]] std::uint32_t tile_index(std::uint32_t row,
                                           std::uint32_t col,
                                           std::uint32_t band) const noexcept {
        return (row * tiles_across + col) * samples_per_pixel + band;
    }
};

// Allocates the tile arrays, so not noexcept. Format errors use ParseError.
[[nodiscard]] SHORTCOG_API std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob);

[[nodiscard]] SHORTCOG_API GDALDataType
infer_gdal_type(std::uint8_t bits_per_sample,
                std::uint8_t sample_format) noexcept;


// Predictor

SHORTCOG_API void apply_horizontal_predictor(std::span<std::byte> tile,
                                              std::uint16_t tile_width,
                                              std::uint16_t tile_length,
                                              std::uint8_t  bytes_per_sample) noexcept;


// Plan / Executor

struct TileSpec {
    std::uint8_t  predictor;
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint8_t  bytes_per_sample;
    std::size_t   tile_bytes;
};

// One tile, single band. When direct is set the tile decompresses straight into
// the output; otherwise it lands in scratch and the w x h rect is copied to dst
// with dst_pitch per row and dst_pixel_stride per pixel. All positions are in
// the result buffer, never the disk layout.
struct TileTask {
    VSILFILE*     file;
    std::uint64_t offset;
    std::uint32_t compressed_size;
    std::byte*    direct;
    std::byte*    dst;
    std::uint32_t src_x;
    std::uint32_t src_y;
    std::uint32_t w;
    std::uint32_t h;
    std::size_t   dst_pitch;
    std::size_t   dst_pixel_stride;
};

struct Plan {
    std::vector<TileTask> tasks;
    TileSpec              spec;
};

class Executor {
public:
    explicit Executor(ThreadPool* pool) noexcept;
    [[nodiscard]] bool run(const Plan& plan) const;

private:
    ThreadPool* pool_;
};

[[nodiscard]] SHORTCOG_API TileSpec make_tile_spec(const Header& h) noexcept;

// Maps a window/band/stride request onto TileTasks. Callers validate the
// window and band indices (1-based) beforehand.
[[nodiscard]] SHORTCOG_API Plan
build_plan(const Header& h, VSILFILE* file,
           int x_off, int y_off, int x_size, int y_size,
           std::byte* data,
           std::span<const int> bands,
           GSpacing pixel_space, GSpacing line_space, GSpacing band_space);


// Layout

// Output placement from compile_layout. sn/sb/sy/sx are per-axis element
// strides, scaled by bytes_per_sample at read time. native means the block is
// already plain (n, b, y, x) C-contiguous so a binding adopts it without a
// copy.
struct LayoutPlan {
    std::vector<std::int64_t> shape;
    std::int64_t              sn{};
    std::int64_t              sb{};
    std::int64_t              sy{};
    std::int64_t              sx{};
    bool                      native{};
};

// Maps a pattern to per-axis strides for a read of size (n, b, y, x), the
// post-selection extents. A single image uses n = 1 with no n in the pattern.
// Whole-axis permute and merge only. Allocates, so not noexcept.
[[nodiscard]] SHORTCOG_API std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t b,
               std::int64_t y, std::int64_t x);


// Stateless read

// Opens path via VSI, plans the window, runs it, closes. bands are 1-based.
// num_threads > 1 uses the process-global pool (sized on first use).
[[nodiscard]] SHORTCOG_API std::expected<void, std::string>
read_window(const char* path, const Header& h,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst,
            int num_threads);

// Validates that every header shares grid, tile size, band count, dtype and
// predictor, then reads each selected asset (n_index, 1-based) into its
// layout.sn slice of dst.
[[nodiscard]] SHORTCOG_API std::expected<void, std::string>
read_stack(std::span<const char* const> paths,
           std::span<const Header* const> headers,
           std::span<const int> n_index,
           std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           int num_threads);


// Image (GDAL driver only; the native fast path is read_window/read_stack)

class Image;

class Band : public GDALRasterBand {
public:
    Band(Image* image, int band_index) noexcept;
    ~Band() override = default;

    Band(const Band&)            = delete;
    Band& operator=(const Band&) = delete;

    CPLErr IReadBlock(int x_block, int y_block, void* buffer) override;

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     GSpacing pixel_space, GSpacing line_space,
                     GDALRasterIOExtraArg* extra_arg) override;

    bool MayMultiBlockReadingBeMultiThreaded() const override;

    // The default block cache is not internally locked, so serialize the
    // entry points that GDAL_OF_THREAD_SAFE may reach from several threads.
    GDALRasterBlock* GetLockedBlockRef(int x_block, int y_block,
                                       int just_initialize = FALSE) override;
    GDALRasterBlock* TryGetLockedBlockRef(int x_block, int y_block) override;
    CPLErr FlushBlock(int x_block, int y_block,
                      int write_dirty = TRUE) override;

private:
    Image*               image_{nullptr};
    std::recursive_mutex block_cache_mutex_;
};

class SHORTCOG_API Image : public GDALDataset {
public:
    static constexpr const char* OPEN_OPTION_HEADER  = "SHORTCOG_HEADER";
    static constexpr const char* OPEN_OPTION_THREADS = "NUM_THREADS";

    Image();
    ~Image() override;

    Image(const Image&)            = delete;
    Image& operator=(const Image&) = delete;

    static GDALDataset* Open(GDALOpenInfo* open_info);
    static int          Identify(GDALOpenInfo* open_info);

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     int band_count, int* band_map,
                     GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                     GDALRasterIOExtraArg* extra_arg) override;

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] VSILFILE*     file()   const noexcept { return file_.get(); }
    [[nodiscard]] ThreadPool*   pool()   const noexcept { return pool_; }

private:
    Header                    header_{};
    std::shared_ptr<VSILFILE> file_;
    ThreadPool*               pool_{nullptr};
};


// Builder

// Stays noexcept. Its large allocations are wrapped and reported as a string.
[[nodiscard]] SHORTCOG_API std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path) noexcept;

SHORTCOG_API void register_driver();

}  // namespace shortcog