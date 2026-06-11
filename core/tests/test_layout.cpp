// Tests for compile_layout and the read engine. Three parts: compile_layout
// in isolation; real pixels through read_window/read_stack; and the GDAL
// driver vs read_window. The "b y x" read is the oracle; for every other
// layout we recompute each (b,y,x) target with a hand-written index formula,
// so a layout's own strides are never used to check themselves.

#include "shortcog/shortcog.hpp"

#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using shortcog::Header;
using shortcog::LayoutPlan;
using shortcog::compile_layout;

namespace {

// uint16, 4 bands, 40x50, 32px tiles. Edges miss the tile grid, so reads also
// hit the clamping path.
const char* FIXTURE = "geom_uint16_mismatch_multiband_p2.tif";

int g_fail = 0;

void check(bool ok, const char* name)
{
    std::printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) ++g_fail;
}


// compile_layout

bool plan_is(const LayoutPlan& p, std::vector<std::int64_t> shape,
             std::int64_t sn, std::int64_t sb, std::int64_t sy, std::int64_t sx,
             bool native)
{
    return p.shape == shape && p.sn == sn && p.sb == sb &&
           p.sy == sy && p.sx == sx && p.native == native;
}

void test_compile_layout()
{
    // n absent from the pattern -> sn is 0.
    if (auto p = compile_layout("b y x", 1, 3, 4, 5))
        check(plan_is(*p, {3,4,5}, 0, 20, 5, 1, true), "b y x");
    else
        check(false, "b y x");

    // HWC: band innermost.
    if (auto p = compile_layout("y x b", 1, 3, 4, 5))
        check(plan_is(*p, {4,5,3}, 0, 1, 15, 3, false), "y x b");
    else
        check(false, "y x b");

    if (auto p = compile_layout("n b y x", 2, 3, 4, 5))
        check(plan_is(*p, {2,3,4,5}, 60, 20, 5, 1, true), "n b y x");
    else
        check(false, "n b y x");

    // fusing n,b into channels moves nothing, so still native.
    if (auto p = compile_layout("(n b) y x", 2, 3, 4, 5))
        check(plan_is(*p, {6,4,5}, 60, 20, 5, 1, true), "(n b) y x");
    else
        check(false, "(n b) y x");

    if (auto p = compile_layout("n (y x) b", 2, 3, 4, 5))
        check(plan_is(*p, {2,20,3}, 60, 1, 15, 3, false), "n (y x) b");
    else
        check(false, "n (y x) b");

    check(!compile_layout("b y x",    1, 0, 4, 5), "reject zero extent");
    check(!compile_layout("b y",      1, 3, 4, 5), "reject missing axis");
    check(!compile_layout("b y x",    3, 3, 4, 5), "reject n>1 without n");
    check(!compile_layout("b y z",    1, 3, 4, 5), "reject unknown axis");
    check(!compile_layout("b b y x",  1, 3, 4, 5), "reject repeated axis");
    check(!compile_layout("(b (y) x)",1, 3, 4, 5), "reject nested parens");
    check(!compile_layout("(b y x",   1, 3, 4, 5), "reject unbalanced paren");
}


// engine (stateless API)

// Empty vector on failure.
std::vector<std::uint16_t> read_u16(const fs::path& path, const Header& h,
                                    std::span<const int> bands,
                                    int yo, int ys, int xo, int xs,
                                    const LayoutPlan& lay)
{
    std::vector<std::uint16_t> out(bands.size() * static_cast<std::size_t>(ys) * xs);
    auto r = shortcog::read_window(path.string().c_str(), h, bands,
                                   yo, ys, xo, xs, lay,
                                   reinterpret_cast<std::byte*>(out.data()),
                                   /*num_threads=*/1);
    if (!r) {
        std::fprintf(stderr, "read_window: %s\n", r.error().c_str());
        out.clear();
    }
    return out;
}

