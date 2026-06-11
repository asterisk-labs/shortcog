#include "shortcog/shortcog.hpp"
#include "shortcog/thread_pool.hpp"

#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "cpl_vsi_virtual.h"
#include "memdataset.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <vector>

namespace shortcog {

namespace {

// One TileTask per intersecting (tile, band), via the shared planner.
CPLErr read_native(const Image* img,
                   int x_off, int y_off, int x_size, int y_size,
                   void* data,
                   int band_count, const int* band_map,
                   GSpacing pixel_space, GSpacing line_space, GSpacing band_space)
{
    Plan plan = build_plan(img->header(), img->file(),
                           x_off, y_off, x_size, y_size,
                           static_cast<std::byte*>(data),
                           std::span<const int>(band_map,
                               static_cast<std::size_t>(band_count)),
                           pixel_space, line_space, band_space);

    Executor exec(img->pool());
    return exec.run(plan) ? CE_None : CE_Failure;
}

// Read a tile-aligned native superset into a MEMDataset and let GDAL do the
// resampling and type conversion.
CPLErr read_via_mem(const Image* img,
                    int x_off, int y_off, int x_size, int y_size,
                    void* data, int buf_x_size, int buf_y_size,
                    GDALDataType buf_type,
                    int band_count, const int* band_map,
                    GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                    GDALRasterIOExtraArg* extra_arg)
{
    const Header& h = img->header();
    const int tw = h.tile_width;
    const int tl = h.tile_length;
    const std::size_t bps = h.bytes_per_sample;

    // The tile-rounded end can blow past int range, so clamp in int64 first.
    // image_width and image_length fit int, so the clamped result does too.
    const int ax_off = (x_off / tw) * tw;
    const int ay_off = (y_off / tl) * tl;
    const int ax_end = static_cast<int>(std::min<std::int64_t>(h.image_width,
        ((static_cast<std::int64_t>(x_off) + x_size + tw - 1) / tw) * tw));
    const int ay_end = static_cast<int>(std::min<std::int64_t>(h.image_length,
        ((static_cast<std::int64_t>(y_off) + y_size + tl - 1) / tl) * tl));
    const int ax_size = ax_end - ax_off;
    const int ay_size = ay_end - ay_off;

    const std::size_t band_stride =
        static_cast<std::size_t>(ax_size) * ay_size * bps;

    std::vector<std::byte> buffer;
    try {
        buffer.resize(band_stride * static_cast<std::size_t>(band_count));
    } catch (const std::bad_alloc&) {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "shortcog: out of memory for staging buffer");
        return CE_Failure;
    }

    if (CPLErr err = read_native(img, ax_off, ay_off, ax_size, ay_size,
            buffer.data(), band_count, band_map,
            static_cast<GSpacing>(bps),
            static_cast<GSpacing>(bps) * ax_size,
            static_cast<GSpacing>(band_stride));
        err != CE_None) {
        return err;
    }

    auto mem = std::unique_ptr<MEMDataset>(
        MEMDataset::Create("", ax_size, ay_size, 0, GDT_Unknown, nullptr));
    if (!mem) return CE_Failure;

    for (int i = 0; i < band_count; ++i) {
        GByte* band_data = reinterpret_cast<GByte*>(buffer.data())
                         + static_cast<std::size_t>(i) * band_stride;
        GDALRasterBandH b = MEMCreateRasterBandEx(
            mem.get(), i + 1, band_data, h.gdal_type,
            static_cast<GSpacing>(bps),
            static_cast<GSpacing>(bps) * ax_size,
            false);
        mem->AddMEMBand(b);
    }

    GDALRasterIOExtraArg sub = *extra_arg;
    if (sub.bFloatingPointWindowValidity) {
        sub.dfXOff -= ax_off;
        sub.dfYOff -= ay_off;
    }

    return mem->RasterIO(GF_Read, x_off - ax_off, y_off - ay_off,
                         x_size, y_size, data, buf_x_size, buf_y_size,
                         buf_type, band_count, nullptr,
                         pixel_space, line_space, band_space, &sub);
}

}  // namespace


// Band

Band::Band(Image* image, int band_index) noexcept : image_(image)
{
    poDS        = image;
    nBand       = band_index;
    eDataType   = image->header().gdal_type;
    nBlockXSize = image->header().tile_width;
    nBlockYSize = image->header().tile_length;
}

bool Band::MayMultiBlockReadingBeMultiThreaded() const
{
    return image_->pool() != nullptr;
}

