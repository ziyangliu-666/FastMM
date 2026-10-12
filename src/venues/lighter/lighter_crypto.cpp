#include "fastmm/venues/lighter/lighter_crypto.hpp"

#include "fastmm/core/config_macros.hpp"

#include <sys/random.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace fastmm::venues::lighter {

namespace {

using u64 = std::uint64_t;
using u128 = fastmm::Uint128;

// ---- Goldilocks ---------------------------------------------------------------------------------
// Values are kept as any u64 (not necessarily below p), as poseidon_crypto does; canonical() is
// taken where bytes or comparisons need it.

constexpr u64 kP = 0xffffffff00000001ULL;
constexpr u64 kEps = 0xffffffffULL;  // 2^64 mod p

constexpr u64 canonical(u64 x) noexcept {
  return x >= kP ? x - kP : x;
}

inline u64 fadd(u64 a, u64 b) noexcept {
  u64 s = a + b;
  const u64 over = s < a ? 1 : 0;
  const u64 s2 = s + over * kEps;
  if (s2 < s) return s2 + kEps;
  return s2;
}

inline u64 fsub(u64 a, u64 b) noexcept {
  const u64 d = a - b;
  const u64 borrow = a < b ? kEps : 0;
  const u64 d2 = d - borrow;
  if (d < borrow) return d2 - kEps;
  return d2;
}

inline u64 reduce128(u128 x) noexcept {
  const auto lo = static_cast<u64>(x);
  const auto hi = static_cast<u64>(x >> 64);
  const u64 hi_hi = hi >> 32;
  const u64 hi_lo = hi & kEps;
  u64 t0 = lo - hi_hi;
  if (lo < hi_hi) t0 -= kEps;
  const u64 t1 = hi_lo * kEps;
  const u64 sum = t0 + t1;
  return sum + (sum < t0 ? kEps : 0);
}

// x < 2^96.
inline u64 reduce96(u128 x) noexcept {
  const auto lo = static_cast<u64>(x);
  const auto hi = static_cast<u64>(x >> 64);
  const u64 t1 = hi * kEps;
  const u64 res = lo + t1;
  return res + (res < lo ? kEps : 0);
}

inline u64 fmul(u64 a, u64 b) noexcept {
  return reduce128(static_cast<u128>(a) * b);
}

inline u64 fsquare(u64 a) noexcept {
  return fmul(a, a);
}

// a + c where c < p.
inline u64 fadd_canonical(u64 a, u64 c) noexcept {
  const u64 s = a + c;
  return s + (s < a ? kEps : 0);
}

inline u64 fneg(u64 x) noexcept {
  const u64 c = canonical(x);
  return c == 0 ? 0 : kP - c;
}

inline u64 fexp_pow2(u64 x, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) x = fsquare(x);
  return x;
}

u64 fexp(u64 x, u64 e) noexcept {
  u64 r = 1;
  while (e > 0) {
    if ((e & 1) != 0) r = fmul(r, x);
    x = fsquare(x);
    e >>= 1;
  }
  return r;
}

// x^(p-2) by poseidon_crypto's addition chain; 0 for 0.
u64 finv(u64 x) noexcept {
  if (canonical(x) == 0) return 0;
  const u64 t2 = fmul(fsquare(x), x);
  const u64 t3 = fmul(fsquare(t2), x);
  const u64 t6 = fmul(fexp_pow2(t3, 3), t3);
  const u64 t12 = fmul(fexp_pow2(t6, 6), t6);
  const u64 t24 = fmul(fexp_pow2(t12, 12), t12);
  const u64 t30 = fmul(fexp_pow2(t24, 6), t6);
  const u64 t31 = fmul(fsquare(t30), x);
  const u64 t63 = fmul(fexp_pow2(t31, 32), t31);
  return fmul(fsquare(t63), x);
}

// Tonelli-Shanks; false for a non-residue.
bool fsqrt(u64 x, u64& out) noexcept {
  x = canonical(x);
  if (x == 0) {
    out = 0;
    return true;
  }
  const u64 legendre = canonical(fexp(x, (kP - 1) >> 1));
  if (legendre != 1) return false;
  constexpr int kTwoAdicity = 32;
  constexpr u64 kPowerOfTwoGenerator = 7277203076849721926ULL;
  const u64 t = (kP - 1) >> kTwoAdicity;
  u64 z = kPowerOfTwoGenerator;
  u64 w = fexp(x, (t - 1) / 2);
  u64 r = fmul(x, w);
  u64 b = fmul(r, w);
  int v = kTwoAdicity;
  while (canonical(b) != 1) {
    int k = 0;
    u64 b2k = b;
    while (canonical(b2k) != 1) {
      b2k = fsquare(b2k);
      ++k;
    }
    const int j = v - k - 1;
    w = z;
    for (int n = 0; n < j; ++n) w = fsquare(w);
    z = fsquare(w);
    b = fmul(b, z);
    r = fmul(r, w);
    v = k;
  }
  out = r;
  return true;
}

