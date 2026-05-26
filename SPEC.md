# shortcog profile

Specification 1.0.0  
Binary format 1  
Status Draft  
Date 2026-05-26  
License CC BY 4.0

shortcog is a profile for Cloud Optimized GeoTIFFs (COGs), paired with a compact binary header that lets a reader locate every tile without parsing the COG IFD.

The words MUST, MUST NOT, SHOULD, and MAY in this document carry the RFC 2119 meaning.

## Scope

This document defines the shortcog file profile and the binary layout of the shortcog header blob. The profile tells a validator which COGs are valid shortcog inputs. The blob gives a reader enough information to locate every compressed tile.

## File profile

A COG is shortcog compliant only when all of the following are true.

- the file is BigTIFF
- the file has exactly one IFD
- the file has no overviews
- the file has no masks or auxiliary IFDs
- the image is tiled, not stripped
- `PlanarConfiguration` is `2`, meaning Separate (the GDAL COG driver writes this as `INTERLEAVE=TILE`)
- `Compression` is `50000`, meaning ZSTD
- `Predictor` is `1` or `2`
- `BitsPerSample` is `8`, `16`, `32`, or `64`
- `SampleFormat` is `1`, `2`, or `3`
- `tile_width <= image_width` and `tile_length <= image_length`
- `TileOffsets` and `TileByteCounts` are present
- `TileOffsets` and `TileByteCounts` have `tiles_across * tiles_down * samples_per_pixel` entries
- the compressed tile payloads in the file form a single contiguous run, with the 4 byte COG leader and 4 byte trailer between every adjacent pair

The last rule is the important one. Equivalently, when `TileOffsets` is sorted into ascending order along with the matching entries of `TileByteCounts`, every consecutive pair satisfies `sorted_offsets[i + 1] == sorted_offsets[i] + sorted_byte_counts[i] + 8`. The shortcog blob stores `tile_byte_counts` already in physical order, so the reader can reconstruct every tile offset from `base_tiles_offset` and the byte counts using a single prefix sum.

## Header blob

The header blob is a small binary record stored outside the COG or passed to the reader by the caller. A reader uses it directly, without touching the COG IFD.

The blob contains a fixed length header followed by one `uint32` byte count per tile.

```
+---------------+---------------------------+
| Header        | tile_byte_counts[N]       |
+---------------+---------------------------+
  31 bytes        4 * N bytes
```

The reader derives the grid dimensions and `N` from the stored header fields.

```
tiles_across = ceil(image_width / tile_width)
tiles_down   = ceil(image_length / tile_length)
N            = tiles_across * tiles_down * samples_per_pixel
```

`N` is the number of `uint32` entries in the trailing array. The full blob size MUST be `31 + 4 * N` bytes.

## Header fields

| offset | size | type   | name              |
|--------|------|--------|-------------------|
| 0      | 4    | uint32 | magic             |
| 4      | 2    | uint16 | version           |
| 6      | 4    | uint32 | image_width       |
| 10     | 4    | uint32 | image_length      |
| 14     | 2    | uint16 | tile_width        |
| 16     | 2    | uint16 | tile_length       |
| 18     | 2    | uint16 | samples_per_pixel |
| 20     | 1    | uint8  | bits_per_sample   |
| 21     | 1    | uint8  | sample_format     |
| 22     | 1    | uint8  | predictor         |
| 23     | 8    | uint64 | base_tiles_offset |

### `magic`

Identifies the blob as a shortcog header.

The value is `0x474F4353`. In little endian ASCII this reads as `SCOG`. A reader MUST reject any blob with a different magic value.

### `version`

The binary format version. The current value is `1`.

### `image_width` and `image_length`

The raster size in pixels. These match COG tags `ImageWidth` and `ImageLength`.

### `tile_width` and `tile_length`

The tile size in pixels. These match COG tags `TileWidth` and `TileLength`. Tiles are uniform. Edge tiles are padded to the full tile size, as the GDAL COG driver writes them.

### `samples_per_pixel`

The number of bands. This matches COG tag `SamplesPerPixel`. All bands use the same `bits_per_sample` and `sample_format`.

### `bits_per_sample`

The number of bits per sample. Valid values are `8`, `16`, `32`, and `64`.

### `sample_format`

The sample type.

- `1` means unsigned integer
- `2` means signed integer
- `3` means IEEE floating point

### `predictor`

The predictor used before compression.

- `1` means no predictor
- `2` means horizontal differencing

The floating point predictor, value `3`, falls outside this version.

### `base_tiles_offset`

The absolute byte offset of the first compressed tile payload in the COG. This is the first byte a reader decompresses. It points to the byte after the four byte COG leader of the first tile.

## Tile byte counts

After the 31 byte header, the blob stores `N` little endian `uint32` values. Each entry is the compressed byte size of one tile from the source COG. Every value MUST be greater than zero.

Tiles are stored in physical file order. For COGs written by the GDAL COG driver with `INTERLEAVE=TILE`, the physical order places all bands of one spatial tile contiguously before moving to the next tile. The tile index for `(row, col, band)` is `(row * tiles_across + col) * samples_per_pixel + band`.

## Offset reconstruction

A reader reconstructs tile offsets from `base_tiles_offset` and `tile_byte_counts`.

```
tile_offset[0]   = base_tiles_offset
tile_offset[i+1] = tile_offset[i] + tile_byte_counts[i] + 8
```

The `+ 8` is required. It accounts for the four byte COG leader before each tile and the four byte trailer after each tile. A reader MUST use the reconstructed offsets when seeking. A raw cumulative sum of `tile_byte_counts` lands at the wrong byte.