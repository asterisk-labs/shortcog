import numpy as np

from . import _repr
from ._ffi import _check, ffi, lib

# Spec dtype encoding (sample_format, bits_per_sample) -> numpy dtype.
# Complex int and 16-bit complex float have no numpy equivalent and raise.
_DTYPE: dict[tuple[int, int], type[np.generic]] = {
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


# predictor doubles as codec selector. 1/2 are ZSTD (TIFF predictor none/
# horizontal); reserved high values pick a codec that needs no predictor.
_CODEC = {1: "ZSTD", 2: "ZSTD + horizontal", 42: "OpenZL"}


def _np_dtype(sf: int, bps: int) -> type[np.generic]:
    try:
        return _DTYPE[(sf, bps)]
    except KeyError:
        raise NotImplementedError(
            f"no numpy dtype for (sample_format={sf}, bits_per_sample={bps})"
        ) from None


class Spec:
    """Parsed header blob. Pure memory, no file handle, reusable across reads."""

    def __init__(self, blob: bytes | bytearray | memoryview) -> None:
        if not isinstance(blob, (bytes, bytearray, memoryview)):
            raise TypeError(f"blob must be bytes-like, got {type(blob).__name__}")

        blob_buf = ffi.from_buffer("unsigned char[]", blob)
        handle_out = ffi.new("shortcog_spec**")
        _check(lib.shortcog_spec_parse(blob_buf, len(blob), handle_out))
        # ffi.gc frees the handle whenever it goes away, even mid-__init__.
        self._handle = ffi.gc(handle_out[0], lib.shortcog_spec_destroy)

        header = ffi.new("shortcog_header*")
        _check(lib.shortcog_spec_header(self._handle, header))
        self._header = header
        self._dtype = _np_dtype(header.sample_format, header.bits_per_sample)

    @property
    def shape(self) -> tuple[int, int, int]:
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self) -> type[np.generic]:
        return self._dtype

    def _facts(self) -> dict:
        try:
            h = self._header
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": self._dtype.__name__,
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "tiles": h.tiles_across * h.tiles_down * h.samples_per_pixel,
                "codec": _CODEC.get(h.predictor, "ZSTD"),
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return _repr.text(self._facts())

    def _repr_html_(self) -> str:
        return _repr.html_(self._facts())