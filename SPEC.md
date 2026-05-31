# shortcog profile

Specification 1.0.0  
Binary format 1  
Status Draft  
Date 2026-05-30  
License CC BY 4.0  

shortcog is a profile for Cloud Optimized GeoTIFFs (COGs), paired with a compact binary header that lets a reader locate every tile without parsing the COG IFD.

The words MUST, MUST NOT, SHOULD, and MAY in this document carry the RFC 2119 meaning.

## Scope

This document defines the shortcog file profile and the binary layout of the shortcog header blob. The profile tells a validator which COGs are valid shortcog inputs. The blob gives a reader enough information to locate every compressed tile.

shortcog supports exactly one tile ordering: `INTERLEAVE=TILE`. It does not support `INTERLEAVE=BAND` or `INTERLEAVE=PIXEL`. This single constraint is what lets the blob carry one prefix-summable run of byte counts instead of an explicit offset per tile. The Tile ordering section below states it in full.

## File profile

A COG is shortcog compliant only when all of the following are true.

- the file is BigTIFF
- the file uses little-endian byte order (the TIFF header begins with `II`)
- the file has exactly one IFD
- the file has no overviews
- the file has no masks or auxiliary IFDs
- the image is tiled, not stripped
- `PlanarConfiguration` is `2`, meaning Separate
- the tiles use the `INTERLEAVE=TILE` ordering, samples innermost (see Tile ordering). `INTERLEAVE=BAND` and `INTERLEAVE=PIXEL` are not compliant
- the file carries the COG structural metadata `LAYOUT=IFDS_BEFORE_DATA`, `BLOCK_ORDER=ROW_MAJOR`, `BLOCK_LEADER=SIZE_AS_UINT4`, and `BLOCK_TRAILER=LAST_4_BYTES_REPEATED`
- `Compression` is `50000`, meaning ZSTD
- `Predictor` is `1` or `2`
- `Predictor` is `2` only when `SampleFormat` is `1`, `2`, or `3`
- the blob's `bits_per_sample` field is `8`, `16`, `32`, `64`, or `128`
- `SampleFormat` is `1`, `2`, `3`, `5`, or `6`
- the `(sample_format, bits_per_sample)` pair is one of the valid sample encodings listed in this specification
- `tile_width <= image_width` and `tile_length <= image_length`
- `TileOffsets` and `TileByteCounts` are present
- `TileOffsets` and `TileByteCounts` have `tiles_across * tiles_down * samples_per_pixel` entries
- the compressed tile payloads form a single contiguous run in tile-index order, with the 4 byte COG leader and 4 byte trailer between every adjacent pair (see Contiguity).

## Tile ordering

This is the main rule that defines shortcog. A tiled image with more than one sample can store its tiles on disk in three orders. GDAL exposes them through the `INTERLEAVE` creation option of the COG driver. 

| GDAL `INTERLEAVE` | `PlanarConfiguration` | physical layout on disk                                              | shortcog     |
|-------------------|-----------------------|----------------------------------------------------------------------|--------------|
| `PIXEL`           | `1` (Contiguous)      | one tile per spatial position holding all samples interleaved        | not supported |
| `BAND`            | `2` (Separate)        | every tile of sample 0, then every tile of sample 1, and so on       | not supported |
| `TILE`            | `2` (Separate)        | for each spatial position, the tiles of every sample stored together | **required**  |

- `INTERLEAVE=PIXEL` is `PlanarConfiguration = 1`. shortcog requires `PlanarConfiguration = 2`, so a PIXEL file is rejected by the planar-configuration rule alone.
- `INTERLEAVE=BAND` and `INTERLEAVE=TILE` both report `PlanarConfiguration = 2`. The tag cannot tell them apart. They differ only in the physical order of the tiles on disk. shortcog requires the `TILE` order, samples innermost, and rejects the `BAND` order.

The reason for the restriction; the blob stores `tile_byte_counts` in tile-index order and reconstructs every offset by a single prefix sum. That reconstruction is correct only when the physical layout is samples innermost, because then tile-index order coincides with ascending file offset. A `BAND` file lays the same tiles out in a different order, so prefix summing the byte counts in tile-index order would point a reader at the wrong bytes.

