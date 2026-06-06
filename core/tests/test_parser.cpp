// parse_blob tests. Each case mutates one field of a good blob and checks the
// driver logs the right describe() text. Blobs are in memory; the /vsimem file
// is just a placeholder since parse runs before the file is read.
//
// offsets: 4 version, 6 width, 14 tile_w, 16 tile_h, 18 spp, 20 bps,
//          21 sample_format, 22 predictor, 23 base_offset.

#include "shortcog/shortcog.hpp"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Accumulating handler so a substring search still works when GDAL appends
// its own "open failed" noise after ours.
thread_local std::string g_errors_log;

void CPL_STDCALL capture_error_handler(CPLErr, CPLErrorNum, const char* msg)
{
    if (msg) {
        g_errors_log += msg;
        g_errors_log.push_back('\n');
    }
}

struct CaptureErrors {
    CaptureErrors()  { g_errors_log.clear(); CPLPushErrorHandler(capture_error_handler); }
    ~CaptureErrors() { CPLPopErrorHandler(); }
};

// 256x256 image, 128x128 tiles, single band uint8, no predictor. Parses
// cleanly; every negative case mutates one field of this.
std::vector<std::byte> make_baseline_blob()
{
    using namespace shortcog;
    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = 256;
    bh.image_length      = 256;
    bh.tile_width        = 128;
    bh.tile_length       = 128;
    bh.samples_per_pixel = 1;
    bh.bits_per_sample   = 8;
    bh.sample_format     = 1;
    bh.predictor         = 1;
    bh.base_tiles_offset = 100000;

    const std::uint32_t counts[] = {1234, 1235, 1236, 1237};  // 2x2 tiles

    std::vector<std::byte> blob(HEADER_SIZE + sizeof(counts));
    std::memcpy(blob.data(),               &bh,    sizeof(bh));
    std::memcpy(blob.data() + HEADER_SIZE, counts, sizeof(counts));
    return blob;
}

std::vector<std::byte> with_byte(std::vector<std::byte> blob,
                                 std::size_t offset, std::uint8_t value)
{
    blob.at(offset) = std::byte{value};
    return blob;
}

template <typename T>
std::vector<std::byte> with_field(std::vector<std::byte> blob,
                                   std::size_t offset, T value)
{
    std::memcpy(blob.data() + offset, &value, sizeof(T));
    return blob;
}

std::string base64_encode(const std::vector<std::byte>& blob)
{
    char* b64 = CPLBase64Encode(static_cast<int>(blob.size()),
                                reinterpret_cast<const unsigned char*>(blob.data()));
    std::string out(b64);
    CPLFree(b64);
    return out;
}

constexpr const char* DUMMY_PATH = "/vsimem/test_parser_dummy.tif";

struct OpenResult {
    GDALDataset* ds = nullptr;
    std::string  log;
};

OpenResult try_open(const std::vector<std::byte>& blob)
{
    CaptureErrors cap;
    const std::string header_b64 = base64_encode(blob);
    CPLStringList opts;
    opts.AddNameValue("SHORTCOG_HEADER", header_b64.c_str());
    const char* drivers[] = {"SHORTCOG", nullptr};

    OpenResult r;
    r.ds  = static_cast<GDALDataset*>(GDALOpenEx(
        DUMMY_PATH, GDAL_OF_RASTER | GDAL_OF_READONLY,
        drivers, opts.List(), nullptr));
    r.log = g_errors_log;
    return r;
}

struct Case {
    const char*            name;
    std::vector<std::byte> blob;
    const char*            expected_substring;  // from describe(ParseError)
};

bool run_case(const Case& c)
{
    auto r = try_open(c.blob);
    if (r.ds) {
        GDALClose(r.ds);
        std::fprintf(stderr,
            "[FAIL] %s\n  open succeeded, blob was not rejected\n", c.name);
        return false;
    }
    if (r.log.find(c.expected_substring) == std::string::npos) {
        std::fprintf(stderr,
            "[FAIL] %s\n  expected substring: %s\n  captured log:       %s",
            c.name, c.expected_substring, r.log.c_str());
        return false;
    }
    std::printf("[PASS] %s\n", c.name);
    return true;
}

