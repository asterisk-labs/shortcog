#include "shortcog/shortcog.hpp"
#include "shortcog/thread_pool.hpp"

#include "cpl_vsi_virtual.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace shortcog {
namespace {

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

struct FileCloser {
    void operator()(VSILFILE* f) const noexcept { if (f) VSIFCloseL(f); }
};
using FilePtr = std::unique_ptr<VSILFILE, FileCloser>;

std::expected<void, std::string>
validate_request(const Header& h, std::span<const int> bands,
                 int y_off, int y_size, int x_off, int x_size)
{
    if (bands.empty()) return err("no bands selected");
    for (int b : bands) {
        if (b < 1 || b > h.samples_per_pixel) {
            return err("band " + std::to_string(b) + " out of range [1, "
                       + std::to_string(h.samples_per_pixel) + "]");
        }
    }
    if (x_off < 0 || y_off < 0 || x_size <= 0 || y_size <= 0 ||
        static_cast<std::int64_t>(x_off) + x_size > h.image_width ||
        static_cast<std::int64_t>(y_off) + y_size > h.image_length) {
        return err("requested window out of bounds");
    }
    return {};
}

int clamp_threads(int n) noexcept
{
    constexpr int MAX_THREADS = 1024;
    return n < 1 ? 1 : (n > MAX_THREADS ? MAX_THREADS : n);
}

}  // namespace


TileSpec make_tile_spec(const Header& h) noexcept
{
    return TileSpec{
        h.predictor,
        h.tile_width,
        h.tile_length,
        static_cast<std::uint8_t>(h.bytes_per_sample),
        h.max_tile_size,
    };
}

// One TileTask per intersecting (tile, band). A full, unclipped tile whose
// output is tile-contiguous decompresses straight into the buffer; everything
// else lands in per-thread scratch and is copied out, honoring pixel_space.
Plan build_plan(const Header& h, VSILFILE* file,
                int x_off, int y_off, int x_size, int y_size,
                std::byte* data,
                std::span<const int> bands,
                GSpacing pixel_space, GSpacing line_space, GSpacing band_space)
{
    const int tw  = h.tile_width;
    const int tl  = h.tile_length;
    const std::size_t bps = h.bytes_per_sample;
    const int band_count = static_cast<int>(bands.size());

    const int tx_min = x_off / tw;
    const int ty_min = y_off / tl;
    const int tx_max = static_cast<int>(
        (static_cast<std::int64_t>(x_off) + x_size + tw - 1) / tw);
    const int ty_max = static_cast<int>(
        (static_cast<std::int64_t>(y_off) + y_size + tl - 1) / tl);

    const bool contiguous_output =
        pixel_space == static_cast<GSpacing>(bps) &&
        line_space  == static_cast<GSpacing>(tw) * static_cast<GSpacing>(bps);

    Plan plan;
    plan.spec = make_tile_spec(h);
    plan.tasks.reserve(static_cast<std::size_t>(tx_max - tx_min) *
                       (ty_max - ty_min) * band_count);

    for (int ty = ty_min; ty < ty_max; ++ty) {
        for (int tx = tx_min; tx < tx_max; ++tx) {
            const int tile_px = tx * tw;
            const int tile_py = ty * tl;
            const int ix0 = std::max(tile_px, x_off);
            const int iy0 = std::max(tile_py, y_off);
            const int ix1 = std::min({tile_px + tw, x_off + x_size,
                                      static_cast<int>(h.image_width)});
            const int iy1 = std::min({tile_py + tl, y_off + y_size,
                                      static_cast<int>(h.image_length)});
            if (ix1 <= ix0 || iy1 <= iy0) continue;

            const bool full_tile =
                ix0 == tile_px && iy0 == tile_py &&
                ix1 == tile_px + tw && iy1 == tile_py + tl;
            const bool direct = full_tile && contiguous_output;

            for (int i = 0; i < band_count; ++i) {
                const auto band = static_cast<std::uint32_t>(bands[i] - 1);
                const std::uint32_t idx = h.tile_index(
                    static_cast<std::uint32_t>(ty),
                    static_cast<std::uint32_t>(tx),
                    band);

                std::byte* dst = data
                    + static_cast<GSpacing>(iy0 - y_off) * line_space
                    + static_cast<GSpacing>(ix0 - x_off) * pixel_space
                    + static_cast<GSpacing>(i)          * band_space;

                TileTask task{};
                task.file            = file;
                task.offset          = h.tile_offset(idx);
                task.compressed_size = h.tile_byte_counts[idx];
                if (direct) {
                    task.direct = dst;
                } else {
                    task.dst              = dst;
                    task.src_x            = static_cast<std::uint32_t>(ix0 - tile_px);
                    task.src_y            = static_cast<std::uint32_t>(iy0 - tile_py);
                    task.w                = static_cast<std::uint32_t>(ix1 - ix0);
                    task.h                = static_cast<std::uint32_t>(iy1 - iy0);
                    task.dst_pitch        = static_cast<std::size_t>(line_space);
                    task.dst_pixel_stride = static_cast<std::size_t>(pixel_space);
                }
                plan.tasks.push_back(task);
            }
        }
    }

    return plan;
}


