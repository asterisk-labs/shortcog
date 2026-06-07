// Round-trip over the fixture corpus. For each manifest entry:
//   accept -> build the blob, open through SHORTCOG, SHA256 each band at its
//             native dtype in band order, compare to sidecar.decoded_sha256.
//   reject -> expect build_blob or the open to fail.
// Dtypes the GDAL build lacks (Float16 before 3.11) are skipped.

#include "shortcog/shortcog.hpp"
#include "sha256.hpp"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

struct GdalDatasetCloser {
    void operator()(GDALDataset* ds) const noexcept { if (ds) GDALClose(ds); }
};
using DatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetCloser>;

// Mute GDAL's handler while we expect failures (negative fixtures).
struct QuietGdalErrors {
    QuietGdalErrors()  { CPLPushErrorHandler(CPLQuietErrorHandler); }
    ~QuietGdalErrors() { CPLPopErrorHandler(); }
};

std::string to_hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string s(bytes.size() * 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        s[i * 2]     = hex[(bytes[i] >> 4) & 0xF];
        s[i * 2 + 1] = hex[ bytes[i]       & 0xF];
    }
    return s;
}

std::string base64_encode(std::span<const std::byte> bytes)
{
    char* b64 = CPLBase64Encode(static_cast<int>(bytes.size()),
                                reinterpret_cast<const unsigned char*>(bytes.data()));
    std::string out(b64);
    CPLFree(b64);
    return out;
}

DatasetPtr open_shortcog(const fs::path& path, const std::string& header_b64)
{
    CPLStringList opts;
    opts.AddNameValue("SHORTCOG_HEADER", header_b64.c_str());
    const char* drivers[] = {"SHORTCOG", nullptr};
    return DatasetPtr(GDALDataset::FromHandle(GDALOpenEx(
        path.string().c_str(),
        GDAL_OF_RASTER | GDAL_OF_READONLY,
        drivers, opts.List(), nullptr)));
}

bool dtype_unavailable_on_this_build(const json& entry)
{
    const auto label = entry.at("dtype").at("label").get<std::string>();
    if (label == "float16")  return GDALGetDataTypeByName("Float16")  == GDT_Unknown;
    if (label == "cfloat16") return GDALGetDataTypeByName("CFloat16") == GDT_Unknown;
    return false;
}

struct Outcome {
    enum class Kind { PASS, FAIL, SKIP };
    Kind        kind = Kind::PASS;
    std::string detail;
};

Outcome run_accept(const fs::path& dir, const json& entry)
{
    const auto path = dir / entry.at("file").get<std::string>();

    if (dtype_unavailable_on_this_build(entry)) {
        return {Outcome::Kind::SKIP, "dtype unavailable in this GDAL build"};
    }

    auto blob = shortcog::build_blob_from_file(path.string().c_str());
    if (!blob) {
        return {Outcome::Kind::FAIL, "build_blob_from_file: " + blob.error()};
    }

    auto ds = open_shortcog(path, base64_encode(*blob));
    if (!ds) {
        return {Outcome::Kind::FAIL, "GDALOpenEx via SHORTCOG returned null"};
    }

    const auto& geom = entry.at("geometry");
    if (ds->GetRasterXSize() != geom.at("width").get<int>() ||
        ds->GetRasterYSize() != geom.at("height").get<int>() ||
        ds->GetRasterCount() != geom.at("bands").get<int>()) {
        return {Outcome::Kind::FAIL, "raster size or band count does not match sidecar"};
    }

    // Band order, native dtype, no conversion: must match the generator.
    SHA256 sha;
    const int xs = ds->GetRasterXSize();
    const int ys = ds->GetRasterYSize();
    for (int b = 1; b <= ds->GetRasterCount(); ++b) {
        GDALRasterBand* band = ds->GetRasterBand(b);
        const GDALDataType dt = band->GetRasterDataType();
        const int dt_size = GDALGetDataTypeSizeBytes(dt);
        if (dt_size <= 0) {
            return {Outcome::Kind::FAIL, "unknown native dtype on band " + std::to_string(b)};
        }
        std::vector<unsigned char> buf(static_cast<std::size_t>(xs) * ys * dt_size);
        if (band->RasterIO(GF_Read, 0, 0, xs, ys,
                           buf.data(), xs, ys, dt, 0, 0, nullptr) != CE_None) {
            return {Outcome::Kind::FAIL, "RasterIO failed for band " + std::to_string(b)};
        }
        sha.update(buf.data(), buf.size());
    }

    const std::string got      = to_hex(sha.finalize());
    const std::string expected = entry.at("decoded_sha256").get<std::string>();
    if (got != expected) {
        return {Outcome::Kind::FAIL, "SHA256 mismatch: got " + got + ", expected " + expected};
    }
    return {Outcome::Kind::PASS, {}};
}

