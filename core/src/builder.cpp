#include "shortcog/shortcog.hpp"
#include "cpl_string.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace shortcog {
namespace {

// Tight wrapper around snprintf for error messages. The format attribute lets
// the compiler type-check call sites against the format string and silences
// -Wformat-security.
[[gnu::format(printf, 1, 2)]]
std::unexpected<std::string> err(const char* fmt, ...)
{
    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::unexpected(std::string(buf));
}

// shortcog accepts only little-endian BigTIFF
std::expected<void, std::string> check_bigtiff_le(const char* path)
{
    VSILFILE* fp = VSIFOpenL(path, "rb");
    if (!fp) return err("could not open for magic check: %s", path);

    unsigned char magic[4] = {0, 0, 0, 0};
    const std::size_t got = VSIFReadL(magic, 1, sizeof(magic), fp);
    VSIFCloseL(fp);

    if (got != sizeof(magic)) {
        return err("file too short to be a TIFF (read %zu of 4 bytes)", got);
    }
    // 'II' = little-endian, 'MM' = big-endian.
    if (magic[0] != 'I' || magic[1] != 'I') {
        return err("shortcog requires little-endian TIFF (II); file starts "
                   "with 0x%02X 0x%02X", magic[0], magic[1]);
    }
    // Little-endian version word: 42 = classic TIFF, 43 = BigTIFF.
    const unsigned version =
        static_cast<unsigned>(magic[2]) | (static_cast<unsigned>(magic[3]) << 8);
    if (version == 42) {
        return err("shortcog requires BigTIFF; file is classic TIFF "
                   "(rewrite with -co BIGTIFF=YES)");
    }
    if (version != 43) {
        return err("not a TIFF file (bad version word %u)", version);
    }
    return {};
}

// For complex sample_format (5 and 6), bits_per_sample is the sum of real and
// imaginary component widths.
std::expected<std::pair<std::uint8_t, std::uint8_t>, std::string>
sample_encoding(GDALDataType dt) noexcept
{
    using E = std::pair<std::uint8_t, std::uint8_t>;
    switch (dt) {
        case GDT_Byte:     return E{1,   8};
        case GDT_UInt16:   return E{1,  16};
        case GDT_UInt32:   return E{1,  32};
        case GDT_UInt64:   return E{1,  64};
        case GDT_Int8:     return E{2,   8};
        case GDT_Int16:    return E{2,  16};
        case GDT_Int32:    return E{2,  32};
        case GDT_Int64:    return E{2,  64};
        case GDT_Float16:  return E{3,  16};
        case GDT_Float32:  return E{3,  32};
        case GDT_Float64:  return E{3,  64};
        case GDT_CInt16:   return E{5,  32};
        case GDT_CInt32:   return E{5,  64};
        case GDT_CFloat16: return E{6,  32};
        case GDT_CFloat32: return E{6,  64};
        case GDT_CFloat64: return E{6, 128};
        default:
            return err("unsupported GDAL data type: %s", GDALGetDataTypeName(dt));
    }
}

struct DatasetCloser {
    void operator()(GDALDataset* ds) const noexcept { if (ds) GDALClose(ds); }
};
using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

// Walk every (row, col, sample) in physical order, validating contiguity and
// recording each tile's compressed size. Returns the array of byte counts
// indexed the same way Header::tile_index produces.
std::expected<std::vector<std::uint32_t>, std::string>
collect_tile_byte_counts(GDALDataset& ds,
                         std::uint32_t tx, std::uint32_t ty,
                         std::uint16_t spp,
                         std::uint64_t base_offset)
{
    const std::uint64_t n = static_cast<std::uint64_t>(tx) * ty * spp;
    std::vector<std::uint32_t> counts;
    try {
        counts.resize(static_cast<std::size_t>(n));
    } catch (const std::bad_alloc&) {
        return err("allocation failed for %llu tile_byte_counts entries",
                   static_cast<unsigned long long>(n));
    }

    std::uint64_t expected_offset = base_offset;
    char key_size[64], key_off[64];
    for (std::uint32_t row = 0; row < ty; ++row) {
        for (std::uint32_t col = 0; col < tx; ++col) {
            std::snprintf(key_size, sizeof(key_size), "BLOCK_SIZE_%u_%u",   col, row);
            std::snprintf(key_off,  sizeof(key_off),  "BLOCK_OFFSET_%u_%u", col, row);
            for (std::uint16_t b = 0; b < spp; ++b) {
                GDALRasterBand* bd = ds.GetRasterBand(b + 1);
                const char* sz_str  = bd->GetMetadataItem(key_size, "TIFF");
                const char* off_str = bd->GetMetadataItem(key_off,  "TIFF");
                if (!sz_str || !off_str) {
                    return err("tile (%u,%u,%u): TIFF BLOCK_SIZE/BLOCK_OFFSET missing",
                               row, col, b);
                }
                const std::uint64_t sz64  = std::strtoull(sz_str,  nullptr, 10);
                const std::uint64_t off64 = std::strtoull(off_str, nullptr, 10);
                if (sz64 == 0) {
                    return err("tile (%u,%u,%u) has zero byte count", row, col, b);
                }
                if (sz64 > 0xFFFFFFFFu) {
                    return err("tile (%u,%u,%u) byte count %llu exceeds uint32 max",
                               row, col, b, static_cast<unsigned long long>(sz64));
                }
                if (off64 != expected_offset) {
                    return err("tile (%u,%u,%u) not contiguous (expected %llu, got %llu); "
                               "the file is INTERLEAVE!=TILE or lacks the COG "
                               "BLOCK_LEADER/BLOCK_TRAILER framing. Rewrite with the COG "
                               "driver and INTERLEAVE=TILE",
                               row, col, b,
                               static_cast<unsigned long long>(expected_offset),
                               static_cast<unsigned long long>(off64));
                }
                const std::size_t i =
                    (static_cast<std::size_t>(row) * tx + col) * spp + b;
                counts[i] = static_cast<std::uint32_t>(sz64);
                expected_offset = off64 + sz64 + COG_TILE_FRAMING;
            }
        }
    }
    return counts;
}

}  // namespace