bool run_baseline_sanity(const std::vector<std::byte>& baseline)
{
    auto r = try_open(baseline);
    if (r.ds) GDALClose(r.ds);
    // The driver logs "SHORTCOG blob invalid" only when parse_blob rejects.
    // A clean parse may still fail to open the /vsimem dummy on some builds;
    // that's fine here, we only assert parse_blob accepted the baseline.
    if (r.log.find("SHORTCOG blob invalid") != std::string::npos) {
        std::fprintf(stderr,
            "[FAIL] baseline_parses_cleanly\n  parse_blob rejected the baseline:\n  %s",
            r.log.c_str());
        return false;
    }
    std::printf("[PASS] baseline_parses_cleanly\n");
    return true;
}

int run()
{
    GDALAllRegister();
    GDALRegister_SHORTCOG();

    const GByte dummy = 0;
    VSIFCloseL(VSIFileFromMemBuffer(DUMMY_PATH, const_cast<GByte*>(&dummy), 1, false));

    const auto baseline = make_baseline_blob();
    int failures = 0;
    if (!run_baseline_sanity(baseline)) ++failures;

    std::vector<Case> cases;

    cases.push_back({"blob_too_short",
        std::vector<std::byte>(shortcog::HEADER_SIZE - 1, std::byte{0}),
        "blob shorter than header"});

    cases.push_back({"bad_magic",
        with_field<std::uint32_t>(baseline, 0, 0xDEADBEEFu),
        "bad magic"});

    cases.push_back({"unsupported_version",
        with_field<std::uint16_t>(baseline, 4, 99),
        "unsupported version"});

    cases.push_back({"invalid_bits_per_sample_24",
        with_byte(baseline, 20, 24),
        "invalid bits per sample"});

    cases.push_back({"invalid_sample_format_4",
        with_byte(baseline, 21, 4),
        "invalid sample format"});

    cases.push_back({"invalid_predictor_3",
        with_byte(baseline, 22, 3),
        "invalid predictor"});

    {
        // predictor=2 on a complex sample_format is forbidden by the profile.
        auto b = baseline;
        b[20] = std::byte{32};  // bits_per_sample = 32 (cint16)
        b[21] = std::byte{5};   // sample_format = complex int
        b[22] = std::byte{2};   // predictor = horizontal
        cases.push_back({"predictor_2_with_complex_sf5", b, "invalid predictor"});
    }

    cases.push_back({"invalid_dimensions_zero_width",
        with_field<std::uint32_t>(baseline, 6, 0u),
        "invalid dimensions"});

    cases.push_back({"invalid_dimensions_zero_samples_per_pixel",
        with_field<std::uint16_t>(baseline, 18, std::uint16_t{0}),
        "invalid dimensions"});

    {
        // 512x512 tile against a 256x256 image.
        auto b = baseline;
        const std::uint16_t big = 512;
        std::memcpy(b.data() + 14, &big, 2);
        std::memcpy(b.data() + 16, &big, 2);
        cases.push_back({"tile_larger_than_image", b, "tile dimensions exceed image"});
    }

    {
        // Trailing array half its expected length.
        auto b = baseline;
        b.resize(shortcog::HEADER_SIZE + 8);
        cases.push_back({"blob_size_mismatch_short", b, "blob size does not match"});
    }

    {
        // First tile_byte_count = 0.
        auto b = baseline;
        const std::uint32_t zero = 0;
        std::memcpy(b.data() + shortcog::HEADER_SIZE, &zero, 4);
        cases.push_back({"non_positive_tile_byte_count", b, "tile byte count is zero"});
    }

    for (const auto& c : cases) {
        if (!run_case(c)) ++failures;
    }

    VSIUnlink(DUMMY_PATH);

    const std::size_t total = cases.size() + 1;
    std::printf("\nSummary: %zu test(s), %d failure(s)\n", total, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace


int main(int argc, char**)
{
    if (argc != 1) {
        std::fprintf(stderr, "usage: test_parser (no arguments)\n");
        return 2;
    }
    return run();
}