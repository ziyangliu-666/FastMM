// Lighter signing against the official Go signer (lighter-go, poseidon_crypto v0.0.15): the
// Poseidon2 hash, the public key, k * G, every transaction's hash, signature and tx_info, the
// ChangePubKey L1 text and the auth token, byte for byte. The vectors come from
// tests/fixtures/lighter/gen_signing.go with a fixed private key and nonce (the Go signer's
// SchnorrSignHashedMessage2); the key controls no account. Account 281474976499586 is a
// sub-account index, chain 304 mainnet.
#include "test_support.hpp"

#include "fastmm/venues/lighter/lighter_crypto.hpp"
#include "fastmm/venues/lighter/lighter_tx.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm::venues::lighter;

namespace {

constexpr std::string_view kSk =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728";
constexpr std::string_view kPk =
    "710c8cd2201061fa5570d20852d90ddabd6c496825b639d6c6486ecb1f859b63f237dee54f3f42b4";
constexpr std::string_view kK =
    "c0a217100fcd97c014128fdd7b5a2fd087bc0bf5dec6c748bbd2d3d4e4d6d758dae2e364e8e6e768";
constexpr std::string_view kKG =
    "b52c80b082b1000366b92272cd7b32a4cfb9bef28d583362e741eaa3f58785aa938a37eba9cb035e";

struct HashVector {
  std::size_t n;
  std::string_view out;
};
// Inputs i * 0x9E3779B97F4A7C15 + 7 for i < n (mod 2^64).
constexpr HashVector kHashes[] = {
    {1, "07c88ae106f09f5279373487d2c5a22b5d9815298a01f9a8631c528e55ceddbbe7c54009653393a6"},
    {5, "0b5f49b317c29a1476e37d51dbc89f433e590d4ce4cd2e8cb53841fd0603667eb522349f79250b87"},
    {8, "6729aadef4bba5c9b52af8109fd56fd6fe7ba8e45699f068f71a8b415b99a129d771b599202d7dbc"},
    {9, "3db0b028b1ebb16c877d1198bc7930e36125ff335e61fd9d8e4dcc196aaeb9f697ca6464c8a085bb"},
    {16, "cb898b43602add13736d2966b7888240e34adea987612f3bb4289ee7877e025590466b4aecd439fb"},
    {17, "e8e5137f715cf0245f07ca2ecadc355699a3dafe173d0780ff70a0cf60ff8d7069b74837495c5d6b"},
};

struct TxVector {
  std::string_view name;
  std::string_view hash;
  std::string_view sig;
  std::string_view info;
};
// clang-format off
constexpr TxVector kTx[] = {
    {"create_order",
     "c0189050371b87221476996a3160f6a8f145e7357171af6e9a2c39a00bac56f2f0ff84458f5188de",
     "09510f63bbf20c40fec2b637619fd3277eacabf163e81dc4a7a72babc4fd1e3d5958b93fc105501ac47a220b9580d47fdf640037734d70f6b1052bd1c32a4aa8b18855f2d57b940005c3ada98a88ef1e",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"MarketIndex":110,"ClientOrderIndex":123456789012,"BaseAmount":87,"Price":2312345,"IsAsk":1,"Type":0,"TimeInForce":2,"ReduceOnly":0,"TriggerPrice":0,"OrderExpiry":1793000000000,"ExpiredAt":1791800000000,"Nonce":77,"Sig":"CVEPY7vyDED+wrY3YZ/TJ36sq/Fj6B3Ep6crq8T9Hj1ZWLk/wQVQGsR6IguVgNR/32QAN3NNcPaxBSvRwypKqLGIVfLVe5QABcOtqYqI7x4=","L2TxAttributes":null})"},
    {"create_order_ioc_skipnonce",
     "8d17f63468f9276c6f3ff3964130b4b17cab9646d8902a332bc7f4733a013043ffb152311166e150",
     "73be31605d65d4687b6eed36f0307397ce94a3eeb694e5a7cd34205e8a68aaa58c3e884f7b40b01a9c8028004e19a585e18d1aee3a86a5aec46fef44747f988a0330586d3693b1bf4d2f79cd02098527",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"MarketIndex":1,"ClientOrderIndex":5,"BaseAmount":1000,"Price":900000,"IsAsk":0,"Type":0,"TimeInForce":0,"ReduceOnly":1,"TriggerPrice":0,"OrderExpiry":0,"ExpiredAt":1791800000001,"Nonce":78,"Sig":"c74xYF1l1Gh7bu028DBzl86Uo+62lOWnzTQgXopoqqWMPohPe0CwGpyAKABOGaWF4Y0a7jqGpa7Eb+9EdH+YigMwWG02k7G/TS95zQIJhSc=","L2TxAttributes":{"4":1}})"},
    {"cancel_order",
     "b5de961786a550bc64498a9f5fcca0b73b971987334c4df0d689cc3e9dc752788b0b04463f5b5d34",
     "09badbed7d87434c901064f189c61d07aebc49fd047d84dda2562576db8b42ef934976819f4a185a051b7425d09cbc8f40ce44b430e83a3cd957152da340cc1775c1a628774b8b1e9be401242e4a3e0c",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"MarketIndex":110,"Index":123456789012,"ExpiredAt":1791800000002,"Nonce":79,"Sig":"Cbrb7X2HQ0yQEGTxicYdB668Sf0EfYTdolYldtuLQu+TSXaBn0oYWgUbdCXQnLyPQM5EtDDoOjzZVxUto0DMF3XBpih3S4sem+QBJC5KPgw=","L2TxAttributes":null})"},
    {"modify_order",
     "90abafb27e060cb236b2d7c0f24a45a8a43eb5341b70d3bde0f6037f3ac14a84c49341c5cf216d7c",
     "b28adb0c9810e8e3a22cd60367a5b4d0b82d3861c230adb36731b09e84482e1b8f876a78f70525543b64ce41624dedae184d53fe88b4636e0766ea536d90fff35420aeaa4f7d96acf8ccc7afa87fd81d",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"MarketIndex":110,"Index":123456789012,"BaseAmount":90,"Price":2312000,"TriggerPrice":0,"ExpiredAt":1791800000003,"Nonce":80,"Sig":"sorbDJgQ6OOiLNYDZ6W00LgtOGHCMK2zZzGwnoRILhuPh2p49wUlVDtkzkFiTe2uGE1T/oi0Y24HZupTbZD/81QgrqpPfZas+MzHr6h/2B0=","L2TxAttributes":null})"},
    {"cancel_all_scheduled",
     "a8bc7d24b71834abb6960b36925202fe037f929637aced39025a1c7b4c311c7f6cd81863a5ad3487",
     "cd78eccd23e38690bc310112f547119311fbcbbbeb275d7c75c68d8029fad033154ebb096f836c73e428b6719debb28a46b1ff5e793bd45459613c9fe73002c5b97b2c6ff1ff7fda9272780053ed4e00",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"TimeInForce":1,"Time":1791800600000,"ExpiredAt":1791800000004,"Nonce":81,"Sig":"zXjszSPjhpC8MQES9UcRkxH7y7vrJ118dcaNgCn60DMVTrsJb4Nsc+QotnGd67KKRrH/Xnk71FRZYTyf5zACxbl7LG/x/3/aknJ4AFPtTgA=","L2TxAttributes":null})"},
    {"cancel_all_now",
     "8945eaf20aaa9d45e41290df4fcaa354e9b541f98f71065c11482c5bfb71d43c31eee81e8914af19",
     "cba15bdae43197a17193153642eb43aef8ac71bc961823c1b9a4bd4199e13e94ed7d6b5a67fe170113b20d1d59888bb372a1dfe2796a97e163015d9ccbbcb9b366205cb3ff3588e65c3b27c06b2f212d",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"TimeInForce":0,"Time":0,"ExpiredAt":1791800000005,"Nonce":82,"Sig":"y6Fb2uQxl6FxkxU2QutDrviscbyWGCPBuaS9QZnhPpTtfWtaZ/4XAROyDR1ZiIuzcqHf4nlql+FjAV2cy7y5s2YgXLP/NYjmXDsnwGsvIS0=","L2TxAttributes":null})"},
    {"change_pub_key",
     "c2b4c6013ac7454d6799834af63dace004833873bc40ea68867ac1ba03a12c1296e859adbf3ec8f0",
     "cd6d4f8b5be66bcc0bf5358c4ae382f06301e0b2881d11445a512e959bb27d8c0e5474e7cee03a1ebd74c5a04c894efe7d1708cceba52fa88696c45bcd0be7406edc957f347917bc5ae47d4926b3271b",
     R"({"AccountIndex":281474976499586,"ApiKeyIndex":3,"PubKey":"cQyM0iAQYfpVcNIIUtkN2r1sSWgltjnWxkhuyx+Fm2PyN97lTz9CtA==","L1Sig":"","ExpiredAt":1791800000006,"Nonce":0,"Sig":"zW1Pi1vma8wL9TWMSuOC8GMB4LKIHRFEWlEulZuyfYwOVHTnzuA6Hr10xaBMiU7+fRcIzOulL6iGlsRbzQvnQG7clX80eRe8WuR9SSazJxs=","L2TxAttributes":null})"},
};
// clang-format on
constexpr std::string_view kAuthMsg = "1791800600:281474976499586:3";
constexpr std::string_view kAuthHash =
    "9cf0365ab69ff555d11f850e7164c1c60ecfe62d1686396b2f6535d6d45ffd704d201e97a3d888b7";
