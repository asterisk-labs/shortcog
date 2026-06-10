"""shortcog — a COG profile for AI training data."""

import os

import numpy as np

from ._ffi import ffi, lib, _check


__all__ = ["index_file", "open", "Image", "Cube", "__version__"]

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
# API. None means "all" and gets passed through as NULL/0.
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


class Image:
    def __init__(self, path, blob, *, num_threads=1):
        self._handle = None
        if not isinstance(blob, (bytes, bytearray, memoryview)):
            raise TypeError(f"blob must be bytes-like, got {type(blob).__name__}")

        blob_buf = ffi.from_buffer("unsigned char[]", blob)
        handle_out = ffi.new("shortcog_image**")
        _check(lib.shortcog_image_open(
            _enc(path), blob_buf, len(blob), num_threads, handle_out
        ))
        handle = handle_out[0]

        try:
            header = ffi.new("shortcog_header*")
            _check(lib.shortcog_image_header(handle, header))
            self._dtype = _np_dtype(header.sample_format, header.bits_per_sample)
        except BaseException:
            lib.shortcog_image_close(handle)
            raise

        self._handle = handle
        self._header = header

    def close(self):
        if self._handle:
            lib.shortcog_image_close(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @property
    def shape(self):
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self):
        return self._dtype

    def __repr__(self):
        h = self._header
        return (f"<shortcog.Image {h.image_width}x{h.image_length} "
                f"bands={h.samples_per_pixel} dtype={self._dtype.__name__}>")

    def read(self, pattern="b y x", *, b=None, y=None, x=None):
        h = self._header
        bands = _resolve_axis(b, "b", h.samples_per_pixel)
        y_off, y_size = _resolve_window(y, "y", h.image_length)
        x_off, x_size = _resolve_window(x, "x", h.image_width)

        n_bands = len(bands) if bands is not None else h.samples_per_pixel
        layout = ffi.new("shortcog_layout*")
        _check(lib.shortcog_compile_layout(
            pattern.encode("ascii"), 1, n_bands, y_size, x_size, layout
        ))
        shape = tuple(layout.shape[i] for i in range(layout.ndim))
        arr = np.empty(shape, dtype=self._dtype)

        bands_c, n_bands_c = _to_c(bands)
        _check(lib.shortcog_image_read(
            self._handle, bands_c, n_bands_c,
            y_off, y_size, x_off, x_size,
            pattern.encode("ascii"),
            ffi.cast("void*", arr.ctypes.data), arr.nbytes,
        ))
        return arr


class Cube:
    def __init__(self, images):
        self._handle = None
        if not images:
            raise ValueError("Cube requires at least one image")
        self._images = list(images)

        handles = ffi.new(f"shortcog_image*[{len(self._images)}]")
        for i, img in enumerate(self._images):
            if img._handle is None:
                raise ValueError(f"image at index {i} is closed")
            handles[i] = img._handle

        cube_out = ffi.new("shortcog_cube**")
        _check(lib.shortcog_cube_create(handles, len(self._images), cube_out))

        self._handle = cube_out[0]
        self._header = self._images[0]._header
        self._dtype = self._images[0]._dtype

    def close(self):
        if self._handle:
            lib.shortcog_cube_destroy(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @property
    def shape(self):
        h = self._header
        return (len(self._images), h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self):
        return self._dtype

    def __repr__(self):
        h = self._header
        return (f"<shortcog.Cube n={len(self._images)} "
                f"{h.image_width}x{h.image_length} bands={h.samples_per_pixel} "
                f"dtype={self._dtype.__name__}>")

    def read(self, pattern=None, *, n=None, b=None, y=None, x=None):
        h = self._header
        n_sel = _resolve_axis(n, "n", len(self._images))
        bands = _resolve_axis(b, "b", h.samples_per_pixel)
        y_off, y_size = _resolve_window(y, "y", h.image_length)
        x_off, x_size = _resolve_window(x, "x", h.image_width)

        n_count = len(n_sel) if n_sel is not None else len(self._images)
        n_bands = len(bands) if bands is not None else h.samples_per_pixel
        if pattern is None:
            pattern = "n b y x" if n_count > 1 else "b y x"

        layout = ffi.new("shortcog_layout*")
        _check(lib.shortcog_compile_layout(
            pattern.encode("ascii"), n_count, n_bands, y_size, x_size, layout
        ))
        shape = tuple(layout.shape[i] for i in range(layout.ndim))
        arr = np.empty(shape, dtype=self._dtype)

        n_c, n_count_c = _to_c(n_sel)
        bands_c, n_bands_c = _to_c(bands)
        _check(lib.shortcog_cube_read(
            self._handle, n_c, n_count_c, bands_c, n_bands_c,
            y_off, y_size, x_off, x_size,
            pattern.encode("ascii"),
            ffi.cast("void*", arr.ctypes.data), arr.nbytes,
        ))
        return arr


# Polymorphic: single path/blob -> Image, lists -> Cube.
def open(paths, blobs, *, num_threads=1):
    if isinstance(paths, (str, bytes, os.PathLike)):
        return Image(paths, blobs, num_threads=num_threads)

    paths = list(paths)
    blobs = list(blobs)
    if len(paths) != len(blobs):
        raise ValueError(
            f"paths and blobs length mismatch: {len(paths)} vs {len(blobs)}"
        )
    return Cube([Image(p, b, num_threads=num_threads) for p, b in zip(paths, blobs)])