// ---- Fp5 = Fp[x]/(x^5 - 3)
// -----------------------------------------------------------------------

using Fp5 = std::array<u64, 5>;
constexpr u64 kW = 3;
constexpr u64 kDthRoot = 1041288259238279555ULL;
constexpr Fp5 kZero5{0, 0, 0, 0, 0};
constexpr Fp5 kOne5{1, 0, 0, 0, 0};

inline Fp5 add5(const Fp5& a, const Fp5& b) noexcept {
  return {fadd(a[0], b[0]), fadd(a[1], b[1]), fadd(a[2], b[2]), fadd(a[3], b[3]), fadd(a[4], b[4])};
}
inline Fp5 sub5(const Fp5& a, const Fp5& b) noexcept {
  return {fsub(a[0], b[0]), fsub(a[1], b[1]), fsub(a[2], b[2]), fsub(a[3], b[3]), fsub(a[4], b[4])};
}
inline Fp5 dbl5(const Fp5& a) noexcept {
  return add5(a, a);
}
inline Fp5 neg5(const Fp5& a) noexcept {
  return {fneg(a[0]), fneg(a[1]), fneg(a[2]), fneg(a[3]), fneg(a[4])};
}
inline bool is_zero5(const Fp5& a) noexcept {
  return canonical(a[0]) == 0 && canonical(a[1]) == 0 && canonical(a[2]) == 0 &&
         canonical(a[3]) == 0 && canonical(a[4]) == 0;
}
inline bool eq5(const Fp5& a, const Fp5& b) noexcept {
  for (std::size_t i = 0; i < 5; ++i) {
    if (canonical(a[i]) != canonical(b[i])) return false;
  }
  return true;
}

Fp5 mul5(const Fp5& a, const Fp5& b) noexcept {
  const u64 c0 = fadd(fmul(a[0], b[0]),
                      fmul(kW,
                           fadd(fadd(fmul(a[1], b[4]), fmul(a[2], b[3])),
                                fadd(fmul(a[3], b[2]), fmul(a[4], b[1])))));
  const u64 c1 = fadd(fadd(fmul(a[0], b[1]), fmul(a[1], b[0])),
                      fmul(kW, fadd(fadd(fmul(a[2], b[4]), fmul(a[3], b[3])), fmul(a[4], b[2]))));
  const u64 c2 = fadd(fadd(fmul(a[0], b[2]), fmul(a[1], b[1])),
                      fadd(fmul(a[2], b[0]), fmul(kW, fadd(fmul(a[3], b[4]), fmul(a[4], b[3])))));
  const u64 c3 =
      fadd(fadd(fadd(fmul(a[0], b[3]), fmul(a[1], b[2])), fadd(fmul(a[2], b[1]), fmul(a[3], b[0]))),
           fmul(kW, fmul(a[4], b[4])));
  const u64 c4 =
      fadd(fadd(fadd(fmul(a[0], b[4]), fmul(a[1], b[3])), fadd(fmul(a[2], b[2]), fmul(a[3], b[1]))),
           fmul(a[4], b[0]));
  return {c0, c1, c2, c3, c4};
}

Fp5 square5(const Fp5& a) noexcept {
  constexpr u64 kW2 = 2 * kW;
  const u64 a0d = fadd(a[0], a[0]);
  const u64 a1d = fadd(a[1], a[1]);
  const u64 c0 = fadd(fmul(a[0], a[0]), fmul(kW2, fadd(fmul(a[1], a[4]), fmul(a[2], a[3]))));
  const u64 c1 =
      fadd(fadd(fmul(a0d, a[1]), fmul(fmul(a[2], a[4]), kW2)), fmul(fmul(a[3], a[3]), kW));
  const u64 c2 = fadd(fadd(fmul(a0d, a[2]), fmul(a[1], a[1])), fmul(fmul(a[4], a[3]), kW2));
  const u64 c3 = fadd(fadd(fmul(a0d, a[3]), fmul(a1d, a[2])), fmul(fmul(a[4], a[4]), kW));
  const u64 c4 = fadd(fadd(fmul(a0d, a[4]), fmul(a1d, a[3])), fmul(a[2], a[2]));
  return {c0, c1, c2, c3, c4};
}

Fp5 scalar_mul5(const Fp5& a, u64 s) noexcept {
  return {fmul(a[0], s), fmul(a[1], s), fmul(a[2], s), fmul(a[3], s), fmul(a[4], s)};
}

Fp5 frobenius5(const Fp5& x, std::size_t count) noexcept {
  count %= 5;
  if (count == 0) return x;
  u64 z0 = kDthRoot;
  for (std::size_t i = 1; i < count; ++i) z0 = fmul(kDthRoot, z0);
  Fp5 r{};
  u64 z = 1;
  for (std::size_t i = 0; i < 5; ++i) {
    r[i] = fmul(x[i], z);
    z = fmul(z, z0);
  }
  return r;
}

