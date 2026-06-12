import os
from collections.abc import Sequence

import numpy as np

from ._ffi import _check, ffi, lib
from ._spec import Spec

# (start, stop) slice, explicit 0-based indices, or None for all.
Axis = tuple[int, int] | list[int] | None
PathLike = str | bytes | os.PathLike


def _enc(path: PathLike) -> bytes:
    return path.encode("utf-8") if isinstance(path, str) else os.fsencode(path)


# Tuple selectors are Python slices (start, stop), 0-based. Lists are 0-based
# explicit indices in the order requested. Both convert to 1-based for the C
# API. None means all and gets passed through as NULL/0.
def _resolve_axis(sel: Axis, name: str, total: int) -> list[int] | None:
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


def _resolve_window(sel: tuple[int, int] | None, name: str,
                    total: int) -> tuple[int, int]:
    if sel is None:
        return 0, total
    if isinstance(sel, tuple) and len(sel) == 2:
        start, stop = sel
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: window ({start}, {stop}) out of [0, {total}]")
        return start, stop - start
    raise TypeError(f"{name}: expected (start, stop) tuple")


def _to_c(lst: list[int] | None):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


def index_file(path: PathLike) -> bytes:
    blob_out = ffi.new("unsigned char**")
    size_out = ffi.new("size_t*")
    _check(lib.shortcog_index_file(_enc(path), blob_out, size_out))
    try:
        return bytes(ffi.buffer(blob_out[0], size_out[0]))
    finally:
        lib.shortcog_free(blob_out[0])


def parse(blob: bytes | bytearray | memoryview) -> Spec:
    return Spec(blob)


def _read_one(path: PathLike, spec: Spec, pattern: str | None,
              b: Axis, y: tuple[int, int] | None, x: tuple[int, int] | None,
              num_threads: int) -> np.ndarray:
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


def _read_stack(paths: Sequence[PathLike], specs: Sequence[Spec],
                pattern: str | None, n: Axis, b: Axis,
                y: tuple[int, int] | None, x: tuple[int, int] | None,
                num_threads: int) -> np.ndarray:
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
def read(paths: PathLike | Sequence[PathLike],
         specs: Spec | Sequence[Spec],
         pattern: str | None = None, *,
         n: Axis = None, b: Axis = None,
         y: tuple[int, int] | None = None,
         x: tuple[int, int] | None = None,
         num_threads: int = 1) -> np.ndarray:
    if isinstance(paths, (str, bytes, os.PathLike)):
        if n is not None:
            raise ValueError("n applies to a stack; pass lists of paths/specs")
        return _read_one(paths, specs, pattern, b, y, x, num_threads)
    return _read_stack(paths, specs, pattern, n, b, y, x, num_threads)