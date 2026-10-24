// FIPS 180-4.
#include "sha256.h"

#include <array>
#include <cstdint>

namespace tortilla
{
namespace
{

constexpr std::array<uint32_t, 64> k_round = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void compress(std::array<uint32_t, 8>& h, const uint8_t* block)
{
    std::array<uint32_t, 64> w;
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 |
               uint32_t(block[i * 4 + 2]) << 8 | uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (int i = 0; i < 64; ++i)
    {
        const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                            k_round[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::array<uint32_t, 8> add{a, b, c, d, e, f, g, hh};
    for (int i = 0; i < 8; ++i) h[i] += add[i];
}

}  // namespace

std::string sha256_hex(std::string_view data)
{
    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t at = 0;
    for (; at + 64 <= data.size(); at += 64) compress(h, bytes + at);

    // The tail, a 1 bit, zeros, and the length in bits: one block or two.
    std::array<uint8_t, 128> tail{};
    const size_t rest = data.size() - at;
    for (size_t i = 0; i < rest; ++i) tail[i] = bytes[at + i];
    tail[rest] = 0x80;
    const size_t blocks = rest < 56 ? 1 : 2;
    const uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[blocks * 64 - 1 - i] = uint8_t(bits >> (8 * i));
    for (size_t b = 0; b < blocks; ++b) compress(h, tail.data() + b * 64);

    static constexpr char k_hex[] = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h)
        for (int shift = 28; shift >= 0; shift -= 4) out += k_hex[(v >> shift) & 0xf];
    return out;
}

}  // namespace tortilla
