// Tests for compile_layout and the read engine.
//
// Two parts. The first checks compile_layout in isolation: a pattern plus
// extents must give the strides, shape and native flag we expect, and bad
// patterns must be rejected. The second reads real pixels through the driver
// and checks placement. The reference is a native "b y x" read; for every
// other layout we recompute where each (b,y,x) element should land with an
// index formula written out by hand, so the layout's own strides are never
// used as their own oracle.

#include "shortcog/shortcog.hpp"

#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using shortcog::LayoutPlan;
using shortcog::compile_layout;

namespace {

// The multi-band fixture: uint16, 4 bands, 40x50, 32px tiles. Edges don't
// line up with the tile grid, so reads hit the clamping path too.
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
    // single image, plain C-contiguous. n isn't in the pattern, so sn is 0.
    if (auto p = compile_layout("b y x", 1, 3, 4, 5))
        check(plan_is(*p, {3,4,5}, 0, 20, 5, 1, true), "b y x");
    else
        check(false, "b y x");

    // HWC: band innermost.
    if (auto p = compile_layout("y x b", 1, 3, 4, 5))
        check(plan_is(*p, {4,5,3}, 0, 1, 15, 3, false), "y x b");
    else
        check(false, "y x b");

    // canonical cube.
    if (auto p = compile_layout("n b y x", 2, 3, 4, 5))
        check(plan_is(*p, {2,3,4,5}, 60, 20, 5, 1, true), "n b y x");
    else
        check(false, "n b y x");

    // fusing n and b into channels doesn't move anything, so still native.
    if (auto p = compile_layout("(n b) y x", 2, 3, 4, 5))
        check(plan_is(*p, {6,4,5}, 60, 20, 5, 1, true), "(n b) y x");
    else
        check(false, "(n b) y x");