GDALRasterBlock* Band::GetLockedBlockRef(int x_block, int y_block,
                                         int just_initialize)
{
    std::lock_guard guard(block_cache_mutex_);
    return GDALRasterBand::GetLockedBlockRef(x_block, y_block, just_initialize);
}

GDALRasterBlock* Band::TryGetLockedBlockRef(int x_block, int y_block)
{
    std::lock_guard guard(block_cache_mutex_);
    return GDALRasterBand::TryGetLockedBlockRef(x_block, y_block);
}

CPLErr Band::FlushBlock(int x_block, int y_block, int write_dirty)
{
    std::lock_guard guard(block_cache_mutex_);
    return GDALRasterBand::FlushBlock(x_block, y_block, write_dirty);
}

CPLErr Band::IReadBlock(int x_block, int y_block, void* buffer)
{
    const Header& h = image_->header();
    const std::uint32_t idx = h.tile_index(
        static_cast<std::uint32_t>(y_block),
        static_cast<std::uint32_t>(x_block),
        static_cast<std::uint32_t>(nBand - 1));

    Plan plan;
    plan.spec = make_tile_spec(h);

    TileTask task{};
    task.file            = image_->file();
    task.offset          = h.tile_offset(idx);
    task.compressed_size = h.tile_byte_counts[idx];
    task.direct          = static_cast<std::byte*>(buffer);
    plan.tasks.push_back(task);

    Executor exec(image_->pool());
    return exec.run(plan) ? CE_None : CE_Failure;
}

CPLErr Band::IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                       int x_size, int y_size, void* data,
                       int buf_x_size, int buf_y_size, GDALDataType buf_type,
                       GSpacing pixel_space, GSpacing line_space,
                       GDALRasterIOExtraArg* extra_arg)
{
    int band_map[] = {nBand};
    return image_->IRasterIO(
        rw_flag, x_off, y_off, x_size, y_size, data, buf_x_size, buf_y_size,
        buf_type, 1, band_map, pixel_space, line_space, 0, extra_arg);
}


// Image

Image::Image()  = default;
Image::~Image() = default;

int Image::Identify(GDALOpenInfo* open_info)
{
    if (CSLFetchNameValue(open_info->papszOpenOptions, OPEN_OPTION_HEADER) != nullptr) {
        return TRUE;
    }
    return -1;
}

GDALDataset* Image::Open(GDALOpenInfo* open_info)
{
    if (Identify(open_info) != TRUE) return nullptr;
    if (open_info->eAccess == GA_Update) {
        CPLError(CE_Failure, CPLE_NotSupported, "SHORTCOG is read-only");
        return nullptr;
    }

    const char* header_b64 = CSLFetchNameValue(
        open_info->papszOpenOptions, OPEN_OPTION_HEADER);

    CPLString decoded(header_b64);
    int decoded_size = CPLBase64DecodeInPlace(
        reinterpret_cast<GByte*>(decoded.data()));
    if (decoded_size <= 0) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to base64-decode SHORTCOG_HEADER");
        return nullptr;
    }

    auto blob = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(decoded.data()),
        static_cast<std::size_t>(decoded_size));
    auto parsed = parse_blob(blob);
    if (!parsed) {
        const auto why = describe(parsed.error());
        CPLError(CE_Failure, CPLE_AppDefined,
                 "SHORTCOG blob invalid: %.*s",
                 static_cast<int>(why.size()), why.data());
        return nullptr;
    }

    // parse_blob already guarantees samples_per_pixel is nonzero and fits
    // uint16, but a count GDAL deems unreasonable (GDAL_MAX_BAND_COUNT) would
    // still drive the SetBand loop into a huge allocation, so reject it like
    // the in-tree drivers do before touching the file.
    if (!GDALCheckBandCount(parsed->samples_per_pixel, FALSE)) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "SHORTCOG: unreasonable band count %u",
                 parsed->samples_per_pixel);
        return nullptr;
    }

    VSILFILE* fp = VSIFOpenL(open_info->pszFilename, "rb");
    if (!fp) {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Cannot open file: %s", open_info->pszFilename);
        return nullptr;
    }

    // PRead is mandatory because we advertise GDAL_OF_THREAD_SAFE and the
    // parallel path takes no file lock.
    if (!fp->HasPRead()) {
        VSIFCloseL(fp);
        CPLError(CE_Failure, CPLE_NotSupported,
                 "%s: VSI handle does not support PRead",
                 open_info->pszFilename);
        return nullptr;
    }

    auto file = std::shared_ptr<VSILFILE>(fp, [](VSILFILE* f) { VSIFCloseL(f); });

    auto img = std::make_unique<Image>();
    img->header_      = std::move(*parsed);
    img->file_        = std::move(file);
    img->nRasterXSize = static_cast<int>(img->header_.image_width);
    img->nRasterYSize = static_cast<int>(img->header_.image_length);

    for (int i = 1; i <= img->header_.samples_per_pixel; ++i) {
        img->SetBand(i, new Band(img.get(), i));
    }
    img->SetDescription(open_info->pszFilename);

    // NUM_THREADS is an integer or ALL_CPUS, absent meaning single-threaded.
    // The pool is process-global and sized on first use, so whichever dataset
    // first enables threading fixes the worker count for every one after it.
    int n_threads = 1;
    if (const char* nt = CSLFetchNameValue(open_info->papszOpenOptions,
                                           OPEN_OPTION_THREADS)) {
        n_threads = EQUAL(nt, "ALL_CPUS") ? CPLGetNumCPUs() : std::atoi(nt);
    }
    constexpr int MAX_THREADS = 1024;
    if (n_threads < 1) {
        n_threads = 1;
    } else if (n_threads > MAX_THREADS) {
        CPLDebug("SHORTCOG", "NUM_THREADS=%d clamped to %d",
                 n_threads, MAX_THREADS);
        n_threads = MAX_THREADS;
    }
    if (n_threads > 1) {
        img->pool_ = &global_thread_pool(static_cast<unsigned>(n_threads));
    }

    img->nOpenFlags = GDAL_OF_RASTER | GDAL_OF_THREAD_SAFE;

    return img.release();
}

