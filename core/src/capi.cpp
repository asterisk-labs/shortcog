#include "shortcog/shortcog.hpp"

#include "cpl_error.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
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


namespace {

// Expands a NULL/0 bands argument into all bands in file order, 1-based.
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


// Spec.

struct shortcog_spec {
    shortcog::Header h;
    explicit shortcog_spec(shortcog::Header&& hh) noexcept : h(std::move(hh)) {}
};

namespace {

void fill_header(const shortcog::Header& h, shortcog_header* out)
{
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
}

}  // namespace

extern "C" shortcog_status
shortcog_spec_parse(const unsigned char* blob, size_t blob_size,
                    shortcog_spec** out)
{
    return capi_call([&]() -> shortcog_status {
        if (!blob || blob_size == 0 || !out) {
            set_error("shortcog_spec_parse: null or empty argument");
            return SHORTCOG_ERR_INVALID;
        }
        auto parsed = shortcog::parse_blob(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(blob), blob_size));
        if (!parsed) {
            set_error(std::string(shortcog::describe(parsed.error())));
            return SHORTCOG_ERR_PARSE;
        }
        *out = new shortcog_spec(std::move(*parsed));
        return SHORTCOG_OK;
    });
}

extern "C" void shortcog_spec_destroy(shortcog_spec* spec)
{
    delete spec;
}

extern "C" shortcog_status
shortcog_spec_header(const shortcog_spec* spec, shortcog_header* out)
{
    return capi_call([&]() -> shortcog_status {
        if (!spec || !out) {
            set_error("shortcog_spec_header: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        fill_header(spec->h, out);
        return SHORTCOG_OK;
    });
}


// Stateless read.

extern "C" shortcog_status
shortcog_read(const char* path, const shortcog_spec* spec,
              const int* bands, size_t n_bands,
              int y_off, int y_size, int x_off, int x_size,
              const char* pattern, int num_threads,
              void* dst, size_t dst_size)
{
    return capi_call([&]() -> shortcog_status {
        if (!path || !spec || !dst) {
            set_error("shortcog_read: null argument");
            return SHORTCOG_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return SHORTCOG_ERR_INVALID;
        }

        const auto& h = spec->h;
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

        auto r = shortcog::read_window(path, h,
                                       std::span<const int>(picked),
                                       y_off, y_size, x_off, x_size,
                                       *plan, static_cast<std::byte*>(dst),
                                       num_threads);
        if (!r) {
            if (g_last_error.empty()) set_error(r.error());
            return SHORTCOG_ERR_IO;
        }
        return SHORTCOG_OK;
    });
}

extern "C" shortcog_status
shortcog_read_stack(const char* const* paths,
                    const shortcog_spec* const* specs, size_t n_images,
                    const int* n_index, size_t n_n,
                    const int* bands, size_t n_bands,
                    int y_off, int y_size, int x_off, int x_size,
                    const char* pattern, int num_threads,
                    void* dst, size_t dst_size)
{
    return capi_call([&]() -> shortcog_status {
        if (!paths || !specs || n_images == 0 || !dst) {
            set_error("shortcog_read_stack: null or empty argument");
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
        for (size_t i = 0; i < n_images; ++i) {
            if (!paths[i] || !specs[i]) {
                set_error("shortcog_read_stack: null entry at index "
                          + std::to_string(i));
                return SHORTCOG_ERR_INVALID;
            }
        }

        const auto& h        = specs[0]->h;
        const auto  picked_n = resolve_n_index(n_index, n_n, n_images);
        const auto  picked_b = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat      = pattern
            ? pattern : (picked_n.size() > 1 ? "n b y x" : "b y x");

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

        std::vector<const shortcog::Header*> headers;
        headers.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) headers.push_back(&specs[i]->h);

        auto r = shortcog::read_stack(
            std::span<const char* const>(paths, n_images),
            std::span<const shortcog::Header* const>(headers.data(), n_images),
            std::span<const int>(picked_n),
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, static_cast<std::byte*>(dst), num_threads);
        if (!r) {
            if (g_last_error.empty()) set_error(r.error());
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