constexpr std::string_view kAuthSig =
    "e23a86ba11ccabf55b16bf3f3fd8492a81f9d50ab4a07ca78becdfbbb24672eb7b2979a242bebc5b764bfcbffb088c"
    "93c742e7bedc88e33b45ce8ec37646eb1623a787cb39fcf536e134693f305adf3a";
constexpr std::string_view kL1Body =
    "Register Lighter Account\n\npubkey: "
    "0x710c8cd2201061fa5570d20852d90ddabd6c496825b639d6c6486ecb1f859b63f237dee54f3f42b4\nnonce: "
    "0x0000000000000000\naccount index: 0x0000fffffffcc782\napi key index: "
    "0x0000000000000003\nOnly sign this message for a trusted client!";

constexpr std::int64_t kAccount = 281474976499586;
constexpr std::uint8_t kApiKey = 3;

template <std::size_t N>
std::array<std::uint8_t, N> bytes(std::string_view hex) {
  std::array<std::uint8_t, N> b{};
  REQUIRE(from_hex(hex, b));
  return b;
}

std::string info_of(std::size_t n, const char* buf) {
  return std::string(buf, n);
}

const TxVector& vec(std::string_view name) {
  for (const TxVector& v : kTx) {
    if (v.name == name) return v;
  }
  FAIL("no vector " << name);
  return kTx[0];
}