For a single-sample image (`samples_per_pixel == 1`) the three orderings coincide and the distinction does not apply.

The `INTERLEAVE=TILE` ordering is produced by GDAL's COG generator driver, version 3.11 or later. The full command needs four creation options:

```
gdal_translate -of COG \
  -co COMPRESS=ZSTD \
  -co INTERLEAVE=TILE \
  -co OVERVIEWS=NONE \
  -co LEVEL=13 \
  -co SPARSE_OK=NO \
  -co PREDICTOR=STANDARD \
  -co BIGTIFF=YES \
  src.tif out.tif
```

- `OVERVIEWS=NONE` is required. The COG driver's `OVERVIEWS` option defaults to `AUTO`, which generates overviews for any image larger than the block size. This profile forbids overviews, so they must be suppressed, or the file gains extra IFDs and is rejected.

- `BIGTIFF=YES` is required. `BIGTIFF` defaults to `IF_NEEDED`, and under compression the driver cannot predict the final size, so it writes a classic TIFF unless forced. This profile requires BigTIFF.

- `INTERLEAVE=TILE` and `COMPRESS=ZSTD` set the ordering and compression this profile requires.

To use horizontal differencing (`predictor = 2`), add `-co PREDICTOR=STANDARD`. Do not use `-co PREDICTOR=YES` on floating point data: `YES` selects the floating-point predictor (Predictor = 3) for float types, which is out of this profile, whereas `STANDARD` forces `Predictor = 2` for every type.

The plain GDAL `GTiff` driver does not, on its own, produce the COG structural metadata or the leader/trailer framing this profile depends on; always use the COG driver.


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

All multi-byte fields are little endian.

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

The value is `0x333C333C`. A reader MUST reject any blob with a different magic value.

### `version`

The binary format version. The current value is `1`.

### `image_width` and `image_length`

The raster size in pixels. These match COG tags `ImageWidth` and `ImageLength`. Each MUST be at least `1` and at most `2147483647`. The upper bound is the largest raster dimension a reader can represent; a larger value cannot describe a valid dataset and a reader MUST reject it.

### `tile_width` and `tile_length`

The tile size in pixels. These match COG tags `TileWidth` and `TileLength`. Tiles are uniform. Edge tiles are padded to the full tile size.

### `samples_per_pixel`

The number of samples per pixel. This matches COG tag `SamplesPerPixel`. All samples use the same `bits_per_sample` and `sample_format`.

### `bits_per_sample`

The width, in bits, of one stored sample slot.

- For non-complex sample formats (`1`, `2`, `3`) this is the per-sample bit width.
- For complex sample formats (`5`, `6`) this is the **sum** of the real and imaginary component widths — twice the per-component width. For example, a complex IEEE 64-bit floating point sample (two IEEE-754 binary64 components) is encoded as `(sample_format = 6, bits_per_sample = 128)`.

Valid values are `8`, `16`, `32`, `64`, and `128`. Any other value is invalid, and any `(sample_format, bits_per_sample)` pair not listed in the table below is invalid.

### `sample_format`

The sample type.

- `1` means unsigned integer
- `2` means signed integer
- `3` means IEEE floating point
- `5` means complex signed integer
- `6` means complex IEEE floating point

The following sample encodings are valid.

| `sample_format` | `bits_per_sample` | sample encoding                                                       |
|----------------:|------------------:|-----------------------------------------------------------------------|
| 1               | 8                 | unsigned 8-bit integer                                                |
| 1               | 16                | unsigned 16-bit integer                                               |
| 1               | 32                | unsigned 32-bit integer                                               |
| 1               | 64                | unsigned 64-bit integer                                               |
| 2               | 8                 | signed 8-bit integer                                                  |
| 2               | 16                | signed 16-bit integer                                                 |
| 2               | 32                | signed 32-bit integer                                                 |
| 2               | 64                | signed 64-bit integer                                                 |
| 3               | 16                | IEEE 16-bit floating point                                            |
| 3               | 32                | IEEE 32-bit floating point                                            |
| 3               | 64                | IEEE 64-bit floating point                                            |
| 5               | 32                | complex signed integer, 16-bit real and 16-bit imaginary components   |
| 5               | 64                | complex signed integer, 32-bit real and 32-bit imaginary components   |
| 6               | 32                | complex IEEE floating point, 16-bit real and 16-bit imaginary components |
| 6               | 64                | complex IEEE floating point, 32-bit real and 32-bit imaginary components |
| 6               | 128               | complex IEEE floating point, 64-bit real and 64-bit imaginary components |

