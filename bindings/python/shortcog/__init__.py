"""shortcog, a COG profile for AI training data."""

from ._ffi import ffi, lib
from ._read import index_file, parse, read
from ._spec import Spec

__all__ = ["Spec", "__version__", "index_file", "parse", "read"]

__version__ = ffi.string(lib.shortcog_version_string()).decode("ascii")