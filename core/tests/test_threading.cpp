// Thread-safety tests for the SHORTCOG driver:
//   threaded_open_matches_baseline  open with NUM_THREADS=4, read, check SHA.
//   concurrent_reads                one dataset, N reader threads, all match.
// Runs on the multi-band mismatch fixture since it hits the most code paths.

#include "shortcog/shortcog.hpp"
#include "sha256.hpp"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

constexpr const char* STRESS_FIXTURE = "geom_uint16_mismatch_multiband_p2.tif";
constexpr int         POOL_THREADS   = 4;
constexpr int         STRESS_READERS = 8;

struct GdalDatasetCloser {
    void operator()(GDALDataset* ds) const noexcept { if (ds) GDALClose(ds); }
};
using DatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetCloser>;

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

DatasetPtr open_shortcog(const fs::path& path, const std::string& header_b64, int num_threads)
{
    CPLStringList opts;
    opts.AddNameValue("SHORTCOG_HEADER", header_b64.c_str());
    opts.AddNameValue("NUM_THREADS", std::to_string(num_threads).c_str());
    const char* drivers[] = {"SHORTCOG", nullptr};
    return DatasetPtr(GDALDataset::FromHandle(GDALOpenEx(
        path.string().c_str(),
        GDAL_OF_RASTER | GDAL_OF_READONLY,
        drivers, opts.List(), nullptr)));
}

// Read every band at native dtype, hash in band order. Zero array on failure.
std::array<std::uint8_t, 32> hash_full_image(GDALDataset* ds, const char* tag)
{
    SHA256 sha;
    const int xs = ds->GetRasterXSize();
    const int ys = ds->GetRasterYSize();
    for (int b = 1; b <= ds->GetRasterCount(); ++b) {
        GDALRasterBand* band = ds->GetRasterBand(b);
        const GDALDataType dt = band->GetRasterDataType();
        const int dt_size = GDALGetDataTypeSizeBytes(dt);
        std::vector<unsigned char> buf(static_cast<std::size_t>(xs) * ys * dt_size);
        if (band->RasterIO(GF_Read, 0, 0, xs, ys,
                           buf.data(), xs, ys, dt, 0, 0, nullptr) != CE_None) {
            std::fprintf(stderr, "[%s] RasterIO failed for band %d\n", tag, b);
            return {};
        }
        sha.update(buf.data(), buf.size());
    }
    return sha.finalize();
}

bool test_threaded_open_matches_baseline(const fs::path& path, const std::string& expected_sha)
{
    auto blob = shortcog::build_blob_from_file(path.string().c_str());
    if (!blob) {
        std::fprintf(stderr, "build_blob failed for %s: %s\n",
                     path.string().c_str(), blob.error().c_str());
        return false;
    }
    auto ds = open_shortcog(path, base64_encode(*blob), POOL_THREADS);
    if (!ds) {
        std::fprintf(stderr, "open_shortcog (NUM_THREADS=%d) failed\n", POOL_THREADS);
        return false;
    }

    const auto digest = hash_full_image(ds.get(), "threaded");
    if (digest == std::array<std::uint8_t, 32>{}) return false;

    const std::string got = to_hex(digest);
    if (got != expected_sha) {
        std::fprintf(stderr, "threaded read wrong bytes:\n  got      = %s\n  expected = %s\n",
                     got.c_str(), expected_sha.c_str());
        return false;
    }
    return true;
}

bool test_concurrent_reads(const fs::path& path, const std::string& expected_sha)
{
    auto blob = shortcog::build_blob_from_file(path.string().c_str());
    if (!blob) {
        std::fprintf(stderr, "build_blob failed for %s: %s\n",
                     path.string().c_str(), blob.error().c_str());
        return false;
    }
    // One dataset, hit from N readers at once. They share the VSILFILE
    // (lockless PRead) and Band's block-cache mutex; decompression scratch is
    // thread_local per worker. This is the GDAL_OF_THREAD_SAFE contract.
    auto ds = open_shortcog(path, base64_encode(*blob), POOL_THREADS);
    if (!ds) {
        std::fprintf(stderr, "open_shortcog (NUM_THREADS=%d) failed\n", POOL_THREADS);
        return false;
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    readers.reserve(STRESS_READERS);

    for (int t = 0; t < STRESS_READERS; ++t) {
        readers.emplace_back([&, t] {
            const std::string tag = "reader-" + std::to_string(t);
            const auto digest = hash_full_image(ds.get(), tag.c_str());
            if (digest == std::array<std::uint8_t, 32>{}) {
                ++failures;
                return;
            }
            if (to_hex(digest) != expected_sha) {
                std::fprintf(stderr, "[%s] wrong bytes\n", tag.c_str());
                ++failures;
            }
        });
    }
    for (auto& t : readers) t.join();

    return failures.load() == 0;
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

    std::string expected_sha;
    for (const auto& entry : manifest.at("fixtures")) {
        if (entry.value("file", "") == STRESS_FIXTURE &&
            entry.value("expected", "") == "accept") {
            expected_sha = entry.at("decoded_sha256").get<std::string>();
            break;
        }
    }
    if (expected_sha.empty()) {
        std::fprintf(stderr,
            "fixture %s not found in manifest (or not 'accept'); regenerate the corpus\n",
            STRESS_FIXTURE);
        return 2;
    }

    GDALAllRegister();
    GDALRegister_SHORTCOG();

    const fs::path fx_path = dir / STRESS_FIXTURE;
    int failures = 0;

    std::printf("[ RUN ] threaded_open_matches_baseline (NUM_THREADS=%d)\n", POOL_THREADS);
    if (test_threaded_open_matches_baseline(fx_path, expected_sha)) {
        std::printf("[PASS] threaded_open_matches_baseline\n");
    } else {
        std::printf("[FAIL] threaded_open_matches_baseline\n");
        ++failures;
    }

    std::printf("[ RUN ] concurrent_reads (%d readers, NUM_THREADS=%d)\n",
                STRESS_READERS, POOL_THREADS);
    if (test_concurrent_reads(fx_path, expected_sha)) {
        std::printf("[PASS] concurrent_reads\n");
    } else {
        std::printf("[FAIL] concurrent_reads\n");
        ++failures;
    }

    std::printf("\nSummary: 2 test(s), %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace


int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixture-dir>\n",
                     argc > 0 ? argv[0] : "test_threading");
        return 2;
    }
    return run(fs::path(argv[1]));
}