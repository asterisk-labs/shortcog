"""shortcog, a COG profile for AI training data."""

import os

import numpy as np

from ._ffi import ffi, lib, _check


__all__ = ["index_file", "parse", "read", "Spec", "__version__"]

__version__ = ffi.string(lib.shortcog_version_string()).decode("ascii")


# Spec dtype encoding (sample_format, bits_per_sample) -> numpy dtype.
# Complex int and 16-bit complex float have no numpy equivalent and raise.
_DTYPE = {
    (1, 8):   np.uint8,
    (1, 16):  np.uint16,
    (1, 32):  np.uint32,
    (1, 64):  np.uint64,
    (2, 8):   np.int8,
    (2, 16):  np.int16,
    (2, 32):  np.int32,
    (2, 64):  np.int64,
    (3, 16):  np.float16,
    (3, 32):  np.float32,
    (3, 64):  np.float64,
    (6, 64):  np.complex64,
    (6, 128): np.complex128,
}


def _np_dtype(sf, bps):
    try:
        return _DTYPE[(sf, bps)]
    except KeyError:
        raise NotImplementedError(
            f"no numpy dtype for (sample_format={sf}, bits_per_sample={bps})"
        )


def _enc(path):
    return path.encode("utf-8") if isinstance(path, str) else os.fsencode(path)


# Tuple selectors are Python slices (start, stop), 0-based. Lists are 0-based
# explicit indices in the order requested. Both convert to 1-based for the C
# API. None means all and gets passed through as NULL/0.
def _resolve_axis(sel, name, total):
    if sel is None:
        return None
    if isinstance(sel, tuple):
        if len(sel) != 2:
            raise ValueError(f"{name}: tuple must be (start, stop)")
        start, stop = sel
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: slice ({start}, {stop}) out of [0, {total}]")
        return list(range(start + 1, stop + 1))
    if isinstance(sel, list):
        out = [int(i) + 1 for i in sel]
        for x in out:
            if not (1 <= x <= total):
                raise ValueError(f"{name}: index {x - 1} out of [0, {total})")
        return out
    raise TypeError(f"{name}: expected tuple or list, got {type(sel).__name__}")


def _resolve_window(sel, name, total):
    if sel is None:
        return 0, total
    if isinstance(sel, tuple) and len(sel) == 2:
        start, stop = sel
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: window ({start}, {stop}) out of [0, {total}]")
        return start, stop - start
    raise TypeError(f"{name}: expected (start, stop) tuple")


def _to_c(lst):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


def index_file(path):
    blob_out = ffi.new("unsigned char**")
    size_out = ffi.new("size_t*")
    _check(lib.shortcog_index_file(_enc(path), blob_out, size_out))
    try:
        return bytes(ffi.buffer(blob_out[0], size_out[0]))
    finally:
        lib.shortcog_free(blob_out[0])


class Spec:
    """Parsed header blob. Pure memory, no file handle, reusable across reads."""

    def __init__(self, blob):
        self._handle = None
        if not isinstance(blob, (bytes, bytearray, memoryview)):
            raise TypeError(f"blob must be bytes-like, got {type(blob).__name__}")

        blob_buf = ffi.from_buffer("unsigned char[]", blob)
        handle_out = ffi.new("shortcog_spec**")
        _check(lib.shortcog_spec_parse(blob_buf, len(blob), handle_out))
        handle = handle_out[0]

        try:
            header = ffi.new("shortcog_header*")
            _check(lib.shortcog_spec_header(handle, header))
            self._dtype = _np_dtype(header.sample_format, header.bits_per_sample)
        except BaseException:
            lib.shortcog_spec_destroy(handle)
            raise

        self._handle = handle
        self._header = header

    def __del__(self):
        if self._handle:
            lib.shortcog_spec_destroy(self._handle)
            self._handle = None

    @property
    def shape(self):
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self):
        return self._dtype

    def __repr__(self):
        h = self._header
        return (f"<shortcog.Spec {h.image_width}x{h.image_length} "
                f"bands={h.samples_per_pixel} dtype={self._dtype.__name__}>")


def parse(blob):
    return Spec(blob)


def _read_one(path, spec, pattern, b, y, x, num_threads):
    h = spec._header
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    if pattern is None:
        pattern = "b y x"

    layout = ffi.new("shortcog_layout*")
    _check(lib.shortcog_compile_layout(
        pattern.encode("ascii"), 1, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))
    arr = np.empty(shape, dtype=spec._dtype)

    bands_c, n_bands_c = _to_c(bands)
    _check(lib.shortcog_read(
        _enc(path), spec._handle, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads,
        ffi.cast("void*", arr.ctypes.data), arr.nbytes,
    ))
    return arr


def _read_stack(paths, specs, pattern, n, b, y, x, num_threads):
    paths = list(paths)
    specs = list(specs)
    if len(paths) != len(specs):
        raise ValueError(
            f"paths and specs length mismatch: {len(paths)} vs {len(specs)}"
        )
    if not paths:
        raise ValueError("read requires at least one image")

    h = specs[0]._header
    n_sel = _resolve_axis(n, "n", len(specs))
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    n_count = len(n_sel) if n_sel is not None else len(specs)
    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    if pattern is None:
        pattern = "n b y x" if n_count > 1 else "b y x"

    layout = ffi.new("shortcog_layout*")
    _check(lib.shortcog_compile_layout(
        pattern.encode("ascii"), n_count, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))
    arr = np.empty(shape, dtype=specs[0]._dtype)

    paths_c = [ffi.new("char[]", _enc(p)) for p in paths]
    paths_arr = ffi.new("char*[]", paths_c)
    specs_arr = ffi.new("shortcog_spec*[]", [s._handle for s in specs])

    n_c, n_count_c = _to_c(n_sel)
    bands_c, n_bands_c = _to_c(bands)
    _check(lib.shortcog_read_stack(
        paths_arr, specs_arr, len(specs),
        n_c, n_count_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads,
        ffi.cast("void*", arr.ctypes.data), arr.nbytes,
    ))
    return arr


# Polymorphic and stateless. A single path/spec reads one image, lists read a
# stack. Opens, reads, closes, with nothing kept alive between calls.
def read(paths, specs, pattern=None, *, n=None, b=None, y=None, x=None,
         num_threads=1):
    if isinstance(paths, (str, bytes, os.PathLike)):
        if n is not None:
            raise ValueError("n applies to a stack; pass lists of paths/specs")
        return _read_one(paths, specs, pattern, b, y, x, num_threads)
    return _read_stack(paths, specs, pattern, n, b, y, x, num_threads)