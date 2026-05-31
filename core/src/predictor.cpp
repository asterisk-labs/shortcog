#include "shortcog/shortcog.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define SHORTCOG_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#define SHORTCOG_NEON 1
#endif

// Inverse TIFF horizontal predictor (PREDICTOR=2). Each row was stored as
// first-order differences; the decoder is a left-to-right prefix sum.

namespace shortcog {
namespace {

template <typename T>
[[nodiscard]] inline T* row_as(std::byte* p, [[maybe_unused]] std::size_t n) noexcept
{
#ifdef __cpp_lib_start_lifetime_as
    return std::start_lifetime_as_array<T>(p, n);
#else
    // P2590 fallback: memmove triggers implicit object creation for
    // trivially-copyable T; launder defeats the optimizer's lifetime
    // tracking. The self-memmove is elided.
    return std::launder(static_cast<T*>(std::memmove(p, p, n * sizeof(T))));
#endif
}

template <typename T>
void decode_row(T* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    using Acc = std::conditional_t<(sizeof(T) < sizeof(std::uint64_t)),
                                   std::uint32_t, T>;
    Acc acc = row[0];
    for (std::size_t i = 1; i < n; ++i) {
        acc = static_cast<Acc>(acc + row[i]);
        row[i] = static_cast<T>(acc);
    }
}

#if defined(SHORTCOG_SSE2)

// Hillis-Steele in-register prefix sum. Each 128-bit block scans itself in
// log2(lanes) shift-and-add steps; broadcasting the block total seeds the
// next block.

void decode_row_simd(std::uint8_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    __m128i carry = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi8(x, carry);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + i), x);
        // SSE2 has no extract_epi8; pull byte 15 via epi16.
        carry = _mm_set1_epi8(static_cast<char>(_mm_extract_epi16(x, 7) >> 8));
    }
    std::uint32_t acc = static_cast<std::uint8_t>(_mm_cvtsi128_si32(carry));
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = static_cast<std::uint8_t>(acc);
    }
}

void decode_row_simd(std::uint16_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    __m128i carry = _mm_setzero_si128();
    for (; i + 8 <= n; i += 8) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 2));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi16(x, carry);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + i), x);
        carry = _mm_set1_epi16(static_cast<short>(_mm_extract_epi16(x, 7)));
    }
    std::uint32_t acc = static_cast<std::uint16_t>(_mm_extract_epi16(carry, 0));
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = static_cast<std::uint16_t>(acc);
    }
}

void decode_row_simd(std::uint32_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    __m128i carry = _mm_setzero_si128();
    for (; i + 4 <= n; i += 4) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
        x = _mm_add_epi32(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi32(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi32(x, carry);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row + i), x);
        carry = _mm_shuffle_epi32(x, _MM_SHUFFLE(3, 3, 3, 3));
    }
    std::uint32_t acc = static_cast<std::uint32_t>(_mm_cvtsi128_si32(carry));
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = acc;
    }
}

#elif defined(SHORTCOG_NEON)

// Same scan as the SSE2 path. vextq against a zero vector reproduces the
// byte-shift slli_si128 gives on x86; vgetq_lane + vdupq broadcasts the
// per-block carry.

void decode_row_simd(std::uint8_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    const uint8x16_t zero = vdupq_n_u8(0);
    uint8x16_t carry = zero;
    for (; i + 16 <= n; i += 16) {
        uint8x16_t x = vld1q_u8(row + i);
        x = vaddq_u8(x, vextq_u8(zero, x, 15));
        x = vaddq_u8(x, vextq_u8(zero, x, 14));
        x = vaddq_u8(x, vextq_u8(zero, x, 12));
        x = vaddq_u8(x, vextq_u8(zero, x, 8));
        x = vaddq_u8(x, carry);
        vst1q_u8(row + i, x);
        carry = vdupq_n_u8(vgetq_lane_u8(x, 15));
    }
    std::uint32_t acc = vgetq_lane_u8(carry, 0);
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = static_cast<std::uint8_t>(acc);
    }
}

void decode_row_simd(std::uint16_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    const uint16x8_t zero = vdupq_n_u16(0);
    uint16x8_t carry = zero;
    for (; i + 8 <= n; i += 8) {
        uint16x8_t x = vld1q_u16(row + i);
        x = vaddq_u16(x, vextq_u16(zero, x, 7));
        x = vaddq_u16(x, vextq_u16(zero, x, 6));
        x = vaddq_u16(x, vextq_u16(zero, x, 4));
        x = vaddq_u16(x, carry);
        vst1q_u16(row + i, x);
        carry = vdupq_n_u16(vgetq_lane_u16(x, 7));
    }
    std::uint32_t acc = vgetq_lane_u16(carry, 0);
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = static_cast<std::uint16_t>(acc);
    }
}

void decode_row_simd(std::uint32_t* row, std::size_t n) noexcept
{
    if (n <= 1) return;
    std::size_t i = 0;
    const uint32x4_t zero = vdupq_n_u32(0);
    uint32x4_t carry = zero;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t x = vld1q_u32(row + i);
        x = vaddq_u32(x, vextq_u32(zero, x, 3));
        x = vaddq_u32(x, vextq_u32(zero, x, 2));
        x = vaddq_u32(x, carry);
        vst1q_u32(row + i, x);
        carry = vdupq_n_u32(vgetq_lane_u32(x, 3));
    }
    std::uint32_t acc = vgetq_lane_u32(carry, 0);
    for (; i < n; ++i) {
        acc += row[i];
        row[i] = acc;
    }
}

#else

inline void decode_row_simd(std::uint8_t*  row, std::size_t n) noexcept { decode_row(row, n); }
inline void decode_row_simd(std::uint16_t* row, std::size_t n) noexcept { decode_row(row, n); }
inline void decode_row_simd(std::uint32_t* row, std::size_t n) noexcept { decode_row(row, n); }

#endif

}  // namespace


void apply_horizontal_predictor(std::span<std::byte> tile,
                                std::uint16_t tile_width,
                                std::uint16_t tile_length,
                                std::uint8_t  bytes_per_sample) noexcept
{
    const std::size_t row_pixels = tile_width;
    const std::size_t rows       = tile_length;
    const std::size_t row_bytes  = row_pixels * bytes_per_sample;
    if (row_pixels <= 1 || rows == 0) return;

    std::byte* base = tile.data();

    // Caller guarantees base alignment >= bytes_per_sample. For GDAL block
    // buffers and numpy arrays this holds because every stride is a multiple
    // of bytes_per_sample.
    assert(reinterpret_cast<std::uintptr_t>(base) % bytes_per_sample == 0);

    switch (bytes_per_sample) {
        case 1:
            for (std::size_t r = 0; r < rows; ++r)
                decode_row_simd(row_as<std::uint8_t>(base + r * row_bytes, row_pixels), row_pixels);
            break;
        case 2:
            for (std::size_t r = 0; r < rows; ++r)
                decode_row_simd(row_as<std::uint16_t>(base + r * row_bytes, row_pixels), row_pixels);
            break;
        case 4:
            for (std::size_t r = 0; r < rows; ++r)
                decode_row_simd(row_as<std::uint32_t>(base + r * row_bytes, row_pixels), row_pixels);
            break;
        case 8:
            for (std::size_t r = 0; r < rows; ++r)
                decode_row(row_as<std::uint64_t>(base + r * row_bytes, row_pixels), row_pixels);
            break;
        default:
            // CFloat64 (16-byte) has PREDICTOR=2 rejected at parse.
            assert(false);
            break;
    }
}


}  // namespace shortcog