Fp5 inv5(const Fp5& a) noexcept {
  if (is_zero5(a)) return kZero5;
  const Fp5 d = frobenius5(a, 1);
  const Fp5 e = mul5(d, frobenius5(d, 1));
  const Fp5 f = mul5(e, frobenius5(e, 2));
  const u64 g = fadd(fmul(a[0], f[0]),
                     fmul(kW,
                          fadd(fadd(fmul(a[1], f[4]), fmul(a[2], f[3])),
                               fadd(fmul(a[3], f[2]), fmul(a[4], f[1])))));
  return scalar_mul5(f, finv(g));
}

Fp5 exp_pow2_5(Fp5 x, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) x = square5(x);
  return x;
}

bool sqrt5(const Fp5& x, Fp5& out) noexcept {
  const Fp5 v = exp_pow2_5(x, 31);
  const Fp5 d = mul5(mul5(x, exp_pow2_5(v, 32)), inv5(v));
  const Fp5 e = frobenius5(mul5(d, frobenius5(d, 2)), 1);
  const Fp5 f = square5(e);
  const u64 g = fadd(fmul(x[0], f[0]),
                     fmul(3,
                          fadd(fadd(fmul(x[1], f[4]), fmul(x[2], f[3])),
                               fadd(fmul(x[3], f[2]), fmul(x[4], f[1])))));
  u64 s = 0;
  if (!fsqrt(g, s)) return false;
  out = scalar_mul5(inv5(e), s);
  return true;
}

bool sgn0_5(const Fp5& x) noexcept {
  bool sign = false;
  bool zero = true;
  for (const u64 limb : x) {
    const u64 c = canonical(limb);
    sign = sign || (zero && (c & 1) == 0);
    zero = zero && c == 0;
  }
  return sign;
}

u64 legendre5(const Fp5& x) noexcept {
  const Fp5 f1 = frobenius5(x, 1);
  const Fp5 f2 = frobenius5(f1, 1);
  const Fp5 f12 = mul5(f1, f2);
  const Fp5 f2f12 = frobenius5(f12, 2);
  const u64 xr = mul5(mul5(x, f12), f2f12)[0];
  const u64 xr31 = fexp_pow2(xr, 31);
  return canonical(fmul(fexp_pow2(xr31, 32), finv(xr31)));
}

