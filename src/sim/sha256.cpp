#include "fastmm/sim/sha256.hpp"

#include <cpuid.h>
#include <immintrin.h>

#include <cstddef>
#include <cstring>

namespace fastmm::sim {

namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept {
  return (x >> n) | (x << (32 - n));
}
}  // namespace

void Sha256::reset() noexcept {
  h_[0] = 0x6a09e667;
  h_[1] = 0xbb67ae85;
  h_[2] = 0x3c6ef372;
  h_[3] = 0xa54ff53a;
  h_[4] = 0x510e527f;
  h_[5] = 0x9b05688c;
  h_[6] = 0x1f83d9ab;
  h_[7] = 0x5be0cd19;
  buf_len_ = 0;
  total_ = 0;
}

namespace detail {

bool sha256_hardware_available() noexcept {
  static const bool ok = [] {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) == 0) return false;
    const bool ssse3 = (c & bit_SSSE3) != 0;
    const bool sse41 = (c & bit_SSE4_1) != 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d) == 0) return false;
    return ssse3 && sse41 && (b & bit_SHA) != 0;
  }();
  return ok;
}

// SHA-NI rounds: sha256rnds2 does two rounds on the ABEF/CDGH halves of the state;
// sha256msg1/msg2 extend the message schedule four words at a time.
__attribute__((target("sha,sse4.1,ssse3"))) void sha256_blocks_hardware(
    std::uint32_t h[8], const std::uint8_t* data, std::size_t blocks) noexcept {
  const __m128i kBswap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h));
  __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h + 4));
  tmp = _mm_shuffle_epi32(tmp, 0xB1);                // CDAB
  state1 = _mm_shuffle_epi32(state1, 0x1B);          // EFGH
  __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
  state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
  for (; blocks > 0; --blocks, data += 64) {
    const __m128i abef = state0;
    const __m128i cdgh = state1;
    __m128i m[4];
#pragma GCC unroll 16
    for (int g = 0; g < 16; ++g) {
      if (g < 4) {
        m[g] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * g)),
                                kBswap);
      }
      __m128i msg =
          _mm_add_epi32(m[g % 4], _mm_loadu_si128(reinterpret_cast<const __m128i*>(kK + 4 * g)));
      state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
      if (g >= 3 && g < 15) {
        const __m128i t = _mm_alignr_epi8(m[g % 4], m[(g + 3) % 4], 4);
        m[(g + 1) % 4] = _mm_sha256msg2_epu32(_mm_add_epi32(m[(g + 1) % 4], t), m[g % 4]);
      }
      msg = _mm_shuffle_epi32(msg, 0x0E);
      state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
      if (g >= 1 && g < 13) m[(g + 3) % 4] = _mm_sha256msg1_epu32(m[(g + 3) % 4], m[g % 4]);
    }
    state0 = _mm_add_epi32(state0, abef);
    state1 = _mm_add_epi32(state1, cdgh);
  }
  tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
  state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
  state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i*>(h), state0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(h + 4), state1);
}

void sha256_blocks_portable(std::uint32_t h[8],
                            const std::uint8_t* data,
                            std::size_t blocks) noexcept {
  for (; blocks > 0; --blocks, data += 64) {
    const std::uint8_t* block = data;
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
}

}  // namespace detail

void Sha256::transform(const std::uint8_t* blocks, std::size_t n) noexcept {
  if (detail::sha256_hardware_available()) {
    detail::sha256_blocks_hardware(h_, blocks, n);
  } else {
    detail::sha256_blocks_portable(h_, blocks, n);
  }
}

void Sha256::update(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_ += len;
  if (buf_len_ != 0) {
    const std::size_t take = 64 - buf_len_ < len ? 64 - buf_len_ : len;
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ < 64) return;
    transform(buf_, 1);
    buf_len_ = 0;
  }
  if (len >= 64) {  // whole blocks straight from the input
    const std::size_t n = len / 64;
    transform(p, n);
    p += n * 64;
    len -= n * 64;
  }
  if (len != 0) {
    std::memcpy(buf_, p, len);
    buf_len_ = len;
  }
}

void Sha256::digest(std::uint8_t out[32]) const noexcept {
  Sha256 c = *this;
  const std::uint64_t bits = c.total_ * 8;
  const std::uint8_t one = 0x80;
  c.update(&one, 1);
  const std::uint8_t zero = 0;
  while (c.buf_len_ != 56) c.update(&zero, 1);
  std::uint8_t len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
  c.update(len_be, 8);
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(c.h_[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(c.h_[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(c.h_[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(c.h_[i]);
  }
}

std::string Sha256::hex() const {
  std::uint8_t d[32];
  digest(d);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s(64, '0');
  for (int i = 0; i < 32; ++i) {
    s[static_cast<std::size_t>(i) * 2] = kHex[d[i] >> 4];
    s[static_cast<std::size_t>(i) * 2 + 1] = kHex[d[i] & 15];
  }
  return s;
}

}  // namespace fastmm::sim