CPLErr Image::IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                        int x_size, int y_size, void* data,
                        int buf_x_size, int buf_y_size, GDALDataType buf_type,
                        int band_count, int* band_map,
                        GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                        GDALRasterIOExtraArg* extra_arg)
{
    if (rw_flag != GF_Read || !data || !band_map || band_count <= 0) {
        return CE_Failure;
    }

    // GDAL's public RasterIO already validates the window and bands, but
    // ImageCube reads reach us directly, so the same checks are repeated here.
    if (x_off < 0 || y_off < 0 || x_size <= 0 || y_size <= 0 ||
        static_cast<std::int64_t>(x_off) + x_size > nRasterXSize ||
        static_cast<std::int64_t>(y_off) + y_size > nRasterYSize) {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "shortcog: requested window out of bounds");
        return CE_Failure;
    }
    for (int i = 0; i < band_count; ++i) {
        if (band_map[i] < 1 || band_map[i] > header_.samples_per_pixel) {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "shortcog: band %d out of range [1, %u]",
                     band_map[i], header_.samples_per_pixel);
            return CE_Failure;
        }
    }

    GDALRasterIOExtraArg default_arg;
    INIT_RASTERIO_EXTRA_ARG(default_arg);
    if (!extra_arg) extra_arg = &default_arg;

    // read_native handles any pixel/line/band spacing, so only resampling or a
    // dtype change needs the staging path.
    const bool fast_path =
        x_size == buf_x_size && y_size == buf_y_size &&
        buf_type == header_.gdal_type;

    if (!fast_path) {
        return read_via_mem(this, x_off, y_off, x_size, y_size,
                            data, buf_x_size, buf_y_size, buf_type,
                            band_count, band_map,
                            pixel_space, line_space, band_space, extra_arg);
    }

    return read_native(this, x_off, y_off, x_size, y_size, data,
                       band_count, band_map,
                       pixel_space, line_space, band_space);
}

// Driver registration

void register_driver()
{
    if (GDALGetDriverByName("SHORTCOG") != nullptr) return;

    auto driver = std::make_unique<GDALDriver>();
    driver->SetDescription("SHORTCOG");
    driver->SetMetadataItem(GDAL_DMD_LONGNAME, "Shortcog fast read path");
    driver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "tif tiff");
    driver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    driver->SetMetadataItem(GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='SHORTCOG_HEADER' type='string' required='true'/>"
        "  <Option name='NUM_THREADS' type='string' default='1'/>"
        "</OpenOptionList>");

    driver->pfnOpen     = Image::Open;
    driver->pfnIdentify = Image::Identify;

    GetGDALDriverManager()->RegisterDriver(driver.release());
}

}  // namespace shortcog