void fp5_bytes(const Fp5& a, std::uint8_t* out) noexcept {
  for (std::size_t i = 0; i < 5; ++i) {
    const u64 c = canonical(a[i]);
    for (std::size_t b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<std::uint8_t>(c >> (8 * b));
  }
}

Fp5 fp5_from_bytes(const std::uint8_t* in) noexcept {
  Fp5 a{};
  for (std::size_t i = 0; i < 5; ++i) {
    u64 v = 0;
    for (std::size_t b = 0; b < 8; ++b) v |= static_cast<u64>(in[i * 8 + b]) << (8 * b);
    a[i] = v;
  }
  return a;
}

// ---- Poseidon2 (plonky2 variant)
// -----------------------------------------------------------------

constexpr std::size_t kWidth = 12;
constexpr std::size_t kRate = 8;

// clang-format off
constexpr std::uint64_t kExternal[8][12] = {
    {0xd70193d17ab3b7d6ULL, 0xa2c3662a78a9162bULL, 0x7a9fda827556ad44ULL, 0xe8d5501818c99643ULL,
     0x4c7a8fced4d5fd38ULL, 0x55ab38985c0c513dULL, 0x28a17bd016210b0bULL, 0x8f8277679ec32fa8ULL,
     0x768b3c3d68a460e9ULL, 0x872a022eb559d941ULL, 0xd1316dd4b3b97973ULL, 0xa7b608e578321000ULL},
    {0x3fa02c87b0bee026ULL, 0x7a38f0022e13c31eULL, 0x00c054f3c5e8d20dULL, 0x439f50f4bca7242fULL,
     0x4d0938aa57cd517fULL, 0xb2e03ac5fb6b9a7dULL, 0xe29d1f4237bedca8ULL, 0x05b7c844bc99b848ULL,
     0x91cc0b73f34e17edULL, 0x876e4427694bd755ULL, 0x67002ae0725c612dULL, 0x05351f20e0b6315fULL},
    {0x2e3b9ef5457eb60bULL, 0xd9ac17618c3783ddULL, 0x0807528ad8874bcfULL, 0xc78d546a455d2a0eULL,
     0xf8b930c81e2481f0ULL, 0x712707d8dff3b041ULL, 0xdcb8c0aa0b9d34c3ULL, 0x9baddbdf2ee3a468ULL,
     0x2dd16d50c5176c78ULL, 0x89eac5cfbc075cd3ULL, 0x2a741dea181587f3ULL, 0x1a4d6aa85a113d84ULL},
    {0x4d736286a2387e34ULL, 0x8bad5dfc4fcb3ee3ULL, 0x84fbd03adb77c56aULL, 0x8d5cdd1a23ec53a2ULL,
     0x036f08f08fff28ecULL, 0xb717a3f4dbdfb443ULL, 0x58a074b5509d645cULL, 0xf92bf834e4b87718ULL,
     0x1541c3a0baa5ac4bULL, 0x22149e6783e67692ULL, 0x9be8b5d9e112476fULL, 0x41e0969f62babb76ULL},
    {0xbc585ad3b9443dbbULL, 0xf28dd3206975cbb1ULL, 0xdd8815e53ca045e0ULL, 0xde82c416b9e701baULL,
     0xc5cb875233afa025ULL, 0x7212697cd897ffa9ULL, 0x67844790aa63cfd7ULL, 0xdc0b9cfa97fe65c3ULL,
     0xe8fe091869a82070ULL, 0x62902bb2e413c6d1ULL, 0x29f9f5001fb84f57ULL, 0xbe1014796ef5f8beULL},
    {0x71feb53e9bdba19cULL, 0x251054f592ebb71cULL, 0xe1a57643a4bb284bULL, 0xa4ba6f87a45b739bULL,
     0x2c1fcade0b958c49ULL, 0xbbb424cda9a3e360ULL, 0x2ca647354c5f3f54ULL, 0xc9277b64d152e084ULL,
     0xdbc9ac97445eff17ULL, 0x6f6cdf3198969f70ULL, 0x1de29d14fa76d8f1ULL, 0x73337458a8cc1d19ULL},
    {0xb87e775e2fb3ab23ULL, 0xf166a1c7a565c80bULL, 0xb24be06f426c747fULL, 0xc281e8c49482ce00ULL,
     0x51974c3b3b726c2dULL, 0x87444cf8caf7d619ULL, 0x7c362f827a580cedULL, 0x9567af14667647a0ULL,
     0xcbf0473cbec54e37ULL, 0xe3209dedeff4f620ULL, 0xd43ad94e45a4c4eeULL, 0x976981ee73f41768ULL},
    {0xef707a224e207258ULL, 0x2fc779e10e6362eeULL, 0x29b5ee60ad8c891fULL, 0x96b37b39d8bfd667ULL,
     0x877df68a8b22e733ULL, 0x5c41746f562c8d9fULL, 0x0c9d76751052b71aULL, 0xfb3465341bf1c087ULL,
     0xa0d14dc614d15eb1ULL, 0xdc27d17136906fa6ULL, 0x482e163b05ec397fULL, 0x0273a462992366efULL},
};
constexpr std::uint64_t kInternal[22] = {
    0xa571418d95897b60ULL, 0x8f32676574fcf6d3ULL, 0x731102d4e3fb1bbeULL, 0x0330f08328a82d2bULL,
    0x7f0449b6557f785dULL, 0x62f06210658dcbcbULL, 0xd5a98af9f89c458bULL, 0x77ec69083a346385ULL,
    0xef7ca48bbc27f890ULL, 0x53e9652f61eac532ULL, 0xa71c634abff4f0ccULL, 0xb16f5f0d7e28ea29ULL,
    0xc9dde31d0a003ab2ULL, 0x2ddadf9775902533ULL, 0xe4fa73fb16408b47ULL, 0x90242ebc00d2ee59ULL,
    0xbb02dffd9f381982ULL, 0xdea328364c50907cULL, 0x1395d3b924857cf8ULL, 0x7d3ead0d5aec04e6ULL,
    0xc2f12be3fed74668ULL, 0x0ba3c338f8c3d285ULL,
};
constexpr std::uint64_t kDiag[12] = {
    0xc3b6c08e23ba9300ULL, 0xd84b5de94a324fb6ULL, 0x0d0c371c5b35b84fULL, 0x7964f570e7188037ULL,
    0x5daf18bbd996604bULL, 0x6743bc47b9595257ULL, 0x5528b9362c59bb70ULL, 0xac45e25b7127b68bULL,
    0xa2077d7dfbb606b5ULL, 0xf3faac6faee378aeULL, 0x0c6388b51545e883ULL, 0xd27dbb6944917b60ULL,
};
// clang-format on

using State = std::array<u64, kWidth>;

void external_linear_layer(State& s) noexcept {
  std::array<u128, kWidth> x{};
  for (std::size_t i = 0; i < kWidth; ++i) x[i] = s[i];
  for (std::size_t i = 0; i < kWidth; i += 4) {
    const u128 t01 = x[i] + x[i + 1];
    const u128 t23 = x[i + 2] + x[i + 3];
    const u128 t0123 = t01 + t23;
    const u128 x0 = x[i];
    const u128 x2 = x[i + 2];
    x[i] = t0123 + t01 + x[i + 1];
    x[i + 1] = t0123 + x[i + 1] + x2 + x2;
    x[i + 2] = t0123 + t23 + x[i + 3];
    x[i + 3] = t0123 + x[i + 3] + x0 + x0;
  }
  u128 sums[4];
  for (std::size_t i = 0; i < 4; ++i) sums[i] = x[i] + x[i + 4] + x[i + 8];
  for (std::size_t i = 0; i < kWidth; ++i) s[i] = reduce96(x[i] + sums[i % 4]);
}

void internal_linear_layer(State& s) noexcept {
  u128 sum = 0;
  for (const u64 v : s) sum += v;
  const u64 sum_f = reduce96(sum);
  for (std::size_t i = 0; i < kWidth; ++i)
    s[i] = reduce128(static_cast<u128>(sum_f) + static_cast<u128>(s[i]) * kDiag[i]);
}

inline u64 sbox(u64 x) noexcept {
  const u64 x2 = fsquare(x);
  const u64 x3 = fmul(x2, x);
  return fmul(fsquare(x3), x);
}

void permute(State& s) noexcept {
  external_linear_layer(s);
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t i = 0; i < kWidth; ++i) s[i] = sbox(fadd_canonical(s[i], kExternal[r][i]));
    external_linear_layer(s);
  }
  for (const u64 c : kInternal) {
    s[0] = sbox(fadd_canonical(s[0], c));
    internal_linear_layer(s);
  }
  for (std::size_t r = 4; r < 8; ++r) {
    for (std::size_t i = 0; i < kWidth; ++i) s[i] = sbox(fadd_canonical(s[i], kExternal[r][i]));
    external_linear_layer(s);
  }
}

