# shortcog header blob v1

Work in progress.

## Layout

The blob is a packed binary struct followed by a `uint32` array of per tile byte counts.

```
[ Header ] [ tile_byte_counts[tile_count] ]
```

Header fields are little endian.

## Invariants

- `magic == 0x474F4353` ('SCOG')
- `version == 1`
- `tile_count == tiles_across * tiles_down * samples_per_pixel`
- every `tile_byte_counts[i]` is greater than zero
- tile offsets are reconstructed as `base_tiles_offset + cumsum(tile_byte_counts[i] + 8)`, accounting for the COG leader and trailer
- the resulting offsets are strictly increasing

## Profile constraints on the source TIFF

- BigTIFF
- tiled
- `PlanarConfiguration = Separate`
- single IFD
- no overviews
- ZSTD compression
- predictor 1 or 2
- tiles strictly sequential in file
