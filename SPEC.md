# shortcog profile

Specification 1.0.0  
Binary format 1  
Status Draft  
Date 2026-06-05  
License CC BY 4.0  

shortcog is a profile for Cloud Optimized GeoTIFFs (COGs), paired with a compact binary header that lets a reader locate every tile without parsing the COG IFD.

The words MUST, MUST NOT, SHOULD, and MAY in this document carry the RFC 2119 meaning.

## Scope

This document defines the shortcog file profile and the binary layout of the shortcog header blob. The profile tells a validator which COGs are valid shortcog inputs. The blob gives a reader enough information to locate every compressed tile.

shortcog supports exactly one tile ordering: `INTERLEAVE=TILE`. It does not support `INTERLEAVE=BAND` or `INTERLEAVE=PIXEL`. This single constraint is what lets the blob carry one prefix-summable run of byte counts instead of an explicit offset per tile. The Tile ordering section below states it in full.

## File profile

A COG is shortcog compliant only when all of the following are true.

- the file is BigTIFF;
- the file uses little-endian byte order, so the TIFF header begins with `II`;
- the file has exactly one IFD;
- the file has no overviews;
- the file has no masks or auxiliary IFDs;
- the image is tiled, not stripped;
- `PlanarConfiguration` is `2`, meaning Separate;
- the tiles use `INTERLEAVE=TILE` ordering, with samples innermost. `INTERLEAVE=BAND` and `INTERLEAVE=PIXEL` are not compliant;
- the file carries the COG structural metadata `LAYOUT=IFDS_BEFORE_DATA`, `BLOCK_ORDER=ROW_MAJOR`, `BLOCK_LEADER=SIZE_AS_UINT4`, and `BLOCK_TRAILER=LAST_4_BYTES_REPEATED`;
- `Compression` is `50000`, meaning ZSTD;
- `Predictor` is `1` or `2`;
- `Predictor` is `2` only when `SampleFormat` is `1`, `2`, or `3`;
- the blob's `bits_per_sample` field is `8`, `16`, `32`, `64`, or `128`;
- `SampleFormat` is `1`, `2`, `3`, `5`, or `6`;
- the `(sample_format, bits_per_sample)` pair is one of the valid sample encodings listed in this specification;
- `tile_width <= image_width` and `tile_length <= image_length`;
- `TileOffsets` and `TileByteCounts` are present;
- `TileOffsets` and `TileByteCounts` have `tiles_across * tiles_down * samples_per_pixel` entries;
- no tile is sparse. Every tile payload is physically present in the file and every `TileByteCounts` entry is greater than zero. A sparse file leaves holes where it omits zero or nodata tiles, and those holes break the prefix-sum reconstruction; and
- the compressed tile payloads form one contiguous run in tile-index order, with a 4-byte COG leader before each payload and a 4-byte COG trailer after each payload, as described in Contiguity and offset reconstruction.


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


### Producing a source COG with GDAL

GDAL's COG generator can produce `INTERLEAVE=TILE` starting with GDAL 3.11. Use the COG driver, not the plain `GTiff` driver, because shortcog depends on the COG structural metadata and on the leader/trailer bytes written around each tile payload.

A minimal command for `Predictor = 1` is:

```bash
gdal_translate -of COG \
  -co COMPRESS=ZSTD \
  -co INTERLEAVE=TILE \
  -co OVERVIEWS=NONE \
  -co SPARSE_OK=FALSE \
  -co PREDICTOR=NO \
  -co BIGTIFF=YES \
  src.tif out.tif
```

A ZSTD level may be added if desired; it does not change whether the file is shortcog-compliant:

```bash
  -co LEVEL=13
```

These creation options matter for compliance:

- `INTERLEAVE=TILE` sets the physical tile order required by shortcog.
- `COMPRESS=ZSTD` sets the required compression.
- `OVERVIEWS=NONE` is required because this profile allows exactly one IFD and forbids overviews.
- `BIGTIFF=YES` is required because the profile accepts BigTIFF only. With compression, GDAL cannot know the final size in advance, so the default BigTIFF heuristic is not enough for this profile.