// HashNToMNoPad(input, 5).
template <class Get>
Fp5 hash5(std::size_t n, Get&& get) noexcept {
  State s{};
  for (std::size_t i = 0; i < n; i += kRate) {
    for (std::size_t j = 0; j < kRate && i + j < n; ++j) s[j] = get(i + j);
    permute(s);
  }
  return {s[0], s[1], s[2], s[3], s[4]};
}

Fp5 hash5(std::span<const u64> in) noexcept {
  return hash5(in.size(), [&](std::size_t i) { return in[i]; });
}

// ---- the scalar field: n ~ 2^319
// ------------------------------------------------------------------

using Scalar = std::array<u64, 5>;
constexpr Scalar kN{0xE80FD996948BFFE1ULL,
                    0xE8885C39D724A09CULL,
                    0x7FFFFFE6CFB80639ULL,
                    0x7FFFFFF100000016ULL,
                    0x7FFFFFFD80000007ULL};
constexpr u64 kN0I = 0xD78BEF72057B7BDFULL;  // -1/N[0] mod 2^64
constexpr Scalar kR2{0xA01001DCE33DC739ULL,  // 2^640 mod n
                     0x6C3228D33F62ACCFULL,
                     0xD1D796CC91CF8525ULL,
                     0xAADFFF5D1574C1D8ULL,
                     0x4ACA13B28CA251F5ULL};

// a - b over 320 bits; the borrow out.
inline bool sub_inner(const Scalar& a, const Scalar& b, Scalar& r) noexcept {
  u64 borrow = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    const u128 d = static_cast<u128>(a[i]) - b[i] - borrow;
    r[i] = static_cast<u64>(d);
    borrow = static_cast<u64>(d >> 64) & 1;
  }
  return borrow != 0;
}

inline void add_inner(const Scalar& a, const Scalar& b, Scalar& r) noexcept {
  u64 carry = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    const u128 s = static_cast<u128>(a[i]) + b[i] + carry;
    r[i] = static_cast<u64>(s);
    carry = static_cast<u64>(s >> 64);
  }
}

// Any 320-bit value mod n (n > 2^318, so at most three subtractions).
Scalar reduce_scalar(Scalar v) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    Scalar r;
    if (sub_inner(v, kN, r)) break;
    v = r;
  }
  return v;
}

// Montgomery product (a * b) / 2^320 mod n; a < n.
Scalar monty_mul(const Scalar& a, const Scalar& b) noexcept {
  Scalar r{};
  for (std::size_t i = 0; i < 5; ++i) {
    const u64 m = b[i];
    const u64 f = (a[0] * m + r[0]) * kN0I;
    u64 cc1 = 0;
    u64 cc2 = 0;
    for (std::size_t j = 0; j < 5; ++j) {
      u128 z = static_cast<u128>(a[j]) * m + r[j] + cc1;
      cc1 = static_cast<u64>(z >> 64);
      z = static_cast<u128>(f) * kN[j] + static_cast<u64>(z) + cc2;
      cc2 = static_cast<u64>(z >> 64);
      if (j > 0) r[j - 1] = static_cast<u64>(z);
    }
    r[4] = cc1 + cc2;
  }
  Scalar r2;
  if (!sub_inner(r, kN, r2)) return r2;
  return r;
}

inline Scalar scalar_mul(const Scalar& a, const Scalar& b) noexcept {
  return monty_mul(monty_mul(a, kR2), b);
}

inline Scalar scalar_sub(const Scalar& a, const Scalar& b) noexcept {
  Scalar r;
  if (sub_inner(a, b, r)) {
    Scalar r2;
    add_inner(r, kN, r2);
    return r2;
  }
  return r;
}

Scalar scalar_from_bytes(const std::uint8_t* in) noexcept {
  Scalar s{};
  for (std::size_t i = 0; i < 5; ++i) {
    u64 v = 0;
    for (std::size_t b = 0; b < 8; ++b) v |= static_cast<u64>(in[i * 8 + b]) << (8 * b);
    s[i] = v;
  }
  return reduce_scalar(s);
}

