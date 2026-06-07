#include "shortcog/shortcog.hpp"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


// Error plumbing.

// Last error is thread-local so concurrent calls don't smash each other.
// CPLError messages produced inside the C++ core are routed here too via
// a scoped error handler installed on every public entry point.

namespace {

thread_local std::string g_last_error;

void set_error(std::string_view msg) noexcept
{
    try { g_last_error.assign(msg); } catch (...) { g_last_error.clear(); }
}

void CPL_STDCALL capture_cpl(CPLErr severity, CPLErrorNum, const char* msg)
{
    if ((severity == CE_Failure || severity == CE_Fatal) && msg) {
        set_error(msg);
    }
}

struct CplScope {
    CplScope() noexcept  { CPLPushErrorHandler(&capture_cpl); }
    ~CplScope() noexcept { CPLPopErrorHandler(); }
    CplScope(const CplScope&)            = delete;
    CplScope& operator=(const CplScope&) = delete;
};

// Wraps the work of one C ABI call. Installs the CPL handler, catches any
// C++ exception, and maps catch-all failures to SHORTCOG_ERR_INTERNAL. The
// body returns the success or domain-specific failure status directly.
template <typename F>
shortcog_status capi_call(F&& body) noexcept
{
    CplScope scope;
    try {
        return body();
    } catch (const std::bad_alloc&) {
        set_error("allocation failed");
        return SHORTCOG_ERR_OOM;
    } catch (const std::exception& e) {
        set_error(e.what());
        return SHORTCOG_ERR_INTERNAL;
    } catch (...) {
        set_error("unknown exception");
        return SHORTCOG_ERR_INTERNAL;
    }
}

}  // namespace


// Opaque handles.

struct shortcog_image {
    shortcog::Image* ds;
    explicit shortcog_image(shortcog::Image* d) noexcept : ds(d) {}
    ~shortcog_image() { if (ds) GDALClose(ds); }
    shortcog_image(const shortcog_image&)            = delete;
    shortcog_image& operator=(const shortcog_image&) = delete;
};

struct shortcog_cube {
    shortcog::ImageCube cube;
    explicit shortcog_cube(shortcog::ImageCube&& c) noexcept : cube(std::move(c)) {}
    shortcog_cube(const shortcog_cube&)            = delete;
    shortcog_cube& operator=(const shortcog_cube&) = delete;
};


// Global.

extern "C" int shortcog_api_version(void)
{
    return SHORTCOG_API_VERSION;
}

extern "C" const char* shortcog_version_string(void)
{
    return SHORTCOG_VERSION_STRING;
}

