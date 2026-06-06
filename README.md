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
img  = shortcog.open("scene.tif", header)                 # one asset  ->  axes  b y x
img  = shortcog.open("scene.tif", header, num_threads=4)  # decompress tiles in parallel
cube = shortcog.open(paths, headers)                      # many       ->  axes  n b y x
```

`open` is local. It rebuilds the tile offsets from the blob by prefix sum and touches no bytes, so opening a million assets is essentially free. With many assets it builds a cube and validates that they share grid, tile size, band count, dtype, and predictor. If they do not, it raises. There is no ragged container, on purpose. If you really have mismatched assets, open them one at a time and loop.

`num_threads` sets the tile-decompression parallelism. The worker pool is process-global and sized on first use, so the first `open` that asks for threads fixes the count for the whole process; later calls share it. Absent means single-threaded.

## Read

```python
arr = img.read("b y x", b=(0,3), y=(0,512), x=(0,512))   # (3,512,512)
arr = img.read("y x b", b=[3,2,1])                        # HWC, bands reordered
```

Returns a numpy array. The string is the output layout. Every axis you name in it can take a same named argument that says what to read from it.

- a tuple `(start, stop)` is a slice, a range of the axis. This is the cheap case, because a slice keeps the tiles you need in the order they sit on disk.
- a list `[i, j, k]` picks those positions in that exact order. More flexible, but it can scatter the read.
- leaving an axis out means you want all of it.

Use slices when you can. The profile stores all bands of a tile together, with samples innermost, so a band slice reads each tile's bands in order and never steps over the bands you did not ask for. The reader still does one read per tile, it does not merge them, so what you gain is locality and readahead, not a single seek. A list over `b` or `n` breaks that order and can spread the reads across the file.

## Stack

```python
arr = cube.read("n b y x", n=(0,12), b=(0,4))   # (12,4,Y,X)
arr = cube.read("(n b) y x", b=(0,4))           # fuse layers and bands into channels
arr = cube.read("n (y x) b", b=(0,4))           # tokens per layer
```

A cube adds an `n` axis over the assets you opened. It works just like the other axes. Slice `n` to take a span, or pass a list to pick assets in the order you want. Since `n` is part of the layout you can reorder it, fuse it into the channels as `(n b)`, or unfold the spatial axes into tokens.

Each asset is its own file, so a cube read just walks the assets one by one. Slicing still helps inside each one, because a slice keeps that asset's tiles in the order they sit on disk.

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