void scalar_bytes(const Scalar& s, std::uint8_t* out) noexcept {
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<std::uint8_t>(s[i] >> (8 * b));
  }
}

// FromGfp5: the canonical limbs as a 320-bit number, mod n.
Scalar scalar_from_fp5(const Fp5& a) noexcept {
  Scalar s{};
  for (std::size_t i = 0; i < 5; ++i) s[i] = canonical(a[i]);
  return reduce_scalar(s);
}

// ---- ECgFp5 ------------------------------------------------------------------------------------
// (x, z, u, t) fractional coordinates: x = X/Z, u = U/T. The complete formulas of point.go.

struct Point {
  Fp5 x, z, u, t;
};
struct Affine {
  Fp5 x, u;
};

constexpr Fp5 kB{0, 263, 0, 0, 0};
constexpr Fp5 kBMul2{0, 2ULL * 263, 0, 0, 0};
constexpr Fp5 kBMul4{0, 4ULL * 263, 0, 0, 0};
constexpr Fp5 kFour{4, 0, 0, 0, 0};
constexpr Fp5 kA{2, 0, 0, 0, 0};
constexpr Point kNeutral{kZero5, kOne5, kZero5, kOne5};
constexpr Point kGenerator{{12883135586176881569ULL,
                            4356519642755055268ULL,
                            5248930565894896907ULL,
                            2165973894480315022ULL,
                            2448410071095648785ULL},
                           kOne5,
                           kOne5,
                           {4, 0, 0, 0, 0}};

Point point_add(const Point& p, const Point& q) noexcept {
  const Fp5 t1 = mul5(p.x, q.x);
  const Fp5 t2 = mul5(p.z, q.z);
  const Fp5 t3 = mul5(p.u, q.u);
  const Fp5 t4 = mul5(p.t, q.t);
  const Fp5 t5 = sub5(mul5(add5(p.x, p.z), add5(q.x, q.z)), add5(t1, t2));
  const Fp5 t6 = sub5(mul5(add5(p.u, p.t), add5(q.u, q.t)), add5(t3, t4));
  const Fp5 t7 = add5(t1, mul5(t2, kB));
  const Fp5 t8 = mul5(t4, t7);
  const Fp5 t9 = mul5(t3, add5(mul5(t5, kBMul2), dbl5(t7)));
  const Fp5 t10 = mul5(add5(t4, dbl5(t3)), add5(t5, t7));
  return {mul5(sub5(t10, t8), kB), sub5(t8, t9), mul5(t6, sub5(mul5(t2, kB), t1)), add5(t8, t9)};
}

Point point_add_affine(const Point& p, const Affine& q) noexcept {
  const Fp5 t1 = mul5(p.x, q.x);
  const Fp5& t2 = p.z;
  const Fp5 t3 = mul5(p.u, q.u);
  const Fp5& t4 = p.t;
  const Fp5 t5 = add5(p.x, mul5(q.x, p.z));
  const Fp5 t6 = add5(p.u, mul5(q.u, p.t));
  const Fp5 t7 = add5(t1, mul5(t2, kB));
  const Fp5 t8 = mul5(t4, t7);
  const Fp5 t9 = mul5(t3, add5(mul5(t5, kBMul2), dbl5(t7)));
  const Fp5 t10 = mul5(add5(t4, dbl5(t3)), add5(t5, t7));
  return {mul5(sub5(t10, t8), kB), sub5(t8, t9), mul5(t6, sub5(mul5(t2, kB), t1)), add5(t8, t9)};
}

Point point_double(const Point& p) noexcept {
  const Fp5 t1 = mul5(p.z, p.t);
  const Fp5 t2 = mul5(t1, p.t);
  const Fp5 x1 = square5(t2);
  const Fp5 z1 = mul5(t1, p.u);
  const Fp5 t3 = square5(p.u);
  const Fp5 w1 = sub5(t2, mul5(t3, dbl5(add5(p.x, p.z))));
  const Fp5 t4 = square5(z1);
  const Fp5 z = square5(w1);
  return {mul5(t4, kBMul4),
          z,
          sub5(square5(add5(w1, z1)), add5(t4, z)),
          sub5(dbl5(x1), add5(mul5(t4, kFour), z))};
}

Fp5 encode(const Point& p) noexcept {
  return mul5(p.t, inv5(p.u));
}

// Decode (point.go): the point whose encoding is w; false when w encodes none.
bool decode(const Fp5& w, Point& out) noexcept {
  const Fp5 e = sub5(square5(w), kA);
  const Fp5 delta = sub5(square5(e), kBMul4);
  Fp5 r{};
  bool c = sqrt5(delta, r);
  if (c && sgn0_5(r)) r = neg5(r);
  if (!c) r = kZero5;
  const u64 half = finv(2);
  const Fp5 x1 = scalar_mul5(add5(e, r), half);
  const Fp5 x2 = scalar_mul5(sub5(e, r), half);
  const Fp5 x = legendre5(x1) == 1 ? x2 : x1;
  if (c) {
    out = {x, kOne5, kOne5, w};
    return true;
  }
  if (is_zero5(w)) {
    out = kNeutral;
    return true;
  }
  return false;
}