extern "C" const char* shortcog_last_error(void)
{
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

extern "C" void shortcog_clear_error(void)
{
    g_last_error.clear();
}

extern "C" void shortcog_free(void* ptr)
{
    std::free(ptr);
}


// Indexing.

extern "C" shortcog_status
shortcog_index_file(const char* path, unsigned char** out_blob, size_t* out_size)
{
    return capi_call([&]() -> shortcog_status {
        if (!path || !out_blob || !out_size) {
            set_error("shortcog_index_file: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        auto result = shortcog::build_blob_from_file(path);
        if (!result) {
            set_error(result.error());
            return SHORTCOG_ERR_FORMAT;
        }
        auto& blob = *result;
        auto* buf  = static_cast<unsigned char*>(std::malloc(blob.size()));
        if (!buf) {
            set_error("allocation failed");
            return SHORTCOG_ERR_OOM;
        }
        std::memcpy(buf, blob.data(), blob.size());
        *out_blob = buf;
        *out_size = blob.size();
        return SHORTCOG_OK;
    });
}


// Layout.

extern "C" shortcog_status
shortcog_compile_layout(const char* pattern,
                        int64_t n, int64_t b, int64_t y, int64_t x,
                        shortcog_layout* out)
{
    return capi_call([&]() -> shortcog_status {
        if (!pattern || !out) {
            set_error("shortcog_compile_layout: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        auto plan = shortcog::compile_layout(pattern, n, b, y, x);
        if (!plan) {
            set_error(plan.error());
            return SHORTCOG_ERR_INVALID;
        }
        std::memset(out, 0, sizeof(*out));
        out->ndim = static_cast<int>(plan->shape.size());
        for (size_t i = 0; i < plan->shape.size() && i < 4; ++i) {
            out->shape[i] = plan->shape[i];
        }
        out->sn     = plan->sn;
        out->sb     = plan->sb;
        out->sy     = plan->sy;
        out->sx     = plan->sx;
        out->native = plan->native ? 1 : 0;
        return SHORTCOG_OK;
    });
}


// Image.

namespace {

std::once_flag g_driver_register_once;

void ensure_driver_registered()
{
    std::call_once(g_driver_register_once, []() { shortcog::register_driver(); });
}

}  // namespace

extern "C" shortcog_status
shortcog_image_open(const char* path,
                    const unsigned char* blob, size_t blob_size,
                    int num_threads,
                    shortcog_image** out)
{
    return capi_call([&]() -> shortcog_status {
        if (!path || !blob || blob_size == 0 || !out) {
            set_error("shortcog_image_open: null or empty argument");
            return SHORTCOG_ERR_INVALID;
        }

        ensure_driver_registered();

        // CPLBase64Encode takes int for length; the blob is tiny so this
        // is safe in practice, but check anyway.
        if (blob_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            set_error("blob too large");
            return SHORTCOG_ERR_INVALID;
        }
        std::unique_ptr<char, decltype(&CPLFree)> encoded(
            CPLBase64Encode(static_cast<int>(blob_size), blob), CPLFree);
        if (!encoded) {
            set_error("base64 encode failed");
            return SHORTCOG_ERR_OOM;
        }

        const std::string header_opt  = std::string("SHORTCOG_HEADER=")  + encoded.get();
        const std::string threads_opt = std::string("NUM_THREADS=")      + std::to_string(num_threads);
        const char* options[] = {
            header_opt.c_str(),
            threads_opt.c_str(),
            nullptr
        };

        // Do not request GDAL_OF_THREAD_SAFE here. Image::Open sets it
        // on the returned dataset; asking for it at GDALOpenEx makes
        // GDAL wrap us in a proxy and the dynamic_cast below returns null.
        GDALDataset* ds = GDALDataset::FromHandle(GDALOpenEx(
            path,
            GDAL_OF_RASTER | GDAL_OF_READONLY,
            nullptr,
            const_cast<const char* const*>(options),
            nullptr));

        if (!ds) {
            if (g_last_error.empty()) set_error("GDALOpenEx returned null");
            return SHORTCOG_ERR_IO;
        }

        auto* img = dynamic_cast<shortcog::Image*>(ds);
        if (!img) {
            GDALClose(ds);
            set_error("opened dataset is not a shortcog image");
            return SHORTCOG_ERR_INTERNAL;
        }

        *out = new shortcog_image(img);
        return SHORTCOG_OK;
    });
}

extern "C" void shortcog_image_close(shortcog_image* image)
{
    delete image;
}

extern "C" shortcog_status
shortcog_image_header(const shortcog_image* image, shortcog_header* out)
{
    return capi_call([&]() -> shortcog_status {
        if (!image || !out) {
            set_error("shortcog_image_header: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        const auto& h = image->ds->header();
        std::memset(out, 0, sizeof(*out));
        out->image_width       = h.image_width;
        out->image_length      = h.image_length;
        out->tile_width        = h.tile_width;
        out->tile_length       = h.tile_length;
        out->samples_per_pixel = h.samples_per_pixel;
        out->bits_per_sample   = h.bits_per_sample;
        out->sample_format     = h.sample_format;
        out->predictor         = h.predictor;
        out->tiles_across      = h.tiles_across;
        out->tiles_down        = h.tiles_down;
        out->base_tiles_offset = h.base_tiles_offset;
        return SHORTCOG_OK;
    });
}

namespace {

// Expands a NULL/0 bands argument into all bands in file order, 1-based.
// The vector is returned by value and lives in the caller's frame.
std::vector<int> resolve_bands(const int* bands, size_t n_bands, uint16_t spp)
{
    if (bands && n_bands > 0) {
        return std::vector<int>(bands, bands + n_bands);
    }
    std::vector<int> all;
    all.reserve(spp);
    for (int i = 1; i <= spp; ++i) all.push_back(i);
    return all;
}

}  // namespace

extern "C" shortcog_status
shortcog_image_read(shortcog_image* image,
                    const int* bands, size_t n_bands,
                    int y_off, int y_size, int x_off, int x_size,
                    const char* pattern,
                    void* dst, size_t dst_size)
{
    return capi_call([&]() -> shortcog_status {
        if (!image || !dst) {
            set_error("shortcog_image_read: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return SHORTCOG_ERR_INVALID;
        }

        const auto& h = image->ds->header();
        const auto  picked = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat    = pattern ? pattern : "b y x";

        auto plan = shortcog::compile_layout(
            pat, 1,
            static_cast<int64_t>(picked.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return SHORTCOG_ERR_INVALID;
        }

        const size_t need = static_cast<size_t>(picked.size())
                          * static_cast<size_t>(y_size)
                          * static_cast<size_t>(x_size)
                          * h.bytes_per_sample;
        if (dst_size < need) {
            set_error("dst buffer too small for the requested read");
            return SHORTCOG_ERR_INVALID;
        }

        if (!image->ds->read(std::span<const int>(picked),
                             y_off, y_size, x_off, x_size,
                             *plan, static_cast<std::byte*>(dst))) {
            if (g_last_error.empty()) set_error("image read failed");
            return SHORTCOG_ERR_IO;
        }
        return SHORTCOG_OK;
    });
}


// Cube.

extern "C" shortcog_status
shortcog_cube_create(shortcog_image** images, size_t n_images, shortcog_cube** out)
{
    return capi_call([&]() -> shortcog_status {
        if (!images || n_images == 0 || !out) {
            set_error("shortcog_cube_create: null or empty argument");
            return SHORTCOG_ERR_INVALID;
        }

        // Wrap each borrowed Image* in a shared_ptr with a no-op deleter.
        // The shared_ptr is the vehicle for ImageCube::create; ownership
        // stays with the caller via the shortcog_image handle.
        std::vector<std::shared_ptr<shortcog::Image>> imgs;
        imgs.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) {
            if (!images[i]) {
                set_error("shortcog_cube_create: null image at index "
                          + std::to_string(i));
                return SHORTCOG_ERR_INVALID;
            }
            imgs.emplace_back(images[i]->ds, [](shortcog::Image*) noexcept {});
        }

        auto result = shortcog::ImageCube::create(std::move(imgs));
        if (!result) {
            set_error(result.error());
            return SHORTCOG_ERR_INVALID;
        }

        *out = new shortcog_cube(std::move(*result));
        return SHORTCOG_OK;
    });
}

extern "C" void shortcog_cube_destroy(shortcog_cube* cube)
{
    delete cube;
}

extern "C" shortcog_status
shortcog_cube_header(const shortcog_cube* cube,
                     shortcog_header* out,
                     size_t* out_n_images)
{
    return capi_call([&]() -> shortcog_status {
        if (!cube || !out) {
            set_error("shortcog_cube_header: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        const auto& h = cube->cube.spec();
        std::memset(out, 0, sizeof(*out));
        out->image_width       = h.image_width;
        out->image_length      = h.image_length;
        out->tile_width        = h.tile_width;
        out->tile_length       = h.tile_length;
        out->samples_per_pixel = h.samples_per_pixel;
        out->bits_per_sample   = h.bits_per_sample;
        out->sample_format     = h.sample_format;
        out->predictor         = h.predictor;
        out->tiles_across      = h.tiles_across;
        out->tiles_down        = h.tiles_down;
        out->base_tiles_offset = h.base_tiles_offset;
        if (out_n_images) *out_n_images = cube->cube.size();
        return SHORTCOG_OK;
    });
}

namespace {

std::vector<int> resolve_n_index(const int* n_index, size_t n_n, size_t total)
{
    if (n_index && n_n > 0) {
        return std::vector<int>(n_index, n_index + n_n);
    }
    std::vector<int> all;
    all.reserve(total);
    for (size_t i = 1; i <= total; ++i) all.push_back(static_cast<int>(i));
    return all;
}

}  // namespace

extern "C" shortcog_status
shortcog_cube_read(shortcog_cube* cube,
                   const int* n_index, size_t n_n,
                   const int* bands,   size_t n_bands,
                   int y_off, int y_size, int x_off, int x_size,
                   const char* pattern,
                   void* dst, size_t dst_size)
{
    return capi_call([&]() -> shortcog_status {
        if (!cube || !dst) {
            set_error("shortcog_cube_read: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        if ((n_index == nullptr) != (n_n == 0)) {
            set_error("n_index and n_n must agree (both empty or both set)");
            return SHORTCOG_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return SHORTCOG_ERR_INVALID;
        }

        const auto& h          = cube->cube.spec();
        const auto  picked_n   = resolve_n_index(n_index, n_n, cube->cube.size());
        const auto  picked_b   = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat        = pattern ? pattern : (picked_n.size() > 1 ? "n b y x" : "b y x");

        auto plan = shortcog::compile_layout(
            pat,
            static_cast<int64_t>(picked_n.size()),
            static_cast<int64_t>(picked_b.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return SHORTCOG_ERR_INVALID;
        }

        const size_t need = static_cast<size_t>(picked_n.size())
                          * static_cast<size_t>(picked_b.size())
                          * static_cast<size_t>(y_size)
                          * static_cast<size_t>(x_size)
                          * h.bytes_per_sample;
        if (dst_size < need) {
            set_error("dst buffer too small for the requested read");
            return SHORTCOG_ERR_INVALID;
        }

        if (!cube->cube.read(std::span<const int>(picked_n),
                             std::span<const int>(picked_b),
                             y_off, y_size, x_off, x_size,
                             *plan, static_cast<std::byte*>(dst))) {
            if (g_last_error.empty()) set_error("cube read failed");
            return SHORTCOG_ERR_IO;
        }
        return SHORTCOG_OK;
    });
}


// GDAL driver.

extern "C" void GDALRegister_SHORTCOG(void)
{
    shortcog::register_driver();
}