std::expected<void, std::string>
read_window(const char* path, const Header& h,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst,
            int num_threads)
{
    if (!path) return err("path is null");
    if (auto ok = validate_request(h, bands, y_off, y_size, x_off, x_size); !ok) {
        return ok;
    }

    FilePtr file(VSIFOpenL(path, "rb"));
    if (!file) return err(std::string("could not open: ") + path);
    if (!file->HasPRead()) {
        return err(std::string(path) + ": VSI handle does not support PRead");
    }

    ThreadPool* pool = nullptr;
    if (clamp_threads(num_threads) > 1) {
        pool = &global_thread_pool(
            static_cast<unsigned>(clamp_threads(num_threads)));
    }

    const std::size_t bps = h.bytes_per_sample;
    Plan plan = build_plan(h, file.get(),
                           x_off, y_off, x_size, y_size, dst, bands,
                           static_cast<GSpacing>(layout.sx) * bps,
                           static_cast<GSpacing>(layout.sy) * bps,
                           static_cast<GSpacing>(layout.sb) * bps);

    Executor exec(pool);
    if (!exec.run(plan)) return err("read failed");
    return {};
}


std::expected<void, std::string>
read_stack(std::span<const char* const> paths,
           std::span<const Header* const> headers,
           std::span<const int> n_index,
           std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           int num_threads)
{
    if (paths.empty() || paths.size() != headers.size()) {
        return err("paths and headers must be non-empty and the same length");
    }
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (!paths[i])   return err("null path at index " + std::to_string(i + 1));
        if (!headers[i]) return err("null spec at index " + std::to_string(i + 1));
    }

    const Header& ref = *headers[0];
    for (std::size_t i = 1; i < headers.size(); ++i) {
        const Header& h = *headers[i];
        if (h.image_width != ref.image_width || h.image_length != ref.image_length) {
            return err("image " + std::to_string(i + 1) + ": image size mismatch");
        }
        if (h.tile_width != ref.tile_width || h.tile_length != ref.tile_length) {
            return err("image " + std::to_string(i + 1) + ": tile size mismatch");
        }
        if (h.samples_per_pixel != ref.samples_per_pixel) {
            return err("image " + std::to_string(i + 1) + ": band count mismatch");
        }
        if (h.gdal_type != ref.gdal_type) {
            return err("image " + std::to_string(i + 1) + ": dtype mismatch");
        }
        if (h.predictor != ref.predictor) {
            return err("image " + std::to_string(i + 1) + ": predictor mismatch");
        }
    }

    if (n_index.empty()) return err("no images selected");
    for (int ni : n_index) {
        if (ni < 1 || static_cast<std::size_t>(ni) > headers.size()) {
            return err("n=" + std::to_string(ni) + " out of range [1, "
                       + std::to_string(headers.size()) + "]");
        }
    }

    const std::size_t n_stride =
        static_cast<std::size_t>(layout.sn) * ref.bytes_per_sample;

    for (std::size_t k = 0; k < n_index.size(); ++k) {
        const std::size_t i = static_cast<std::size_t>(n_index[k] - 1);
        if (auto ok = read_window(paths[i], *headers[i], bands,
                                  y_off, y_size, x_off, x_size,
                                  layout, dst + k * n_stride, num_threads);
            !ok) {
            return err("image " + std::to_string(n_index[k]) + ": " + ok.error());
        }
    }
    return {};
}

}  // namespace shortcog