std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path) noexcept
{
    if (!path) return err("path is null");

    GDALAllRegister();

    // Profile rules: little-endian byte order and BigTIFF.
    if (auto ok = check_bigtiff_le(path); !ok) {
        return std::unexpected(ok.error());
    }

    DatasetPtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path, GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    if (!ds) return err("could not open: %s", path);

    // A subdataset container is several images in one file, which breaks the
    // single-IFD assumption. Reject before the band-count check so the message
    // is specific.
    if (CSLCount(ds->GetMetadata("SUBDATASETS")) != 0) {
        return err("shortcog rejects multi-image files (found subdatasets)");
    }

    const std::uint32_t iw   = static_cast<std::uint32_t>(ds->GetRasterXSize());
    const std::uint32_t ih   = static_cast<std::uint32_t>(ds->GetRasterYSize());
    const int           rcnt = ds->GetRasterCount();
    if (rcnt <= 0 || rcnt > 65535) {
        return err("invalid band count: %d", rcnt);
    }
    const std::uint16_t spp = static_cast<std::uint16_t>(rcnt);

    // ZSTD-only; any other compression would build a blob fine and then fail
    // at decode.
    const char* comp = ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE");
    if (!comp || std::strcmp(comp, "ZSTD") != 0) {
        return err("shortcog requires COMPRESS=ZSTD; file has COMPRESSION=%s",
                   comp ? comp : "(none)");
    }

    // shortcog needs the INTERLEAVE=TILE ordering, where all samples of a
    // spatial tile sit together. INTERLEAVE=BAND is also PlanarConfig=Separate
    // but stores each sample's tiles in one run, breaking that layout;
    // INTERLEAVE=PIXEL is Contiguous. Both are rejected. The contiguity loop
    // below is the real gate, but checking the tag fails fast with a clear
    // message. For a single sample the orderings coincide, so it is skipped.
    if (spp > 1) {
        const char* il = ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE");
        if (!il || std::strcmp(il, "TILE") != 0) {
            return err("shortcog requires INTERLEAVE=TILE (write with the COG "
                       "driver, e.g. -co INTERLEAVE=TILE); file has "
                       "INTERLEAVE=%s", il ? il : "(unset)");
        }
    }

    GDALRasterBand* band1 = ds->GetRasterBand(1);

    // A per-dataset mask is a separate IFD whose tiles can fall inside the data
    // run, which the prefix-sum reconstruction cannot describe.
    if (band1->GetMaskFlags() & GMF_PER_DATASET) {
        return err("shortcog rejects an internal per-dataset mask "
                   "(rewrite without a mask band)");
    }

    if (band1->GetOverviewCount() != 0) {
        return err("shortcog rejects overviews (rewrite with OVERVIEWS=NONE); found %d level(s)",
                   band1->GetOverviewCount());
    }

    int bxs = 0, bys = 0;
    band1->GetBlockSize(&bxs, &bys);
    if (bxs <= 0 || bys <= 0 || bxs > 65535 || bys > 65535) {
        return err("invalid block size: %dx%d", bxs, bys);
    }

    // shortcog depends on the COG ghost framing. GDAL reports
    // LAYOUT=COG only when that ghost area is present with
    // KNOWN_INCOMPATIBLE_EDITION=NO, which is exactly the structure we need :)
    const char* layout = ds->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE");
    if (!layout || std::strcmp(layout, "COG") != 0) {
        return err("shortcog requires the COG layout with BLOCK_LEADER/"
                   "BLOCK_TRAILER framing; file has LAYOUT=%s. Rewrite with "
                   "the COG driver (gdal_translate -of COG ...)",
                   layout ? layout : "(unset)");
    }

    const std::uint16_t tw = static_cast<std::uint16_t>(bxs);
    const std::uint16_t th = static_cast<std::uint16_t>(bys);
    if (tw > iw || th > ih) {
        return err("tile larger than image: tile=%ux%u, image=%ux%u", tw, th, iw, ih);
    }

    auto enc = sample_encoding(band1->GetRasterDataType());
    if (!enc) return std::unexpected(enc.error());
    const std::uint8_t sf  = enc->first;
    const std::uint8_t bps = enc->second;

    const char* pred_str = ds->GetMetadataItem("PREDICTOR", "IMAGE_STRUCTURE");
    const long pred_l = pred_str ? std::strtol(pred_str, nullptr, 10) : 1;
    if (pred_l != 1 && pred_l != 2) {
        return err("unsupported predictor %ld (spec allows 1 or 2; "
                   "floating-point predictor 3 is out of scope)", pred_l);
    }
    const std::uint8_t predictor = static_cast<std::uint8_t>(pred_l);

    // Differencing across real/imag components of a single stored unit is
    // undefined.
    if (predictor == 2 && (sf == 5 || sf == 6)) {
        return err("predictor=2 is not allowed when sample_format=%d "
                   "(complex samples); re-encode with PREDICTOR=1", sf);
    }

    const char* base_str = band1->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF");
    if (!base_str) return err("BLOCK_OFFSET_0_0 missing; is the file tiled?");
    const std::uint64_t base_offset = std::strtoull(base_str, nullptr, 10);

    const std::uint32_t tx = (iw + tw - 1) / tw;
    const std::uint32_t ty = (ih + th - 1) / th;
    const std::uint64_t n_tiles = static_cast<std::uint64_t>(tx) * ty * spp;
    if (n_tiles > 0xFFFFFFFFu) {
        return err("tile count overflows uint32: %llu",
                   static_cast<unsigned long long>(n_tiles));
    }

    auto counts = collect_tile_byte_counts(*ds, tx, ty, spp, base_offset);
    if (!counts) return std::unexpected(counts.error());

    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = iw;
    bh.image_length      = ih;
    bh.tile_width        = tw;
    bh.tile_length       = th;
    bh.samples_per_pixel = spp;
    bh.bits_per_sample   = bps;
    bh.sample_format     = sf;
    bh.predictor         = predictor;
    bh.base_tiles_offset = base_offset;

    std::vector<std::byte> blob;
    try {
        blob.resize(HEADER_SIZE + counts->size() * sizeof(std::uint32_t));
    } catch (const std::bad_alloc&) {
        return err("allocation failed for output blob");
    }
    std::memcpy(blob.data(), &bh, sizeof(BlobHeader));
    std::memcpy(blob.data() + HEADER_SIZE,
                counts->data(),
                counts->size() * sizeof(std::uint32_t));
    return blob;
}

}  // namespace shortcog