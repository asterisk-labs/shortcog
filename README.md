<p align="center">
  <img src="img/banner.svg" alt="shortcog" width="750"/>
</p>

<p align="center">
  <a href="https://pypi.org/project/shortcog/"><img src="https://img.shields.io/pypi/v/shortcog.svg?color=2b8a3e" alt="PyPI"/></a>
  <img src="https://img.shields.io/pypi/pyversions/shortcog.svg?color=3572A5" alt="Python"/>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue" alt="Platform"/>
  <img src="https://img.shields.io/badge/Windows-unsupported-red" alt="Windows unsupported"/>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License: MIT"/></a>
</p>

<p align="center"><b>A COG profile for AI training data.</b></p>

A GeoTIFF can be written thousands of ways. Interleave, compression, predictor, overviews, tiling. shortcog allows exactly one combination and rejects the rest. The rules are in the [specification](SPEC.md).

That fixed combination makes reading stateless. Normally you open an image, the library walks its header, and you keep it open while you read. shortcog has nothing to explore and nothing to keep open. You index an image once into a small blob, a handful of bytes you keep in your catalog. To read it later you parse that blob into a `Header`, an in-memory object that knows the image size, its data type, and where every tile sits in the file. Parsing is pure local work with no I/O, and the `Header` is what `read` takes to go straight to the bytes.

Index once, store the blob, parse and read. That makes shortcog a good fit for deep learning datasets, where thousands or millions of images can stay parsed and ready to consume instead of being opened one by one.

## Install

```bash
pip install shortcog
```

Wheels are published for **Linux** (x86_64, aarch64) and **macOS** (Apple Silicon, 14+), with GDAL bundled in, so nothing else to install! **Windows is not supported**, sorry!

## Quick start

```python
import shortcog

blob   = shortcog.index_file("scene.tif")   # once per asset; store the blob
header = shortcog.parse(blob)                # local, no I/O
arr    = shortcog.read("scene.tif", header, "b y x", num_threads=4)
```

## Index

```python
blob = shortcog.index_file("scene.tif")
```

Run this once per asset. It reads the file to extract the tile table and returns a compact blob. Store it next to the path in your catalog, a Parquet column works well. The catalog format is not part of shortcog.

## Parse

```python
header = shortcog.parse(blob)
header.shape, header.dtype
```

`parse` rebuilds the tile layout from the blob with no I/O and returns a `Header`, the object you hand to `read`. `header.shape` and `header.dtype` give the image size and type.

## Read

```python
arr = shortcog.read("scene.tif", header, "b y x", b=(0,3), y=(0,512), x=(0,512))  # (3,512,512)
arr = shortcog.read("scene.tif", header, "y x b", b=[3,2,1])                      # HWC, bands reordered
arr = shortcog.read("scene.tif", header, num_threads=4)                           # whole image, parallel decode
```

Returns a numpy array. The first argument after `header` is the output layout (default `"b y x"`). Each axis you name can take a same-named argument.

- a tuple `(start, stop)` is a slice. The cheap case, because it keeps the tiles in the order they sit on disk. `y` and `x` are spatial windows and only take a slice (or all).
- a list `[i, j, k]` picks those 0-based positions in that order. Allowed for `b` (and `n` on a stack), more flexible, but it can scatter the read.
- leaving an axis out reads all of it.

Slices are worth preferring. The profile stores all bands of a tile together, so a band slice reads each tile's bands in order without stepping over the ones you skipped. The reader still does one read per tile, so the win is locality and readahead, not a single seek.

`num_threads` sets tile-decompression parallelism. The pool is process-global and sized on first use, so the first read that asks for threads fixes the count for the whole process. Default is single-threaded.

## Stack

```python
headers = [shortcog.parse(b) for b in blobs]
arr = shortcog.read(paths, headers, "n b y x", n=(0,12), b=(0,4))   # (12,4,Y,X)
arr = shortcog.read(paths, headers, "(n b) y x", b=(0,4))           # fuse layers and bands into channels
arr = shortcog.read(paths, headers, "n (y x) b", b=(0,4))           # tokens per layer
```

Pass lists of paths and headers and `read` adds an `n` axis over the assets. Reorder it, fuse it into channels as `(n b)`, or unfold space into tokens. The assets must match in size and encoding or it raises, no ragged cubes.

## License

MIT

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="img/asterisk_banner.svg" alt="Asterisk Labs" width="400"/>
  </a>
</div>