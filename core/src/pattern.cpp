#include "shortcog/shortcog.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace shortcog {
namespace {

// n, b, y, x -> 0..3; anything else -> -1.
int axis_index(char c) noexcept
{
    switch (c) {
        case 'n': return 0;
        case 'b': return 1;
        case 'y': return 2;
        case 'x': return 3;
        default:  return -1;
    }
}

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

}  // namespace

std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t b,
               std::int64_t y, std::int64_t x)
{
    if (n <= 0 || b <= 0 || y <= 0 || x <= 0)
        return err("extents n, b, y, x must be positive");

    const std::array<std::int64_t, 4> size{ n, b, y, x };

    // Parse into output groups. A bare axis is a group of one; parentheses merge
    // several axes into one output axis. One level of parentheses, no splits.
    std::vector<std::vector<int>> groups;
    std::vector<int>              cur;
    bool                          in_paren = false;
    std::array<bool, 4>           seen{ false, false, false, false };

    for (char c : pattern) {
        if (c == ' ') continue;
        if (c == '(') {
            if (in_paren) return err("nested parentheses are not allowed");
            in_paren = true;
            cur.clear();
            continue;
        }
        if (c == ')') {
            if (!in_paren)  return err("unbalanced ')'");
            if (cur.empty()) return err("empty group '()'");
            groups.push_back(cur);
            in_paren = false;
            continue;
        }
        const int a = axis_index(c);
        if (a < 0)    return err(std::string("unknown axis '") + c + "' (expected n, b, y, x)");
        if (seen[a])  return err(std::string("axis '") + c + "' used more than once");
        seen[a] = true;
        if (in_paren) cur.push_back(a);
        else          groups.push_back({ a });
    }
    if (in_paren)      return err("unbalanced '('");
    if (groups.empty()) return err("empty pattern");

    // Axis set is exactly {b, y, x} or {n, b, y, x}. n present means a cube axis;
    // absent means a single image, valid only when n == 1.
    const bool has_n = seen[0];
    if (!seen[1] || !seen[2] || !seen[3]) return err("pattern must contain b, y, x");
    if (!has_n && n > 1)                  return err("n > 1 needs n in the pattern");

    // Flatten to output order, then element strides right to left. run ends up
    // as the full element count, so guarding it bounds every stride and shape.
    std::vector<int> flat;
    for (const auto& g : groups)
        for (int a : g) flat.push_back(a);

    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    std::array<std::int64_t, 4> stride{ 0, 0, 0, 0 };
    std::int64_t run = 1;
    for (auto it = flat.rbegin(); it != flat.rend(); ++it) {
        stride[*it] = run;
        if (run > kMax / size[*it]) return err("layout size overflows int64");
        run *= size[*it];
    }

    LayoutPlan plan;
    plan.shape.reserve(groups.size());
    for (const auto& g : groups) {
        std::int64_t s = 1;
        for (int a : g) s *= size[a];
        plan.shape.push_back(s);
    }
    plan.sn = stride[0];
    plan.sb = stride[1];
    plan.sy = stride[2];
    plan.sx = stride[3];

    // native means the output order is canonical, so the buffer is plain
    // C-contiguous.
    const std::vector<int> canon = has_n
        ? std::vector<int>{ 0, 1, 2, 3 }
        : std::vector<int>{ 1, 2, 3 };
    plan.native = (flat == canon);

    return plan;
}

}  // namespace shortcog