Outcome run_reject(const fs::path& dir, const json& entry)
{
    const auto path = dir / entry.at("file").get<std::string>();

    if (dtype_unavailable_on_this_build(entry)) {
        return {Outcome::Kind::SKIP, "dtype unavailable in this GDAL build"};
    }

    QuietGdalErrors quiet;

    auto blob = shortcog::build_blob_from_file(path.string().c_str());
    if (!blob) {
        return {Outcome::Kind::PASS, "rejected at build_blob"};
    }
    auto ds = open_shortcog(path, base64_encode(*blob));
    if (!ds) {
        return {Outcome::Kind::PASS, "rejected at open"};
    }
    return {Outcome::Kind::FAIL, "expected rejection but build and open both succeeded"};
}

const char* tag(Outcome::Kind k) noexcept
{
    switch (k) {
        case Outcome::Kind::PASS: return "[PASS]";
        case Outcome::Kind::FAIL: return "[FAIL]";
        case Outcome::Kind::SKIP: return "[SKIP]";
    }
    return "[??? ]";
}

int run(const fs::path& dir)
{
    const auto manifest_path = dir / "manifest.json";
    std::ifstream in(manifest_path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", manifest_path.string().c_str());
        return 2;
    }
    json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "manifest.json parse error: %s\n", e.what());
        return 2;
    }

    // build_blob_from_file opens the source COG via GDALOpenEx (GTiff), so
    // the standard drivers must be registered alongside SHORTCOG.
    GDALAllRegister();
    GDALRegister_SHORTCOG();

    int n_pass = 0, n_fail = 0, n_skip = 0;
    for (const auto& entry : manifest.at("fixtures")) {
        const auto file = entry.at("file").get<std::string>();
        const auto exp  = entry.at("expected").get<std::string>();

        Outcome out;
        try {
            out = (exp == "accept") ? run_accept(dir, entry) : run_reject(dir, entry);
        } catch (const std::exception& e) {
            out = {Outcome::Kind::FAIL, std::string("exception: ") + e.what()};
        }

        std::printf("%s  %s", tag(out.kind), file.c_str());
        if (!out.detail.empty()) std::printf("  (%s)", out.detail.c_str());
        std::putchar('\n');

        switch (out.kind) {
            case Outcome::Kind::PASS: ++n_pass; break;
            case Outcome::Kind::FAIL: ++n_fail; break;
            case Outcome::Kind::SKIP: ++n_skip; break;
        }
    }

    if (manifest.contains("failed") && !manifest["failed"].empty()) {
        std::printf("\nGenerator reported %zu failed fixture(s):\n",
                    manifest["failed"].size());
        for (const auto& f : manifest["failed"]) {
            std::printf("  - %s  (%s)\n",
                        f.value("name",  "<unnamed>").c_str(),
                        f.value("error", "<no error>").c_str());
        }
    }

    std::printf("\nSummary: %d passed, %d failed, %d skipped (%d total)\n",
                n_pass, n_fail, n_skip, n_pass + n_fail + n_skip);
    return n_fail == 0 ? 0 : 1;
}

}  // namespace


int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixture-dir>\n",
                     argc > 0 ? argv[0] : "test_fixtures");
        return 2;
    }
    return run(fs::path(argv[1]));
}