// Hash, signature with the fixed nonce and tx_info of one transaction against its vector.
template <class Tx>
void check(const Signer& s, Tx tx, std::string_view name) {
  const TxVector& v = vec(name);
  const Fp5Bytes h = tx_hash(tx, kMainnetChainId);
  CHECK(to_hex(h) == v.hash);
  SigBytes sig{};
  REQUIRE(s.sign_with_nonce(h, bytes<40>(kK), sig));
  CHECK(to_hex(sig) == v.sig);
  char buf[kMaxTxInfoBytes];
  const std::size_t n = write_tx_info(tx, sig, buf);
  REQUIRE(n > 0);
  CHECK(info_of(n, buf) == v.info);
  CHECK(verify(s.public_key(), h, sig));
}

TxHeader header(std::int64_t expired_at, std::int64_t nonce) {
  TxHeader h;
  h.account_index = kAccount;
  h.api_key_index = kApiKey;
  h.expired_at = expired_at;
  h.nonce = nonce;
  return h;
}

}  // namespace

TEST_CASE("lighter.signing: Poseidon2 over 1 to 17 elements") {
  for (const HashVector& v : kHashes) {
    std::vector<std::uint64_t> in(v.n);
    for (std::size_t i = 0; i < v.n; ++i) in[i] = i * 0x9E3779B97F4A7C15ULL + 7;
    CHECK(to_hex(hash_to_quintic(in)) == v.out);
  }
}

TEST_CASE("lighter.signing: the public key and k * G") {
  const Signer s(kSk);
  REQUIRE(s.usable());
  CHECK(s.public_key_hex() == kPk);
  CHECK(to_hex(generator_mul(bytes<40>(kK))) == kKG);
  // "0x" is accepted; anything but 80 hex digits is not.
  CHECK(Signer(std::string("0x") + std::string(kSk)).public_key_hex() == kPk);
  CHECK_FALSE(Signer(kSk.substr(2)).usable());
  CHECK_FALSE(Signer(std::string(80, '0')).usable());
}

