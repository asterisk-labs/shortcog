<p align="center">
  <img src="img/banner.svg" alt="shortcog" />
</p>

A COG profile for AI training data.

shortcog reads tiled COGs without walking the TIFF header. You generate a small blob once with the indexer, store it next to the asset in your catalog, and pass it on every read. The reader uses the blob to go straight to the tiles.

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

Run this once per asset. Store the header next to the path in your catalog. A Parquet column works well. The catalog format is not part of shortcog.

## Read

```python
arr = shortcog.read(
    "scene.tif",
    header,
    bands=[3, 2, 1],
    window=(0, 0, 512, 512),
    num_threads=1,
    pin_memory=False,
)
```

Returns a numpy array. `bands=None` reads every band, `window=None` reads the full extent. `num_threads` controls tile decompression parallelism. `pin_memory=True` writes into a page locked buffer for faster transfer to GPU in single process pipelines.

## Read a batch

```python
arrs = shortcog.read_batch(
    paths,
    headers,
    bands=[3, 2, 1],
    window=(0, 0, 512, 512),
    num_threads=1,
    pin_memory=False,
)
```

The batch entry point. Tile jobs from every sample share one thread pool call instead of being submitted one read at a time, so the CPU stays busy and reads can be interleaved. Use this when you have many samples to feed at once.

## License

MIT

---

<p align="center">
  Made with ♥ by <a href="https://asterisk.coop">Asterisk Labs</a>
</p>

<p align="center">
  <img src="img/asterisk_banner.svg" alt="Asterisk Labs" />
</p>