`SPARSE_OK=FALSE` is already the default. What the profile really needs is that no tile is missing and no byte count is zero. `SPARSE_OK=TRUE` would break that, because it drops empty tiles and leaves holes. You do not have to trust the flag though. The check further down rebuilds the offsets and rejects any gap or any zero byte count, so a file that passes it is fine no matter how it was made. Just do not set `SPARSE_OK=TRUE`.

To use horizontal differencing (`Predictor = 2`), replace `-co PREDICTOR=NO` with:

```bash
-co PREDICTOR=STANDARD
```

Do not use `-co PREDICTOR=YES` for floating point data. In GDAL, `YES` selects the floating-point predictor (`Predictor = 3`) for floating point types, and Predictor 3 is outside this profile. `STANDARD` requests the standard horizontal differencing predictor instead.

## Header blob

The header blob is a small binary record stored outside the COG that is passed to the reader. A reader uses it directly and does not need to inspect the COG IFD before seeking to tile payloads.

The blob contains a fixed length header followed by one `uint32` byte count per tile.

```
+---------------+---------------------------+
| Header        | tile_byte_counts[N]       |
+---------------+---------------------------+
  31 bytes        4 * N bytes
```

The reader derives the tile grid and `N` from the header fields:

```text
tiles_across = ceil(image_width / tile_width)
tiles_down   = ceil(image_length / tile_length)
N            = tiles_across * tiles_down * samples_per_pixel
```

`N` is the number of `uint32` entries in the trailing array. The full blob size MUST be exactly `31 + 4 * N` bytes.

All multi-byte fields are little endian. The header is packed; it contains no padding bytes.

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

The value is `0x333C333C`. A reader MUST reject any blob with a different value.

### `version`

The binary format version. The current value is `1`.

A reader that implements only binary format 1 MUST reject any other version.

### `image_width` and `image_length`

The raster size in pixels. These fields match the COG tags `ImageWidth` and `ImageLength`.

Each value MUST be at least `1` and at most `2147483647`. The upper bound is the largest raster dimension this profile allows a reader to represent. A larger value cannot describe a valid shortcog dataset, and a reader MUST reject it.

### `tile_width` and `tile_length`

The tile size in pixels. These fields match the COG tags `TileWidth` and `TileLength`.

Each value MUST be at least `1`. `tile_width` MUST be less than or equal to `image_width`, and `tile_length` MUST be less than or equal to `image_length`.

Tiles are uniform. Edge tiles are padded to the full tile size, as in normal tiled TIFF storage.

### `samples_per_pixel`

The number of samples per pixel. This field matches the COG tag `SamplesPerPixel`.

The value MUST be at least `1`. All samples use the same `bits_per_sample` and `sample_format`.

### `bits_per_sample`

The width, in bits, of one stored sample slot.

For non-complex sample formats (`1`, `2`, `3`), this is the normal per-sample bit width.

For complex sample formats (`5`, `6`), this is the sum of the real and imaginary component widths. For example, a complex IEEE 64-bit floating point sample has two IEEE-754 binary64 components, so it is encoded as `(sample_format = 6, bits_per_sample = 128)`.

Valid values are `8`, `16`, `32`, `64`, and `128`. A reader MUST reject any other value. A reader MUST also reject any `(sample_format, bits_per_sample)` pair not listed in the sample encoding table below.

### `sample_format`

The sample type.

- `1` means unsigned integer.
- `2` means signed integer.
- `3` means IEEE floating point.
- `5` means complex signed integer.
- `6` means complex IEEE floating point.

The following sample encodings are valid:

| `sample_format` | `bits_per_sample` | sample encoding |
|---:|---:|---|
| 1 | 8   | unsigned 8-bit integer |
| 1 | 16  | unsigned 16-bit integer |
| 1 | 32  | unsigned 32-bit integer |
| 1 | 64  | unsigned 64-bit integer |
| 2 | 8   | signed 8-bit integer |
| 2 | 16  | signed 16-bit integer |
| 2 | 32  | signed 32-bit integer |
| 2 | 64  | signed 64-bit integer |
| 3 | 16  | IEEE 16-bit floating point |
| 3 | 32  | IEEE 32-bit floating point |
| 3 | 64  | IEEE 64-bit floating point |
| 5 | 32  | complex signed integer, with 16-bit real and 16-bit imaginary components |
| 5 | 64  | complex signed integer, with 32-bit real and 32-bit imaginary components |
| 6 | 32  | complex IEEE floating point, with 16-bit real and 16-bit imaginary components |
| 6 | 64  | complex IEEE floating point, with 32-bit real and 32-bit imaginary components |
| 6 | 128 | complex IEEE floating point, with 64-bit real and 64-bit imaginary components |