A reader MUST reject any `(sample_format, bits_per_sample)` pair not listed above.

The IEEE 16-bit float encoding `(3, 16)` and the complex IEEE 16-bit float encoding `(6, 32)` require a producer built against GDAL 3.11 or later, where the `Float16` and `CFloat16` types were introduced (GDAL RFC 100). Reading these encodings has no such dependency; a reader interprets the sample bytes directly and never relies on GDAL for the sample type.

### `predictor`

The predictor used before compression.

- `1` means no predictor
- `2` means horizontal differencing

The floating point predictor, value `3`, is not compliant with this shortcog profile.

When `sample_format` is `3` (IEEE floating point) and `predictor` is `2`, the predictor is applied bitwise; the stored bytes of each sample are treated as an unsigned integer of the same width, and horizontal differencing is computed on that integer representation. A writer producing a shortcog file with `(sample_format = 3, predictor = 2)` MUST encode the predictor using this bitwise convention; a reader MUST decode it the same way.

When `sample_format` is `5` or `6` (complex sample formats), `predictor` MUST be `1`. Horizontal differencing across the real and imaginary components of a single stored unit is not defined by this profile. A reader MUST reject any blob where `predictor` is `2` and `sample_format` is `5` or `6`. A writer producing such a file violates this profile even when the underlying COG library accepts the combination.

### `base_tiles_offset`

The absolute byte offset of the first compressed tile payload in the COG. This is the first byte a reader decompresses. It points to the byte after the four byte COG leader of the first tile.

## Tile byte counts

After the 31 byte header, the blob stores `N` little endian `uint32` values. Each entry is the compressed byte size of one tile from the source COG. Every value MUST be greater than zero.

Tiles are stored in tile-index order, samples innermost. For `PlanarConfiguration` `2` this places all samples of one spatial tile contiguously before moving to the next spatial tile, walking spatial positions row-major. The tile index for `(row, col, sample)` is `(row * tiles_across + col) * samples_per_pixel + sample`.

## Offset reconstruction

shortcog stores no explicit tile offsets. A reader reconstructs them by walking the tiles in tile-index order and prefix summing the byte counts; a file is compliant only when that reconstruction matches the real file offsets.

The tile index for `(row, col, sample)` is

```
idx = (row * tiles_across + col) * samples_per_pixel + sample
```

that is, row-major over spatial positions, with samples innermost. Walking `idx` from `0` upward, the physical offset of every tile MUST satisfy

```
offset[0]     = base_tiles_offset
offset[idx+1] = offset[idx] + tile_byte_counts[idx] + 8
```

This check MUST be performed in tile-index order, not on sorted offsets. A `BAND` file is also a single contiguous run and would pass a sorted check while failing the in-order one: it places the next sample of a spatial position where tile-index order expects the next spatial position, so it breaks the rule at the first sample boundary.

The `+ 8` is the tile framing. Each tile on disk is wrapped in two ghost fields written by the COG driver:

- a 4 byte **leader** immediately before the tile payload (`BLOCK_LEADER=SIZE_AS_UINT4`), giving the payload size, and
- a 4 byte **trailer** immediately after the payload (`BLOCK_TRAILER=LAST_4_BYTES_REPEATED`), repeating its last 4 bytes.

Both are required by this profile. `base_tiles_offset` and every reconstructed offset point at the tile *payload*, after the leader, so a reader decompresses exactly `tile_byte_counts[idx]` bytes from there. Between one tile's payload and the next lie that tile's 4 byte trailer and the next tile's 4 byte leader — 8 bytes in total, the `+ 8` above. A file written without the trailer, or with different framing, fails this check and is not compliant.

A reader MUST use the reconstructed offsets when seeking; a raw cumulative sum of `tile_byte_counts` lands at the wrong byte.

## Changelog

- **1.0.0** — Initial draft.
