// Single-header SHA-256, test-only. Used by test_fixtures and test_threading
// to hash decoded bands and compare against the Python generator's sidecars.
// Scalar, big-endian I/O so it works on LE and BE hosts. One-shot: the object
// is consumed by finalize().
//
//   SHA256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
//   SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shortcog_sha256_detail {

inline constexpr std::uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept
{
    return (x >> n) | (x << (32 - n));
}

}  // namespace shortcog_sha256_detail


class SHA256 {
public:
    SHA256() noexcept { reset(); }

    SHA256(const SHA256&)            = default;
    SHA256& operator=(const SHA256&) = default;

    void update(const void* data, std::size_t len) noexcept
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bit_length_ += static_cast<std::uint64_t>(len) * 8u;

        if (buffer_len_ > 0) {
            const std::size_t want = 64u - buffer_len_;
            const std::size_t take = std::min(len, want);
            std::memcpy(buffer_.data() + buffer_len_, p, take);
            buffer_len_ += take;
            p          += take;
            len        -= take;
            if (buffer_len_ == 64) {
                process_block(buffer_.data());
                buffer_len_ = 0;
            }
        }

        while (len >= 64) {
            process_block(p);
            p   += 64;
            len -= 64;
        }

        if (len > 0) {
            std::memcpy(buffer_.data(), p, len);
            buffer_len_ = len;
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finalize() noexcept
    {
        // 0x80, zero pad to 56 mod 64, then the bit length as big-endian u64.
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56) {
            std::memset(buffer_.data() + buffer_len_, 0, 64u - buffer_len_);
            process_block(buffer_.data());
            buffer_len_ = 0;
        }
        std::memset(buffer_.data() + buffer_len_, 0, 56u - buffer_len_);
        for (int i = 0; i < 8; ++i) {
            buffer_[56 + i] = static_cast<std::uint8_t>(bit_length_ >> (56 - i * 8));
        }
        process_block(buffer_.data());

        std::array<std::uint8_t, 32> digest{};
        for (int i = 0; i < 8; ++i) {
            digest[i * 4    ] = static_cast<std::uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >>  8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    void reset() noexcept
    {
        state_ = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
        };
        bit_length_ = 0;
        buffer_len_ = 0;
    }

    void process_block(const std::uint8_t* block) noexcept
    {
        using shortcog_sha256_detail::K;
        using shortcog_sha256_detail::rotr;

        std::uint32_t W[64];
        for (int i = 0; i < 16; ++i) {
            W[i] = (static_cast<std::uint32_t>(block[i * 4    ]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) <<  8)
                 |  static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr(W[i - 15],  7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >>  3);
            const std::uint32_t s1 =
                rotr(W[i -  2], 17) ^ rotr(W[i -  2], 19) ^ (W[i -  2] >> 10);
            W[i] = W[i - 16] + s0 + W[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1  = rotr(e,  6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch  = (e & f) ^ (~e & g);
            const std::uint32_t t1  = h + S1 + ch + K[i] + W[i];
            const std::uint32_t S0  = rotr(a,  2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2  = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8>  state_{};
    std::array<std::uint8_t, 64>  buffer_{};
    std::uint64_t                 bit_length_ = 0;
    std::size_t                   buffer_len_ = 0;
};