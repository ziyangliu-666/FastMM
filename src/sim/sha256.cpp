#include "fastmm/sim/sha256.hpp"

#include <cstddef>
#include <cstring>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

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

#if defined(__x86_64__)
// One block with the x86 SHA extensions (SHA-NI): the state is kept as ABEF / CDGH halves, four
// rounds per message group, the schedule from sha256msg1 / sha256msg2. The outbound hash runs on
// every order the engine sends in the simulator (and in BM_TickToOrder_Sim), where the portable
// transform was about half of the measured time.
__attribute__((target("sha,sse4.1"))) void transform_shani(std::uint32_t state[8],
                                                           const std::uint8_t block[64]) noexcept {
  const __m128i kMask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
  __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
  tmp = _mm_shuffle_epi32(tmp, 0xB1);                // CDAB
  state1 = _mm_shuffle_epi32(state1, 0x1B);          // EFGH
  __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
  state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
  const __m128i abef = state0;
  const __m128i cdgh = state1;
  __m128i w[4];
  for (std::size_t i = 0; i < 16; ++i) {
    if (i < 4) {
      w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16 * i)),
                              kMask);
    } else {
      __m128i t = _mm_sha256msg1_epu32(w[(i - 4) & 3], w[(i - 3) & 3]);
      t = _mm_add_epi32(t, _mm_alignr_epi8(w[(i - 1) & 3], w[(i - 2) & 3], 4));
      w[i & 3] = _mm_sha256msg2_epu32(t, w[(i - 1) & 3]);
    }
    __m128i msg =
        _mm_add_epi32(w[i & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(&kK[4 * i])));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  }
  state0 = _mm_add_epi32(state0, abef);
  state1 = _mm_add_epi32(state1, cdgh);
  tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
  state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
  state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
}

bool has_sha_ni() noexcept {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (__get_cpuid(1, &a, &b, &c, &d) == 0 || (c & bit_SSE4_1) == 0) return false;
  if (__get_cpuid_count(7, 0, &a, &b, &c, &d) == 0) return false;
  return (b & bit_SHA) != 0;
}

const bool kHasShaNi = has_sha_ni();
#endif
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

void Sha256::transform(const std::uint8_t block[64]) noexcept {
#if defined(__x86_64__)
  if (kHasShaNi) {
    transform_shani(h_, block);
    return;
  }
#endif
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
  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
  std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_ += len;
  while (len > 0) {
    if (buf_len_ == 0 && len >= 64) {  // whole blocks straight from the input
      transform(p);
      p += 64;
      len -= 64;
      continue;
    }
    const std::size_t take = 64 - buf_len_ < len ? 64 - buf_len_ : len;
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ == 64) {
      transform(buf_);
      buf_len_ = 0;
    }
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
