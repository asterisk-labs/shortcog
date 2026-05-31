#include "shortcog/shortcog.hpp"
#include "shortcog/thread_pool.hpp"

#include "cpl_error.h"
#include "cpl_vsi_virtual.h"

#include <zstd.h>

#include <atomic>
#include <cstring>
#include <new>
#include <vector>

namespace shortcog {
namespace {

// One dctx per thread; ZSTD_decompressDCtx resets it per call. Buffers grow to
// the largest tile seen and never shrink.
struct WorkerState {
    ZSTD_DCtx*             dctx = ZSTD_createDCtx();
    std::vector<std::byte> compressed;
    std::vector<std::byte> scratch;

    WorkerState() = default;
    ~WorkerState() { if (dctx) ZSTD_freeDCtx(dctx); }

    WorkerState(const WorkerState&)            = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

WorkerState& worker_state() noexcept
{
    thread_local WorkerState ws;
    return ws;
}

// dst_pixel_stride == sample size: contiguous row. Larger: the output layout
// puts another axis inner, so place pixels one by one. Result buffer only.
void copy_rect(const TileTask& t, const TileSpec& spec,
               const std::byte* tile) noexcept
{
    const std::size_t bps       = spec.bytes_per_sample;
    const std::size_t src_pitch = static_cast<std::size_t>(spec.tile_width) * bps;

    if (t.dst_pixel_stride == bps) {
        const std::size_t row_bytes = static_cast<std::size_t>(t.w) * bps;
        for (std::uint32_t row = 0; row < t.h; ++row) {
            const std::byte* src = tile
                + static_cast<std::size_t>(t.src_y + row) * src_pitch
                + static_cast<std::size_t>(t.src_x) * bps;
            std::memcpy(t.dst + static_cast<std::size_t>(row) * t.dst_pitch,
                        src, row_bytes);
        }
        return;
    }

    for (std::uint32_t row = 0; row < t.h; ++row) {
        const std::byte* src = tile
            + static_cast<std::size_t>(t.src_y + row) * src_pitch
            + static_cast<std::size_t>(t.src_x) * bps;
        std::byte* dst = t.dst + static_cast<std::size_t>(row) * t.dst_pitch;
        for (std::uint32_t col = 0; col < t.w; ++col) {
            std::memcpy(dst + static_cast<std::size_t>(col) * t.dst_pixel_stride,
                        src + static_cast<std::size_t>(col) * bps, bps);
        }
    }
}

bool execute_task(const TileTask& t, const TileSpec& spec) noexcept
{
    WorkerState& ws = worker_state();
    if (!ws.dctx) {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "shortcog: could not allocate ZSTD context");
        return false;
    }

    if (ws.compressed.size() < t.compressed_size) {
        try {
            ws.compressed.resize(t.compressed_size);
        } catch (const std::bad_alloc&) {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "shortcog: out of memory growing compressed scratch");
            return false;
        }
    }

    // PRead: the parallel path holds no file lock.
    const std::size_t got = t.file->PRead(
        ws.compressed.data(), t.compressed_size, t.offset);
    if (got != t.compressed_size) {
        CPLError(CE_Failure, CPLE_FileIO,
                 "shortcog: short read at " CPL_FRMT_GUIB ": %zu of %u",
                 static_cast<GUIntBig>(t.offset), got, t.compressed_size);
        return false;
    }

    std::byte* tile = t.direct;
    if (!tile) {
        if (ws.scratch.size() < spec.tile_bytes) {
            try {
                ws.scratch.resize(spec.tile_bytes);
            } catch (const std::bad_alloc&) {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "shortcog: out of memory growing tile scratch");
                return false;
            }
        }
        tile = ws.scratch.data();
    }

    const std::size_t produced = ZSTD_decompressDCtx(
        ws.dctx, tile, spec.tile_bytes, ws.compressed.data(), t.compressed_size);

    if (ZSTD_isError(produced)) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "shortcog: ZSTD failed: %s", ZSTD_getErrorName(produced));
        return false;
    }
    if (produced != spec.tile_bytes) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "shortcog: decompressed %zu bytes, expected %zu",
                 produced, spec.tile_bytes);
        return false;
    }

    if (spec.predictor == 2) {
        apply_horizontal_predictor(
            std::span<std::byte>(tile, spec.tile_bytes),
            spec.tile_width, spec.tile_length, spec.bytes_per_sample);
    }

    if (!t.direct) copy_rect(t, spec, tile);
    return true;
}

}  // namespace


Executor::Executor(ThreadPool* pool) noexcept : pool_(pool) {}

bool Executor::run(const Plan& plan) const
{
    if (plan.tasks.empty()) return true;

    std::atomic<bool> ok{true};

    if (pool_ != nullptr && plan.tasks.size() > 1) {
        ThreadPool::Batch batch(*pool_);
        for (const TileTask& t : plan.tasks) {
            batch.submit([&t, &plan, &ok]() {
                if (!ok.load(std::memory_order_relaxed)) return;
                if (!execute_task(t, plan.spec)) {
                    ok.store(false, std::memory_order_relaxed);
                }
            });
        }
        batch.wait();
    } else {
        for (const TileTask& t : plan.tasks) {
            if (!execute_task(t, plan.spec)) { ok.store(false); break; }
        }
    }

    return ok.load();
}

}  // namespace shortcog