void test_engine_image(const fs::path& path, const Header& h)
{
    const int B = h.samples_per_pixel;
    const int Y = static_cast<int>(h.image_length);
    const int X = static_cast<int>(h.image_width);

    std::vector<int> all(B);
    for (int i = 0; i < B; ++i) all[i] = i + 1;

    auto ref = read_u16(path, h, all, 0, Y, 0, X, *compile_layout("b y x", 1, B, Y, X));

    // hwc(y,x,b) must equal ref(b,y,x).
    {
        auto hwc = read_u16(path, h, all, 0, Y, 0, X, *compile_layout("y x b", 1, B, Y, X));
        bool ok = !ref.empty() && !hwc.empty();
        for (int b = 0; ok && b < B; ++b)
            for (int y = 0; ok && y < Y; ++y)
                for (int x = 0; x < X; ++x) {
                    const std::size_t r = (static_cast<std::size_t>(b) * Y + y) * X + x;
                    const std::size_t hh = (static_cast<std::size_t>(y) * X + x) * B + b;
                    if (ref[r] != hwc[hh]) { ok = false; break; }
                }
        check(ok, "y x b matches b y x (full image)");
    }

    // window crossing tile borders, bands selected out of order.
    if (B >= 3) {
        const std::vector<int> sel = {B, 2, 1};
        const int nb = static_cast<int>(sel.size());
        const int yo = 8, ys = 20, xo = 5, xs = 30;

        auto rn = read_u16(path, h, sel, yo, ys, xo, xs, *compile_layout("b y x", 1, nb, ys, xs));
        auto hw = read_u16(path, h, sel, yo, ys, xo, xs, *compile_layout("y x b", 1, nb, ys, xs));
        bool ok = !rn.empty() && !hw.empty();
        for (int i = 0; ok && i < nb; ++i)
            for (int y = 0; ok && y < ys; ++y)
                for (int x = 0; x < xs; ++x) {
                    const std::size_t r = (static_cast<std::size_t>(i) * ys + y) * xs + x;
                    const std::size_t hh = (static_cast<std::size_t>(y) * xs + x) * nb + i;
                    if (rn[r] != hw[hh]) { ok = false; break; }
                }
        check(ok, "y x b matches b y x (window + band reorder)");
    }

    // out-of-bounds window and bad band must fail cleanly.
    {
        std::vector<std::uint16_t> out(static_cast<std::size_t>(B) * Y * X);
        auto lay = *compile_layout("b y x", 1, B, Y, X);
        auto r1 = shortcog::read_window(path.string().c_str(), h, all,
                                        0, Y + 1, 0, X, lay,
                                        reinterpret_cast<std::byte*>(out.data()), 1);
        const int bad[] = {B + 1};
        auto r2 = shortcog::read_window(path.string().c_str(), h,
                                        std::span<const int>(bad, 1),
                                        0, Y, 0, X,
                                        *compile_layout("b y x", 1, 1, Y, X),
                                        reinterpret_cast<std::byte*>(out.data()), 1);
        check(!r1 && !r2, "read_window rejects bad window / band");
    }
}

void test_engine_stack(const fs::path& path, const Header& h)
{
    // Same file twice: every n slice must equal the single-image reference.
    const int B = h.samples_per_pixel;
    const int Y = static_cast<int>(h.image_length);
    const int X = static_cast<int>(h.image_width);
    const int N = 2;

    const std::string p = path.string();
    const char* paths[]        = {p.c_str(), p.c_str()};
    const Header* hdrs[]       = {&h, &h};
    const std::vector<int> ni  = {1, 2};

    std::vector<int> bands(B);
    for (int i = 0; i < B; ++i) bands[i] = i + 1;

    auto ref = read_u16(path, h, bands, 0, Y, 0, X, *compile_layout("b y x", 1, B, Y, X));

    auto run = [&](const LayoutPlan& lay, std::vector<std::uint16_t>& out) {
        auto r = shortcog::read_stack(
            std::span<const char* const>(paths, 2),
            std::span<const Header* const>(hdrs, 2),
            ni, bands, 0, Y, 0, X, lay,
            reinterpret_cast<std::byte*>(out.data()), 1);
        if (!r) std::fprintf(stderr, "read_stack: %s\n", r.error().c_str());
        return bool(r);
    };

    {
        std::vector<std::uint16_t> out(static_cast<std::size_t>(N) * B * Y * X);
        bool got = run(*compile_layout("n b y x", N, B, Y, X), out);
        bool ok = got && !ref.empty();
        for (int n = 0; ok && n < N; ++n)
            for (int bb = 0; ok && bb < B; ++bb)
                for (int y = 0; ok && y < Y; ++y)
                    for (int x = 0; x < X; ++x) {
                        const std::size_t o = ((static_cast<std::size_t>(n) * B + bb) * Y + y) * X + x;
                        const std::size_t r = (static_cast<std::size_t>(bb) * Y + y) * X + x;
                        if (out[o] != ref[r]) { ok = false; break; }
                    }
        check(ok, "stack n b y x");
    }

    // out laid out as [N, Y*X, B].
    {
        std::vector<std::uint16_t> out(static_cast<std::size_t>(N) * B * Y * X);
        bool got = run(*compile_layout("n (y x) b", N, B, Y, X), out);
        bool ok = got && !ref.empty();
        for (int n = 0; ok && n < N; ++n)
            for (int bb = 0; ok && bb < B; ++bb)
                for (int y = 0; ok && y < Y; ++y)
                    for (int x = 0; x < X; ++x) {
                        const std::size_t tok = static_cast<std::size_t>(y) * X + x;
                        const std::size_t o = (static_cast<std::size_t>(n) * Y * X + tok) * B + bb;
                        const std::size_t r = (static_cast<std::size_t>(bb) * Y + y) * X + x;
                        if (out[o] != ref[r]) { ok = false; break; }
                    }
        check(ok, "stack n (y x) b");
    }

    // mismatched specs must be rejected: lie about the predictor.
    {
        Header bad = h;
        bad.predictor = (h.predictor == 1) ? 2 : 1;
        const Header* hdrs2[] = {&h, &bad};
        std::vector<std::uint16_t> out(static_cast<std::size_t>(N) * B * Y * X);
        auto r = shortcog::read_stack(
            std::span<const char* const>(paths, 2),
            std::span<const Header* const>(hdrs2, 2),
            ni, bands, 0, Y, 0, X,
            *compile_layout("n b y x", N, B, Y, X),
            reinterpret_cast<std::byte*>(out.data()), 1);
        check(!r, "stack rejects mismatched specs");
    }
}