TEST_CASE("lighter.signing: transactions match the Go signer") {
  const Signer s(kSk);
  REQUIRE(s.usable());
  {
    CreateOrderTx t;
    t.h = header(1791800000000, 77);
    t.market_index = 110;
    t.client_order_index = 123456789012;
    t.base_amount = 87;
    t.price = 2312345;
    t.is_ask = 1;
    t.type = kOrderLimit;
    t.time_in_force = kTifPostOnly;
    t.order_expiry = 1793000000000;
    check(s, t, "create_order");
  }
  {
    CreateOrderTx t;
    t.h = header(1791800000001, 78);
    REQUIRE(t.h.attributes.add(kAttrSkipNonce, 1));
    t.market_index = 1;
    t.client_order_index = 5;
    t.base_amount = 1000;
    t.price = 900000;
    t.is_ask = 0;
    t.time_in_force = kTifIoc;
    t.reduce_only = 1;
    t.order_expiry = 0;
    check(s, t, "create_order_ioc_skipnonce");
  }
  {
    CancelOrderTx t;
    t.h = header(1791800000002, 79);
    t.market_index = 110;
    t.index = 123456789012;
    check(s, t, "cancel_order");
  }
  {
    ModifyOrderTx t;
    t.h = header(1791800000003, 80);
    t.market_index = 110;
    t.index = 123456789012;
    t.base_amount = 90;
    t.price = 2312000;
    check(s, t, "modify_order");
  }
  {
    CancelAllTx t;
    t.h = header(1791800000004, 81);
    t.time_in_force = kCancelAllScheduled;
    t.time = 1791800600000;
    check(s, t, "cancel_all_scheduled");
  }
  {
    CancelAllTx t;
    t.h = header(1791800000005, 82);
    t.time_in_force = kCancelAllNow;
    check(s, t, "cancel_all_now");
  }
  {
    ChangePubKeyTx t;
    t.h = header(1791800000006, 0);
    t.pub_key = s.public_key();
    check(s, t, "change_pub_key");
    CHECK(l1_body(t) == kL1Body);
  }
}

TEST_CASE("lighter.signing: the auth token's hash and signature") {
  const Signer s(kSk);
  const Fp5Bytes h = hash_bytes_to_quintic(kAuthMsg);
  CHECK(to_hex(h) == kAuthHash);
  SigBytes sig{};
  REQUIRE(s.sign_with_nonce(h, bytes<40>(kK), sig));
  CHECK(to_hex(sig) == kAuthSig);
  // A real token: the message, then a signature over it with a random nonce.
  const std::string token = auth_token(s, 1791800600, kAccount, kApiKey);
  REQUIRE(token.starts_with(std::string(kAuthMsg) + ":"));
  const std::string_view sig_hex = std::string_view(token).substr(kAuthMsg.size() + 1);
  SigBytes got{};
  REQUIRE(from_hex(sig_hex, got));
  CHECK(verify(s.public_key(), h, got));
}

TEST_CASE("lighter.signing: random nonces verify, tampering does not") {
  const Signer s(kSk);
  const Fp5Bytes h = bytes<40>(vec("create_order").hash);
  SigBytes a{};
  SigBytes b{};
  REQUIRE(s.sign(h, a));
  REQUIRE(s.sign(h, b));
  CHECK(a != b);
  CHECK(verify(s.public_key(), h, a));
  CHECK(verify(s.public_key(), h, b));
  SigBytes bad = a;
  bad[3] ^= 1;
  CHECK_FALSE(verify(s.public_key(), h, bad));
  Fp5Bytes other = h;
  other[0] ^= 1;
  CHECK_FALSE(verify(s.public_key(), other, a));
  // A generated key signs too.
  const std::string fresh = Signer::generate_private_key_hex();
  REQUIRE(fresh.size() == 80);
  Signer g(fresh);
  REQUIRE(g.usable());
  SigBytes c{};
  REQUIRE(g.sign(h, c));
  CHECK(verify(g.public_key(), h, c));
  CHECK_FALSE(verify(s.public_key(), h, c));
  // Prepared nonces sign the same way, each once.
  g.prepare(2);
  CHECK(g.prepared() == 2);
  SigBytes d{};
  SigBytes e{};
  REQUIRE(g.sign(h, d));
  REQUIRE(g.sign(h, e));
  CHECK(g.prepared() == 0);
  CHECK(d != e);
  CHECK(verify(g.public_key(), h, d));
  CHECK(verify(g.public_key(), h, e));
}

TEST_CASE("lighter.signing: base64 as Go's encoding/json writes []byte") {
  const std::uint8_t in[] = {0xff, 0x00, 0x10, 0x61, 0x62};
  char out[16];
  CHECK(std::string_view(out, base64(std::span(in, 0), out)).empty());
  CHECK(std::string_view(out, base64(std::span(in, 1), out)) == "/w==");
  CHECK(std::string_view(out, base64(std::span(in, 2), out)) == "/wA=");
  CHECK(std::string_view(out, base64(std::span(in, 3), out)) == "/wAQ");
  CHECK(std::string_view(out, base64(std::span(in, 5), out)) == "/wAQYWI=");
}
