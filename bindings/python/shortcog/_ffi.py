"""cffi binding to libshortcog. Internal."""

import ctypes.util
import os
from pathlib import Path

from cffi import FFI


# Mirrors the subset of shortcog.h this binding uses; update together.
_CDEF = """
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

typedef struct {
    int64_t shape[4];
    int     ndim;
    int64_t sn;
    int64_t sb;
    int64_t sy;
    int64_t sx;
    int     native;
} shortcog_layout;

typedef struct shortcog_spec shortcog_spec;

int         shortcog_api_version(void);
const char* shortcog_version_string(void);
const char* shortcog_last_error(void);
void        shortcog_clear_error(void);
void        shortcog_free(void* ptr);

shortcog_status
shortcog_index_file(const char* path, unsigned char** out_blob, size_t* out_size);

shortcog_status
shortcog_compile_layout(const char* pattern,
                        int64_t n, int64_t b, int64_t y, int64_t x,
                        shortcog_layout* out);

shortcog_status
shortcog_spec_parse(const unsigned char* blob, size_t blob_size,
                    shortcog_spec** out);

void shortcog_spec_destroy(shortcog_spec* spec);

shortcog_status
shortcog_spec_header(const shortcog_spec* spec, shortcog_header* out);

shortcog_status
shortcog_read(const char* path, const shortcog_spec* spec,
              const int* bands, size_t n_bands,
              int y_off, int y_size, int x_off, int x_size,
              const char* pattern, int num_threads,
              void* dst, size_t dst_size);

shortcog_status
shortcog_read_stack(const char* const* paths,
                    const shortcog_spec* const* specs, size_t n_images,
                    const int* n_index, size_t n_n,
                    const int* bands, size_t n_bands,
                    int y_off, int y_size, int x_off, int x_size,
                    const char* pattern, int num_threads,
                    void* dst, size_t dst_size);
"""


ffi = FFI()
ffi.cdef(_CDEF)

_LIB_GLOBS = ("*.so", "*.so.*", "*.dylib", "*.dll")


def _bundled_lib():
    lib_dir = Path(__file__).parent / "_lib"
    for pattern in _LIB_GLOBS:
        for path in sorted(lib_dir.glob(pattern)):
            return str(path)
    return None


def _load_lib():
    # SHORTCOG_LIB beats everything, then the bundled wheel copy, then the OS path.
    env_path = os.environ.get("SHORTCOG_LIB")
    candidate = env_path or _bundled_lib() or ctypes.util.find_library("shortcog")
    if candidate is None:
        raise OSError(
            "libshortcog not found. Install it or set SHORTCOG_LIB to its path."
        )
    try:
        return ffi.dlopen(candidate)
    except OSError as exc:
        raise OSError(
            f"failed to load libshortcog from {candidate!r}: {exc}. "
            "It links GDAL; make sure a compatible libgdal is on the loader path."
        ) from exc


lib = _load_lib()


_STATUS_TO_EXC = {
    lib.SHORTCOG_ERR_INVALID:     ValueError,
    lib.SHORTCOG_ERR_IO:          IOError,
    lib.SHORTCOG_ERR_PARSE:       ValueError,
    lib.SHORTCOG_ERR_FORMAT:      ValueError,
    lib.SHORTCOG_ERR_DECODE:      IOError,
    lib.SHORTCOG_ERR_OOM:         MemoryError,
    lib.SHORTCOG_ERR_UNSUPPORTED: NotImplementedError,
    lib.SHORTCOG_ERR_INTERNAL:    RuntimeError,
}


def _check(rc):
    if rc == lib.SHORTCOG_OK:
        return
    err = lib.shortcog_last_error()
    msg = (ffi.string(err).decode("utf-8", errors="replace")
           if err != ffi.NULL else "(no error message)")
    raise _STATUS_TO_EXC.get(rc, RuntimeError)(msg)