// driver interop

std::string b64(std::span<const std::byte> bytes)
{
    char* s = CPLBase64Encode(static_cast<int>(bytes.size()),
                              reinterpret_cast<const unsigned char*>(bytes.data()));
    std::string out(s);
    CPLFree(s);
    return out;
}

void test_driver_interop(const fs::path& path, const std::string& header,
                         const Header& h)
{
    CPLStringList opts;
    opts.AddNameValue("SHORTCOG_HEADER", header.c_str());
    const char* drv[] = {"SHORTCOG", nullptr};
    auto* ds = GDALDataset::FromHandle(GDALOpenEx(
        path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
        drv, opts.List(), nullptr));
    if (!ds) { check(false, "driver open"); return; }
    std::unique_ptr<GDALDataset, void(*)(GDALDataset*)> guard(
        ds, [](GDALDataset* d) { GDALClose(d); });

    const int B = ds->GetRasterCount();
    const int Y = ds->GetRasterYSize();
    const int X = ds->GetRasterXSize();
    check(B == h.samples_per_pixel &&
          Y == static_cast<int>(h.image_length) &&
          X == static_cast<int>(h.image_width), "driver dimensions");

    std::vector<int> all(B);
    for (int i = 0; i < B; ++i) all[i] = i + 1;

    auto ref = read_u16(path, h, all, 0, Y, 0, X, *compile_layout("b y x", 1, B, Y, X));

    std::vector<std::uint16_t> via(static_cast<std::size_t>(B) * Y * X);
    CPLErr e = ds->RasterIO(GF_Read, 0, 0, X, Y, via.data(), X, Y,
                            GDT_UInt16, B, all.data(), 0, 0, 0, nullptr);
    check(e == CE_None && !ref.empty() &&
          std::memcmp(via.data(), ref.data(),
                      via.size() * sizeof(std::uint16_t)) == 0,
          "driver RasterIO matches read_window");
}

}  // namespace


int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixture-dir>\n",
                     argc > 0 ? argv[0] : "test_layout");
        return 2;
    }
    const fs::path path = fs::path(argv[1]) / FIXTURE;

    GDALAllRegister();
    GDALRegister_SHORTCOG();

    test_compile_layout();

    auto blob = shortcog::build_blob_from_file(path.string().c_str());
    if (!blob) {
        std::fprintf(stderr, "build_blob failed for %s: %s\n",
                     path.string().c_str(), blob.error().c_str());
        return 1;
    }

    auto parsed = shortcog::parse_blob(*blob);
    if (!parsed) {
        std::fprintf(stderr, "parse_blob failed: %.*s\n",
                     static_cast<int>(shortcog::describe(parsed.error()).size()),
                     shortcog::describe(parsed.error()).data());
        return 1;
    }
    const Header& h = *parsed;

    test_engine_image(path, h);
    test_engine_stack(path, h);
    test_driver_interop(path, b64(*blob), h);

    std::printf("\nSummary: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}