    // tokens per layer.
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


// engine

struct DsCloser {
    void operator()(GDALDataset* d) const noexcept { if (d) GDALClose(d); }
};
using DsPtr = std::unique_ptr<GDALDataset, DsCloser>;

std::string b64(std::span<const std::byte> bytes)
{
    char* s = CPLBase64Encode(static_cast<int>(bytes.size()),
                              reinterpret_cast<const unsigned char*>(bytes.data()));
    std::string out(s);
    CPLFree(s);
    return out;
}

GDALDataset* open_raw(const fs::path& path, const std::string& header)
{
    CPLStringList opts;
    opts.AddNameValue("SHORTCOG_HEADER", header.c_str());
    const char* drv[] = {"SHORTCOG", nullptr};
    return GDALDataset::FromHandle(GDALOpenEx(
        path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
        drv, opts.List(), nullptr));
}

// Read a window with the given layout into a uint16 buffer (the fixture is
// uint16). Empty vector on failure.
std::vector<std::uint16_t> read_u16(shortcog::Image* img,
                                    std::span<const int> bands,
                                    int yo, int ys, int xo, int xs,
                                    const LayoutPlan& lay)
{
    std::vector<std::uint16_t> out(bands.size() * static_cast<std::size_t>(ys) * xs);
    if (!img->read(bands, yo, ys, xo, xs, lay,
                   reinterpret_cast<std::byte*>(out.data()))) {
        out.clear();
    }
    return out;
}

void test_engine_image(const fs::path& path, const std::string& header)
{
    DsPtr ds(open_raw(path, header));
    auto* img = dynamic_cast<shortcog::Image*>(ds.get());
    if (!img) { check(false, "open image"); return; }

    const int B = ds->GetRasterCount();
    const int Y = ds->GetRasterYSize();
    const int X = ds->GetRasterXSize();

    std::vector<int> all(B);
    for (int i = 0; i < B; ++i) all[i] = i + 1;

    // reference: full image as b y x, plain [B,Y,X].
    auto ref = read_u16(img, all, 0, Y, 0, X, *compile_layout("b y x", 1, B, Y, X));

    // y x b over the full image: hwc(y,x,b) must equal ref(b,y,x).
    {
        auto hwc = read_u16(img, all, 0, Y, 0, X, *compile_layout("y x b", 1, B, Y, X));
        bool ok = !ref.empty() && !hwc.empty();
        for (int b = 0; ok && b < B; ++b)
            for (int y = 0; ok && y < Y; ++y)
                for (int x = 0; x < X; ++x) {
                    const std::size_t r = (static_cast<std::size_t>(b) * Y + y) * X + x;
                    const std::size_t h = (static_cast<std::size_t>(y) * X + x) * B + b;
                    if (ref[r] != hwc[h]) { ok = false; break; }
                }
        check(ok, "y x b matches b y x (full image)");
    }

    // windowed read crossing tile borders, with bands selected out of order.
    if (B >= 3) {
        const std::vector<int> sel = {B, 2, 1};
        const int nb = static_cast<int>(sel.size());
        const int yo = 8, ys = 20, xo = 5, xs = 30;

        auto rn = read_u16(img, sel, yo, ys, xo, xs, *compile_layout("b y x", 1, nb, ys, xs));
        auto hw = read_u16(img, sel, yo, ys, xo, xs, *compile_layout("y x b", 1, nb, ys, xs));
        bool ok = !rn.empty() && !hw.empty();
        for (int i = 0; ok && i < nb; ++i)
            for (int y = 0; ok && y < ys; ++y)
                for (int x = 0; x < xs; ++x) {
                    const std::size_t r = (static_cast<std::size_t>(i) * ys + y) * xs + x;
                    const std::size_t h = (static_cast<std::size_t>(y) * xs + x) * nb + i;
                    if (rn[r] != hw[h]) { ok = false; break; }
                }
        check(ok, "y x b matches b y x (window + band reorder)");
    }
}

void test_engine_cube(const fs::path& path, const std::string& header)
{
    // Open the same file twice. Identical layers, so each n slice of the cube
    // must equal the single-image reference.
    auto open_layer = [&]() -> std::shared_ptr<shortcog::Image> {
        GDALDataset* d = open_raw(path, header);
        auto* im = dynamic_cast<shortcog::Image*>(d);
        if (!im) { if (d) GDALClose(d); return nullptr; }
        return std::shared_ptr<shortcog::Image>(im, [](shortcog::Image* p) { GDALClose(p); });
    };

    auto a = open_layer();
    auto b = open_layer();
    if (!a || !b) { check(false, "open cube layers"); return; }

    const int B = a->GetRasterCount();
    const int Y = a->GetRasterYSize();
    const int X = a->GetRasterXSize();
    const int N = 2;

    auto cube = shortcog::ImageCube::create({a, b});
    if (!cube) { check(false, "ImageCube::create"); return; }

    std::vector<int> bands(B);
    for (int i = 0; i < B; ++i) bands[i] = i + 1;
    const std::vector<int> ni = {1, 2};

    // a is still valid (create copied the shared_ptrs), reuse it for the ref.
    auto ref = read_u16(a.get(), bands, 0, Y, 0, X, *compile_layout("b y x", 1, B, Y, X));

    // n b y x: out(n,b,y,x) == ref(b,y,x) for every n.
    {
        std::vector<std::uint16_t> out(static_cast<std::size_t>(N) * B * Y * X);
        bool got = cube->read(ni, bands, 0, Y, 0, X,
                              *compile_layout("n b y x", N, B, Y, X),
                              reinterpret_cast<std::byte*>(out.data()));
        bool ok = got && !ref.empty();
        for (int n = 0; ok && n < N; ++n)
            for (int bb = 0; ok && bb < B; ++bb)
                for (int y = 0; ok && y < Y; ++y)
                    for (int x = 0; x < X; ++x) {
                        const std::size_t o = ((static_cast<std::size_t>(n) * B + bb) * Y + y) * X + x;
                        const std::size_t r = (static_cast<std::size_t>(bb) * Y + y) * X + x;
                        if (out[o] != ref[r]) { ok = false; break; }
                    }
        check(ok, "cube n b y x");
    }

    // n (y x) b: out laid out as [N, Y*X, B].
    {
        std::vector<std::uint16_t> out(static_cast<std::size_t>(N) * B * Y * X);
        bool got = cube->read(ni, bands, 0, Y, 0, X,
                              *compile_layout("n (y x) b", N, B, Y, X),
                              reinterpret_cast<std::byte*>(out.data()));
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
        check(ok, "cube n (y x) b");
    }
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
    const std::string header = b64(*blob);

    test_engine_image(path, header);
    test_engine_cube(path, header);

    std::printf("\nSummary: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}