#ifndef SHORTCOG_H
#define SHORTCOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The build hides symbols by default and SHORTCOG_API marks the C ABI
// surface. SHORTCOG_BUILD is defined only on the targets that compile the
// library.
#if defined(SHORTCOG_BUILD)
#  define SHORTCOG_API __attribute__((visibility("default")))
#else
#  define SHORTCOG_API
#endif


// Version.

#define SHORTCOG_API_VERSION 1

SHORTCOG_API int         shortcog_api_version(void);
SHORTCOG_API const char* shortcog_version_string(void);


// Status.

typedef enum {
    SHORTCOG_OK              = 0,
    SHORTCOG_ERR_INVALID     = 1,
    SHORTCOG_ERR_IO          = 2,
    SHORTCOG_ERR_PARSE       = 3,
    SHORTCOG_ERR_FORMAT      = 4,
    SHORTCOG_ERR_DECODE      = 5,
    SHORTCOG_ERR_OOM         = 6,
    SHORTCOG_ERR_UNSUPPORTED = 7,
    SHORTCOG_ERR_INTERNAL    = 99
} shortcog_status;

// Thread-local, NULL when no error is pending. Valid until the next
// library call on the same thread.
SHORTCOG_API const char* shortcog_last_error(void);

SHORTCOG_API void shortcog_clear_error(void);


// Memory.

// Releases buffers the library allocated. Only shortcog_index_file
// allocates, everything else is caller-owned.
SHORTCOG_API void shortcog_free(void* ptr);


// Indexing.

// On success *out_blob is *out_size bytes owned by the caller, released
// with shortcog_free. On failure the out-pointers are left untouched.
SHORTCOG_API shortcog_status
shortcog_index_file(const char*     path,
                    unsigned char** out_blob,
                    size_t*         out_size);


// Header.

// dtype is reported as the spec's (sample_format, bits_per_sample) pair,
// not the GDAL enum, so the ABI does not move when GDAL adds a type.
typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    uint8_t  predictor;
    uint8_t  _reserved;
    uint32_t tiles_across;
    uint32_t tiles_down;
    uint64_t base_tiles_offset;
} shortcog_header;


// Layout.

// shape[0..ndim) is populated, the rest is zero. sn/sb/sy/sx are element
// strides, multiply by bytes_per_sample for bytes. native is 1 when the
// output order is canonical (n) b y x.
typedef struct {
    int64_t shape[4];
    int     ndim;
    int64_t sn;
    int64_t sb;
    int64_t sy;
    int64_t sx;
    int     native;
} shortcog_layout;

// Pure function over (pattern, post-selection extents). For a single
// image pass n = 1 and a pattern without n.
SHORTCOG_API shortcog_status
shortcog_compile_layout(const char*      pattern,
                        int64_t n, int64_t b, int64_t y, int64_t x,
                        shortcog_layout* out);


// Spec.

// A parsed header blob. Pure memory, no file handle, cheap to create and
// destroy, reusable across any number of reads.
typedef struct shortcog_spec shortcog_spec;

SHORTCOG_API shortcog_status
shortcog_spec_parse(const unsigned char* blob, size_t blob_size,
                    shortcog_spec** out);

SHORTCOG_API void shortcog_spec_destroy(shortcog_spec* spec);

SHORTCOG_API shortcog_status
shortcog_spec_header(const shortcog_spec* spec, shortcog_header* out);


// Stateless read.

// Opens path, reads the window, closes. path is a VSI path. bands holds
// 1-based indices in output order, NULL with n_bands = 0 means all bands in
// file order. pattern NULL is shorthand for "b y x". dst must be aligned to
// the sample size and dst_size is checked up front. num_threads > 1 uses the
// process-global pool (sized on first use).
SHORTCOG_API shortcog_status
shortcog_read(const char*          path,
              const shortcog_spec* spec,
              const int*           bands, size_t n_bands,
              int                  y_off, int y_size,
              int                  x_off, int x_size,
              const char*          pattern,
              int                  num_threads,
              void*                dst,   size_t dst_size);

// Stack form. One path+spec per image, all sharing grid, tile size, band
// count, dtype and predictor. n_index holds 1-based image indices, NULL with
// n_n = 0 means all images in order. The pattern must contain n when more
// than one image is selected. dst follows the same alignment rule as
// shortcog_read.
SHORTCOG_API shortcog_status
shortcog_read_stack(const char* const*          paths,
                    const shortcog_spec* const* specs,  size_t n_images,
                    const int*                  n_index, size_t n_n,
                    const int*                  bands,   size_t n_bands,
                    int                         y_off, int y_size,
                    int                         x_off, int x_size,
                    const char*                 pattern,
                    int                         num_threads,
                    void*                       dst,   size_t dst_size);


// GDAL driver.

// Registers the SHORTCOG driver so GDALOpenEx accepts shortcog files
// via the open option "SHORTCOG_HEADER=<base64 blob>". Idempotent.
SHORTCOG_API void GDALRegister_SHORTCOG(void);


#ifdef __cplusplus
}
#endif

#endif  // SHORTCOG_H