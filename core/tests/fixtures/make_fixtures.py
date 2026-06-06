#!/usr/bin/env python3
"""Generate the shortcog test-fixture corpus.

    A. dtype x predictor, canonical geometry   (accept)
    B. alignment variants, uint16              (accept)
    C. predictor=2 + complex                   (reject)

accept sidecars carry decoded_sha256, reject sidecars carry reject_reason.
The C++ test branches on `expected`.

    python core/tests/fixtures/make_fixtures.py --out core/tests/fixtures/data
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from osgeo import gdal
from pydantic import BaseModel, ConfigDict

gdal.UseExceptions()


class DTypeSpec(BaseModel):
    model_config = ConfigDict(frozen=True, arbitrary_types_allowed=True)

    label: str
    sample_format: int
    bits_per_sample: int
    gdal_type: int
    np_component: np.dtype
    is_complex: bool
    is_float: bool


def _build_dtype_table() -> list[DTypeSpec]:
    table = [
        DTypeSpec(label="uint8",    sample_format=1, bits_per_sample=8,  gdal_type=gdal.GDT_Byte,     np_component=np.dtype("u1"), is_complex=False, is_float=False),
        DTypeSpec(label="uint16",   sample_format=1, bits_per_sample=16, gdal_type=gdal.GDT_UInt16,   np_component=np.dtype("u2"), is_complex=False, is_float=False),
        DTypeSpec(label="uint32",   sample_format=1, bits_per_sample=32, gdal_type=gdal.GDT_UInt32,   np_component=np.dtype("u4"), is_complex=False, is_float=False),
        DTypeSpec(label="uint64",   sample_format=1, bits_per_sample=64, gdal_type=gdal.GDT_UInt64,   np_component=np.dtype("u8"), is_complex=False, is_float=False),
        DTypeSpec(label="int8",     sample_format=2, bits_per_sample=8,  gdal_type=gdal.GDT_Int8,     np_component=np.dtype("i1"), is_complex=False, is_float=False),
        DTypeSpec(label="int16",    sample_format=2, bits_per_sample=16, gdal_type=gdal.GDT_Int16,    np_component=np.dtype("i2"), is_complex=False, is_float=False),
        DTypeSpec(label="int32",    sample_format=2, bits_per_sample=32, gdal_type=gdal.GDT_Int32,    np_component=np.dtype("i4"), is_complex=False, is_float=False),
        DTypeSpec(label="int64",    sample_format=2, bits_per_sample=64, gdal_type=gdal.GDT_Int64,    np_component=np.dtype("i8"), is_complex=False, is_float=False),
        DTypeSpec(label="float32",  sample_format=3, bits_per_sample=32, gdal_type=gdal.GDT_Float32,  np_component=np.dtype("f4"), is_complex=False, is_float=True),
        DTypeSpec(label="float64",  sample_format=3, bits_per_sample=64, gdal_type=gdal.GDT_Float64,  np_component=np.dtype("f8"), is_complex=False, is_float=True),
        DTypeSpec(label="cint16",   sample_format=5, bits_per_sample=16, gdal_type=gdal.GDT_CInt16,   np_component=np.dtype("i2"), is_complex=True,  is_float=False),
        DTypeSpec(label="cint32",   sample_format=5, bits_per_sample=32, gdal_type=gdal.GDT_CInt32,   np_component=np.dtype("i4"), is_complex=True,  is_float=False),
        DTypeSpec(label="cfloat32", sample_format=6, bits_per_sample=32, gdal_type=gdal.GDT_CFloat32, np_component=np.dtype("f4"), is_complex=True,  is_float=True),
        DTypeSpec(label="cfloat64", sample_format=6, bits_per_sample=64, gdal_type=gdal.GDT_CFloat64, np_component=np.dtype("f8"), is_complex=True,  is_float=True),
    ]
    # RFC 100 half-precision, present only if GDAL was built with it.
    if hasattr(gdal, "GDT_Float16"):
        table.insert(8, DTypeSpec(label="float16", sample_format=3, bits_per_sample=16,
                                  gdal_type=gdal.GDT_Float16, np_component=np.dtype("f2"),
                                  is_complex=False, is_float=True))
    if hasattr(gdal, "GDT_CFloat16"):
        table.append(DTypeSpec(label="cfloat16", sample_format=6, bits_per_sample=16,
                               gdal_type=gdal.GDT_CFloat16, np_component=np.dtype("f2"),
                               is_complex=True, is_float=True))
    return table


DTYPES = _build_dtype_table()
DTYPES_BY_LABEL = {d.label: d for d in DTYPES}


class Geometry(BaseModel):
    model_config = ConfigDict(frozen=True)

    label: str
    height: int
    width: int
    tile: int
    bands: int


# match: dims are a multiple of the tile. mismatch: the last tile row/column
# overlaps the boundary, which is what read_tile_aligned has to clamp.
CANONICAL_GEOM = Geometry(label="match_2x2", height=256, width=256, tile=128, bands=1)

GEOMETRIES = [
    Geometry(label="match_1x1",          height=128, width=128, tile=128, bands=1),
    Geometry(label="mismatch_2x2",       height=200, width=150, tile=128, bands=1),
    Geometry(label="mismatch_multiband", height=200, width=150, tile=128, bands=4),
]


def _smooth_field(h, w, band_idx, rng):
    # Structured field so ZSTD and predictor=2 have something to compress.
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    sx = max(64.0, w / 4.0)
    sy = max(64.0, h / 4.0)
    f = (
        0.5
        + 0.30 * np.sin(2.0 * np.pi * xx / sx) * np.cos(2.0 * np.pi * yy / sy)
        + 0.10 * np.sin(2.0 * np.pi * (xx + yy) / (sx + sy))
        + 0.05 * band_idx
    )
    f += rng.standard_normal(f.shape) * 1e-3
    return np.clip(f, 0.0, 1.0)


def _quantize(field01, dtype, rng):
    kind = dtype.kind
    if kind == "u":
        info = np.iinfo(dtype)
        amp = max(1, info.max // 1024)
        noise = rng.integers(-amp, amp + 1, size=field01.shape)
        return np.clip(field01 * float(info.max) + noise, info.min, info.max).astype(dtype)
    if kind == "i":
        info = np.iinfo(dtype)
        amp_range = min(info.max, -info.min - 1)
        amp = max(1, amp_range // 1024)
        noise = rng.integers(-amp, amp + 1, size=field01.shape)
        scaled = (field01 - 0.5) * 2.0 * float(amp_range)
        return np.clip(scaled + noise, info.min, info.max).astype(dtype)
    if kind == "f":
        return field01.astype(dtype)
    raise ValueError(f"unsupported component dtype kind {kind!r}")


def synth_band_buffer(geom, band_idx, spec, rng):
    h, w = geom.height, geom.width
    if spec.is_complex:
        # numpy->CInt16 narrowing has varied across GDAL releases, so write
        # the two components as raw little-endian bytes.
        real = _quantize(_smooth_field(h, w, band_idx, rng),      spec.np_component, rng)
        imag = _quantize(_smooth_field(h, w, band_idx + 17, rng), spec.np_component, rng)
        dt = np.dtype([("re", spec.np_component.newbyteorder("<")),
                       ("im", spec.np_component.newbyteorder("<"))])
        out = np.empty((h, w), dtype=dt)
        out["re"] = real
        out["im"] = imag
        return out, spec.gdal_type
    arr = _quantize(_smooth_field(h, w, band_idx, rng), spec.np_component, rng)
    return arr, spec.gdal_type


def _mem_dataset(geom, spec):
    drv = gdal.GetDriverByName("MEM")
    ds = drv.Create("", geom.width, geom.height, geom.bands, spec.gdal_type)
    if ds is None:
        raise RuntimeError("MEM Create failed")
    # Seed from fixture identity; changing this rebuilds every SHA.
    seed = int.from_bytes(
        hashlib.sha256(f"{geom.label}|{spec.label}|{geom.bands}".encode()).digest()[:8],
        "little",
    )
    rng = np.random.default_rng(seed)
    for b in range(geom.bands):
        band = ds.GetRasterBand(b + 1)
        buf, buf_type = synth_band_buffer(geom, b, spec, rng)
        if spec.is_complex:
            band.WriteRaster(0, 0, geom.width, geom.height, buf.tobytes(), buf_type=buf_type)
        else:
            band.WriteArray(buf)
    return ds


# COG driver predictor vocabulary, not the GTiff integers.
_COG_PREDICTOR = {1: "NO", 2: "STANDARD", 3: "FLOATING_POINT"}


def write_cog(out_path, geom, spec, predictor):
    src = _mem_dataset(geom, spec)
    opts = gdal.TranslateOptions(
        format="COG",
        creationOptions=[
            "COMPRESS=ZSTD",
            "INTERLEAVE=TILE",
            f"BLOCKSIZE={geom.tile}",
            f"PREDICTOR={_COG_PREDICTOR[predictor]}",
            "BIGTIFF=YES",
            "OVERVIEWS=NONE",
            "LEVEL=9",
        ],
    )
    out = gdal.Translate(str(out_path), src, options=opts)
    if out is None:
        raise RuntimeError(f"COG Translate failed for {out_path}")
    out.FlushCache()


def decoded_sha(path):
    # Native bytes per band in band order, no conversion. The C++ test must
    # read with buf_type = band.DataType for the hash to match.
    ds = gdal.Open(str(path), gdal.GA_ReadOnly)
    h = hashlib.sha256()
    for b in range(1, ds.RasterCount + 1):
        band = ds.GetRasterBand(b)
        h.update(band.ReadRaster(0, 0, ds.RasterXSize, ds.RasterYSize,
                                 ds.RasterXSize, ds.RasterYSize, band.DataType))
    return h.hexdigest()


class Job(BaseModel):
    model_config = ConfigDict(frozen=True)

    name: str
    geom: Geometry
    spec: DTypeSpec
    predictor: int
    expected: str                       # "accept" | "reject"
    reject_reason: str | None = None


def write_sidecar(out_path, job, sha):
    meta = {
        "file": out_path.name,
        "geometry": job.geom.model_dump(),
        "dtype": {
            "label": job.spec.label,
            "sample_format": job.spec.sample_format,
            "bits_per_sample": job.spec.bits_per_sample,
        },
        "predictor": job.predictor,
        "expected": job.expected,
    }
    if job.expected == "accept":
        meta["decoded_sha256"] = sha
    else:
        meta["reject_reason"] = job.reject_reason
    out_path.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def fixture_jobs(args):
    jobs: list[Job] = []

    # A: dtype x predictor on the canonical geometry. Integers take both
    # predictors; float and complex take predictor=1 only.
    for spec in DTYPES:
        if args.dtype and spec.label != args.dtype:
            continue
        predictors = [1] if (spec.is_complex or spec.is_float) else [1, 2]
        for pred in predictors:
            jobs.append(Job(
                name=f"dtype_{spec.label}_{CANONICAL_GEOM.label}_p{pred}",
                geom=CANONICAL_GEOM, spec=spec, predictor=pred, expected="accept",
            ))

    # B: alignment variants on uint16, predictor=2.
    if not args.dtype or args.dtype == "uint16":
        u16 = DTYPES_BY_LABEL["uint16"]
        for geom in GEOMETRIES:
            if args.only and args.only not in geom.label:
                continue
            jobs.append(Job(
                name=f"geom_uint16_{geom.label}_p2",
                geom=geom, spec=u16, predictor=2, expected="accept",
            ))

    # C: predictor=2 + complex. GDAL writes these with PREDICTOR=2 in
    # IMAGE_STRUCTURE, but the profile rejects them at parse time. cfloat64+p2
    # is absent because libtiff refuses a 128-bit-sample predictor=2.
    reason = ("predictor=2 with complex sample_format (5 or 6) "
              "is forbidden by the shortcog profile")
    for spec in DTYPES:
        if not spec.is_complex or spec.label == "cfloat64":
            continue
        if args.dtype and spec.label != args.dtype:
            continue
        jobs.append(Job(
            name=f"neg_complex_p2_{spec.label}",
            geom=CANONICAL_GEOM, spec=spec, predictor=2,
            expected="reject", reject_reason=reason,
        ))

    return jobs


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, default=Path("tests/fixtures/data"))
    p.add_argument("--dtype", type=str, default=None)
    p.add_argument("--only", type=str, default=None)
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    jobs = fixture_jobs(args)
    print(f"planning {len(jobs)} fixture(s) -> {args.out}", file=sys.stderr)

    if args.dry_run:
        for job in jobs:
            print(f"  {job.name}  {job.geom.height}x{job.geom.width}x"
                  f"{job.geom.bands} tile={job.geom.tile} "
                  f"{job.spec.label} pred={job.predictor}  [{job.expected}]")
        return 0

    manifest: list[dict] = []
    failed: list[dict] = []
    for job in jobs:
        path = args.out / f"{job.name}.tif"
        print(f"  writing {path.name}  [{job.expected}] ...", file=sys.stderr)
        try:
            write_cog(path, job.geom, job.spec, job.predictor)
            sha = decoded_sha(path) if job.expected == "accept" else None
            manifest.append(write_sidecar(path, job, sha))
        except Exception as exc:
            print(f"    FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
            failed.append({
                "name": job.name,
                "dtype": job.spec.label,
                "predictor": job.predictor,
                "expected": job.expected,
                "error": f"{type(exc).__name__}: {exc}",
            })
            for f in (path, path.with_suffix(".json")):
                if f.exists():
                    f.unlink()

    (args.out / "manifest.json").write_text(
        json.dumps({"fixtures": manifest, "failed": failed}, indent=2) + "\n"
    )
    n_accept = sum(1 for m in manifest if m["expected"] == "accept")
    n_reject = sum(1 for m in manifest if m["expected"] == "reject")
    print(f"done: {n_accept} accept + {n_reject} reject, {len(failed)} failed",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())