A reader MUST reject any `(sample_format, bits_per_sample)` pair not listed above.

The encodings `(sample_format = 3, bits_per_sample = 16)` and `(sample_format = 6, bits_per_sample = 32)` correspond to IEEE 16-bit float and complex IEEE 16-bit float.

### `predictor`

The predictor used before compression.

- `1` means no predictor.
- `2` means horizontal differencing.

The floating point predictor, value `3`, is not compliant with this profile.

When `sample_format` is `3` and `predictor` is `2`, the predictor is applied bitwise. In other words, the stored bytes of each floating point sample are treated as an unsigned integer of the same width, and horizontal differencing is computed on that integer representation. A writer producing a shortcog file with `(sample_format = 3, predictor = 2)` MUST encode the predictor using this bitwise convention. A reader MUST decode it the same way.

When `sample_format` is `5` or `6`, `predictor` MUST be `1`. This profile does not define horizontal differencing across the real and imaginary components of a complex sample. A reader MUST reject any blob where `sample_format` is `5` or `6` and `predictor` is `2`.

### `base_tiles_offset`

The absolute byte offset of the first compressed tile payload in the COG.

This offset points to the first byte that a reader decompresses. It points after the 4-byte COG leader of the first tile, not to the leader itself.

## Tile byte counts

After the 31-byte header, the blob stores `N` little-endian `uint32` values. Each value is the compressed byte size of one tile payload from the source COG. Every value MUST be greater than zero.

Tiles are listed in tile-index order, with samples innermost. For `PlanarConfiguration = 2`, this means all samples of one spatial tile are listed before the next spatial tile. Spatial positions are walked row-major.

The tile index for `(row, col, sample)` is:

```text
idx = (row * tiles_across + col) * samples_per_pixel + sample
```

## Contiguity and offset reconstruction

shortcog stores no explicit tile offsets. A reader reconstructs tile payload offsets by walking `tile_byte_counts` in tile-index order and prefix summing their values.

A COG is compliant only when the reconstructed offsets match the real COG `TileOffsets` in the same order.

The tile index is:

```text
idx = (row * tiles_across + col) * samples_per_pixel + sample
```

Walking `idx` from `0` upward, every tile payload offset MUST satisfy:

```text
offset[0]     = base_tiles_offset
offset[idx+1] = offset[idx] + tile_byte_counts[idx] + 8
```

The validation check MUST be performed in tile-index order. Do not sort offsets first. A `BAND`-interleaved file can still look like one contiguous run if the offsets are sorted, but it fails the shortcog ordering rule because the next payload in file order is not the next payload in tile-index order.

The `+ 8` is the COG tile framing. Each tile payload is wrapped by two ghost fields written by the COG driver:

- a 4-byte leader immediately before the payload, written when `BLOCK_LEADER=SIZE_AS_UINT4`; and
- a 4-byte trailer immediately after the payload, written when `BLOCK_TRAILER=LAST_4_BYTES_REPEATED`.

The leader stores the payload size as a little-endian unsigned 32-bit integer. The trailer repeats the last 4 bytes of the payload.

`base_tiles_offset` and all reconstructed offsets point to tile payloads, not to leaders. A reader therefore reads exactly `tile_byte_counts[idx]` bytes from `offset[idx]` and sends those bytes to the decompressor. Between one payload and the next are the current tile's 4-byte trailer and the next tile's 4-byte leader. That is the 8-byte gap in the reconstruction formula.

A reader MUST use the reconstructed offsets when seeking. A raw cumulative sum of `tile_byte_counts` alone lands at the wrong byte because it ignores the leader/trailer framing.

## Changelog

- **1.0.0** — Initial draft.