// Affine points with one inversion (Montgomery's trick); none of them may have z * t == 0.
void to_affine(const Point* src, Affine* dst, std::size_t n) noexcept {
  if (n == 0) return;
  std::array<Fp5, 16> prefix{};  // n <= 16
  Fp5 m = kOne5;
  for (std::size_t i = 0; i < n; ++i) {
    prefix[i] = m;
    m = mul5(m, mul5(src[i].z, src[i].t));
  }
  Fp5 inv = inv5(m);
  for (std::size_t i = n; i-- > 0;) {
    const Fp5 zt_inv = mul5(inv, prefix[i]);  // 1 / (z_i t_i)
    dst[i].x = mul5(mul5(src[i].x, src[i].t), zt_inv);
    dst[i].u = mul5(mul5(src[i].u, src[i].z), zt_inv);
    inv = mul5(inv, mul5(src[i].z, src[i].t));
  }
}

// j * 16^w * G for w in 0..80 and j in 0..16 (j = 0 the affine neutral (0, 0)).
struct GeneratorTable {
  std::array<std::array<Affine, 16>, 80> rows{};
  GeneratorTable() noexcept {
    Point base = kGenerator;
    for (auto& row : rows) {
      std::array<Point, 15> multiples{};
      multiples[0] = base;
      for (std::size_t j = 1; j < multiples.size(); ++j)
        multiples[j] = point_add(multiples[j - 1], base);
      row[0] = Affine{kZero5, kZero5};
      to_affine(multiples.data(), row.data() + 1, multiples.size());
      base = point_double(point_double(point_double(point_double(base))));
    }
  }
};

const GeneratorTable& generator_table() noexcept {
  static const GeneratorTable table;
  return table;
}

// k * G, every window added (the digit selected without branching on it).
Point mul_generator(const Scalar& k) noexcept {
  const GeneratorTable& t = generator_table();
  Point p = kNeutral;
  for (std::size_t w = 0; w < 80; ++w) {
    const auto digit = static_cast<unsigned>((k[w / 16] >> (4 * (w % 16))) & 0xF);
    Affine sel{kZero5, kZero5};
    for (unsigned j = 0; j < 16; ++j) {
      const u64 mask = 0 - static_cast<u64>(j == digit);
      for (std::size_t l = 0; l < 5; ++l) {
        sel.x[l] |= t.rows[static_cast<std::size_t>(w)][j].x[l] & mask;
        sel.u[l] |= t.rows[static_cast<std::size_t>(w)][j].u[l] & mask;
      }
    }
    p = point_add_affine(p, sel);
  }
  return p;
}

// k * P, variable time (public values only).
Point mul_point(const Point& p, const Scalar& k) noexcept {
  Point r = kNeutral;
  for (std::size_t bit = 320; bit-- > 0;) {
    r = point_double(r);
    if (((k[bit / 64] >> (bit % 64)) & 1) != 0) r = point_add(r, p);
  }
  return r;
}

// e = H(r || m) as a scalar.
Scalar challenge(const Fp5& r, const Fp5& m) noexcept {
  const u64 pre[10] = {r[0], r[1], r[2], r[3], r[4], m[0], m[1], m[2], m[3], m[4]};
  return scalar_from_fp5(hash5(pre));
}

void sign_inner(
    const Scalar& sk, const Fp5& m, const Scalar& k, const Fp5& r, SigBytes& out) noexcept {
  const Scalar e = challenge(r, m);
  const Scalar s = scalar_sub(k, scalar_mul(e, sk));
  scalar_bytes(s, out.data());
  scalar_bytes(e, out.data() + 40);
}

// A uniform scalar in [1, n): 319 random bits, retried when >= n or 0.
bool random_scalar(Scalar& out) noexcept {
  for (std::size_t attempt = 0; attempt < 64; ++attempt) {
    std::uint8_t buf[40];
    std::size_t got = 0;
    while (got < sizeof buf) {
      const ssize_t r = getrandom(buf + got, sizeof buf - got, 0);
      if (r <= 0) return false;
      got += static_cast<std::size_t>(r);
    }
    Scalar s{};
    for (std::size_t i = 0; i < 5; ++i) {
      u64 v = 0;
      for (std::size_t b = 0; b < 8; ++b) v |= static_cast<u64>(buf[i * 8 + b]) << (8 * b);
      s[i] = v;
    }
    s[4] &= 0x7FFFFFFFFFFFFFFFULL;
    Scalar tmp;
    const bool below_n = sub_inner(s, kN, tmp);
    std::memset(buf, 0, sizeof buf);
    if (below_n && (s[0] | s[1] | s[2] | s[3] | s[4]) != 0) {
      out = s;
      return true;
    }
  }
  return false;
}

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Fp5Bytes hash_to_quintic(std::span<const std::uint64_t> elems) noexcept {
  Fp5Bytes out{};
  fp5_bytes(hash5(elems), out.data());
  return out;
}

