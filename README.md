<p align="center">
  <img src="img/banner.svg" alt="shortcog" width="750"/>
</p>

A COG profile for AI training data.

shortcog reads tiled COGs without walking the TIFF header. You index each asset once to get a small blob, store it in your catalog, and open from the blob. Opening touches no bytes, it just rebuilds the tile offsets, so reads go straight to the tiles.

Built for loops that read millions of chips, not for general GeoTIFF browsing.

## Install

```bash
pip install shortcog
```

## Index

```python
import shortcog

header = shortcog.index_file("scene.tif")
```

Run this once per asset. It reads the file to extract the tile table and returns a compact blob. Store it next to the path in your catalog. A Parquet column works well. The catalog format is not part of shortcog.

## Open

```python
img  = shortcog.open("scene.tif", header)    # one asset  ->  axes  b y x
cube = shortcog.open(paths, headers)         # many       ->  axes  n b y x
```

`open` is local. It rebuilds the tile offsets from the blob by prefix sum and touches no bytes, so opening a million assets is essentially free. With many assets it builds a cube and validates that they share grid, dtype, and predictor. If they do not, it raises. There is no ragged container, on purpose. If you really have mismatched assets, open them one at a time and loop.

## Read

```python
arr = img.read("b y x", b=(0,3), y=(0,512), x=(0,512))   # (3,512,512)
arr = img.read("y x b", b=[3,2,1])                        # HWC, bands reordered
```

Returns a numpy array. The string is the output layout. Every axis named in it can be constrained by a same named argument:

- a tuple `(start, stop)` is a slice. Contiguous, and the fast path: a slice maps to one byte contiguous run of tiles, which is what the reader wants.
- a list `[i, j, k]` is a cardinal selection, read in that exact order. Flexible, but it can scatter the read.
- omitted means the whole axis.

Prefer slices. Because the profile forces `INTERLEAVE=TILE`, the tiles of a band range sit contiguously on disk, so even `b=(0,3)` stays a single contiguous read.

```python
arr = img.read("b y x", b=(0,3), num_threads=4, pin_memory=False)
```

`num_threads` controls tile decompression parallelism. `pin_memory=True` writes into a page locked buffer for faster transfer to GPU in single process pipelines.

## Stack

```python
arr = cube.read("n b y x", n=(0,12), b=(0,4))   # (12,4,Y,X)
arr = cube.read("(n b) y x", b=(0,4))           # fuse layers and bands into channels
arr = cube.read("n (y x) b", b=(0,4))           # tokens per layer
```

A cube adds the `n` axis over the opened assets, with the same idiom: slice `n` for a contiguous span, list it to pick. Because a cube is dense, `n` takes part in the layout, so you can reorder it, fuse it as `(n b)`, or unfold the spatial axes into tokens. Slices on every axis keep the whole read byte contiguous.

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