Fp5Bytes hash_bytes_to_quintic(std::string_view bytes) noexcept {
  const std::size_t n = (bytes.size() + 7) / 8;
  const Fp5 h = hash5(n, [&](std::size_t i) {
    u64 v = 0;
    for (std::size_t b = 0; b < 8 && i * 8 + b < bytes.size(); ++b)
      v |= static_cast<u64>(static_cast<std::uint8_t>(bytes[i * 8 + b])) << (8 * b);
    return v;
  });
  Fp5Bytes out{};
  fp5_bytes(h, out.data());
  return out;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    s[2 * i] = kDigits[bytes[i] >> 4];
    s[2 * i + 1] = kDigits[bytes[i] & 0xF];
  }
  return s;
}

bool from_hex(std::string_view s, std::span<std::uint8_t> out) noexcept {
  if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
  if (s.size() != out.size() * 2) return false;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = hex_digit(s[2 * i]);
    const int lo = hex_digit(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return true;
}

Fp5Bytes generator_mul(const ScalarBytes& k) noexcept {
  Fp5Bytes out{};
  fp5_bytes(encode(mul_generator(scalar_from_bytes(k.data()))), out.data());
  return out;
}

Signer::Signer(std::string_view private_key_hex) {
  ScalarBytes raw{};
  if (!from_hex(private_key_hex, raw)) {
    error_ = "the private key is not 80 hex digits";
    return;
  }
  const Scalar sk = scalar_from_bytes(raw.data());
  raw.fill(0);
  if ((sk[0] | sk[1] | sk[2] | sk[3] | sk[4]) == 0) {
    error_ = "the private key is zero";
    return;
  }
  sk_ = sk;
  fp5_bytes(encode(mul_generator(sk)), pk_.data());
  usable_ = true;
}

Signer::~Signer() {
  volatile std::uint64_t* p = sk_.data();
  for (std::size_t i = 0; i < sk_.size(); ++i) p[i] = 0;
  for (Nonce& n : pool_) n = Nonce{};
}

Signer::Signer(Signer&& o) noexcept
    : sk_(o.sk_),
      pk_(o.pk_),
      pool_(std::move(o.pool_)),
      usable_(o.usable_),
      error_(std::move(o.error_)) {
  o.sk_.fill(0);
  o.usable_ = false;
}

Signer& Signer::operator=(Signer&& o) noexcept {
  if (this != &o) {
    sk_ = o.sk_;
    pk_ = o.pk_;
    pool_ = std::move(o.pool_);
    usable_ = o.usable_;
    error_ = std::move(o.error_);
    o.sk_.fill(0);
    o.usable_ = false;
  }
  return *this;
}

bool Signer::sign(const Fp5Bytes& message, SigBytes& out) const noexcept {
  if (!usable_) return false;
  Nonce n;
  if (!pool_.empty()) {
    n = pool_.back();
    pool_.back() = Nonce{};
    pool_.pop_back();
  } else {
    if (!random_scalar(n.k)) return false;
    n.r = encode(mul_generator(n.k));
  }
  sign_inner(sk_, fp5_from_bytes(message.data()), n.k, n.r, out);
  n.k.fill(0);
  return true;
}

void Signer::prepare(std::size_t n) {
  if (!usable_) return;
  pool_.reserve(n);
  while (pool_.size() < n) {
    Nonce x;
    if (!random_scalar(x.k)) return;
    x.r = encode(mul_generator(x.k));
    pool_.push_back(x);
  }
}

bool Signer::sign_with_nonce(const Fp5Bytes& message,
                             const ScalarBytes& k,
                             SigBytes& out) const noexcept {
  if (!usable_) return false;
  const Scalar ks = scalar_from_bytes(k.data());
  sign_inner(sk_, fp5_from_bytes(message.data()), ks, encode(mul_generator(ks)), out);
  return true;
}

std::string Signer::generate_private_key_hex() {
  Scalar s{};
  if (!random_scalar(s)) return {};
  ScalarBytes b{};
  scalar_bytes(s, b.data());
  std::string hex = to_hex(b);
  b.fill(0);
  return hex;
}

bool verify(const Fp5Bytes& public_key, const Fp5Bytes& message, const SigBytes& sig) noexcept {
  // s and e must be canonical (below n).
  Scalar s{};
  Scalar e{};
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t b = 0; b < 8; ++b) {
      s[i] |= static_cast<u64>(sig[i * 8 + b]) << (8 * b);
      e[i] |= static_cast<u64>(sig[40 + i * 8 + b]) << (8 * b);
    }
  }
  Scalar tmp;
  if (!sub_inner(s, kN, tmp) || !sub_inner(e, kN, tmp)) return false;
  Point pk{};
  if (!decode(fp5_from_bytes(public_key.data()), pk)) return false;
  const Fp5 r = encode(point_add(mul_generator(s), mul_point(pk, e)));
  const Scalar ev = challenge(r, fp5_from_bytes(message.data()));
  return ev == e;
}

}  // namespace fastmm::venues::lighter
