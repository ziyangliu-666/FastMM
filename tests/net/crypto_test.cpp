#include "fastmm/net/crypto.hpp"

#include "test_support.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace fastmm::net;

namespace {

std::string hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s;
  for (auto b : bytes) {
    s += kDigits[b >> 4];
    s += kDigits[b & 15];
  }
  return s;
}

}  // namespace

TEST_CASE("crypto: HMAC-SHA256 RFC 4231 vectors") {
  Sha256Digest d{};

  SUBCASE("case 1") {
    const std::string key(20, '\x0b');
    REQUIRE(hmac_sha256(key, "Hi There", d));
    CHECK(hex(d) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  }
  SUBCASE("case 2") {
    REQUIRE(hmac_sha256("Jefe", "what do ya want for nothing?", d));
    CHECK(hex(d) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(hmac_sha256_hex("Jefe", "what do ya want for nothing?").view() ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  }
  SUBCASE("case 3") {
    const std::string key(20, '\xaa');
    const std::string data(50, '\xdd');
    REQUIRE(hmac_sha256(key, data, d));
    CHECK(hex(d) == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
  }
  SUBCASE("case 4") {
    std::string key;
    for (int i = 1; i <= 25; ++i) key.push_back(static_cast<char>(i));
    const std::string data(50, '\xcd');
    REQUIRE(hmac_sha256(key, data, d));
    CHECK(hex(d) == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
  }
  SUBCASE("case 6: key longer than block size") {
    const std::string key(131, '\xaa');
    REQUIRE(hmac_sha256(key, "Test Using Larger Than Block-Size Key - Hash Key First", d));
    CHECK(hex(d) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  }
  SUBCASE("hex string is NUL-terminated and 64 chars") {
    auto h = hmac_sha256_hex("k", "v");
    CHECK(h.size() == 64);
    CHECK(std::string_view(h.c_str()).size() == 64);
  }
}

TEST_CASE("crypto: SHA-1 / SHA-256 known answers") {
  CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("crypto: base64 RFC 4648 vectors") {
  const std::vector<std::pair<std::string, std::string>> vectors = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
  };
  for (const auto& [plain, encoded] : vectors) {
    CAPTURE(plain);
    CHECK(base64_encode(plain) == encoded);
    std::string decoded;
    REQUIRE(base64_decode(encoded, decoded));
    CHECK(decoded == plain);
  }

  SUBCASE("fixed-buffer encode") {
    char out[8];
    const std::uint8_t in[] = {'f', 'o', 'o'};
    CHECK(base64_encode(std::span<const std::uint8_t>(in), std::span<char>(out)) == 4);
    CHECK(std::string_view(out, 4) == "Zm9v");
    CHECK(base64_encode(std::span<const std::uint8_t>(in), std::span<char>(out, 3)) == 0);
  }
  SUBCASE("binary round trip incl. high bytes") {
    std::string bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
    std::string decoded;
    REQUIRE(base64_decode(base64_encode(bin), decoded));
    CHECK(decoded == bin);
  }
  SUBCASE("malformed input is rejected") {
    std::string out;
    CHECK_FALSE(base64_decode("Zm9", out));       // length not multiple of 4
    CHECK_FALSE(base64_decode("Zm9v!A==", out));  // invalid symbol
    CHECK_FALSE(base64_decode("Z===", out));      // too much padding
    CHECK_FALSE(base64_decode("Zg==Zg==", out));  // padding not at the end
    CHECK_FALSE(base64_decode("Zm9=v", out));     // wrong length
  }
}

TEST_CASE("crypto: random_bytes fills the buffer") {
  std::uint8_t a[32] = {};
  std::uint8_t b[32] = {};
  REQUIRE(random_bytes(a));
  REQUIRE(random_bytes(b));
  CHECK(std::string_view(reinterpret_cast<const char*>(a), 32) !=
        std::string_view(reinterpret_cast<const char*>(b), 32));
  CHECK(random_bytes(std::span<std::uint8_t>{}));
}

TEST_CASE("crypto: ed25519 signature matches openssl pkeyutl") {
  // Test-only key generated with `openssl genpkey -algorithm ed25519`.
  constexpr std::string_view kPem =
      "-----BEGIN PRIVATE KEY-----\n"
      "MC4CAQAwBQYDK2VwBCIEIMiNHZWMX5DqXpUqYby34Xu7FODcF13zDfu9PXiBvdtZ\n"
      "-----END PRIVATE KEY-----\n";
  CHECK(ed25519_sign_base64(kPem, "hello fastmm") ==
        "BCNp/ayIaamIRXyK+pXncZr5dCxdXEFOu69LPyYJdiRRNs6UeW3U4VihzyuVRyafhQR1JzCzGq2vy+YlFBxSDg==");
  CHECK(ed25519_sign_base64("not a pem", "x").empty());
}

TEST_CASE("crypto: HmacSha256Key reuses the pad midstates across messages") {
  const HmacSha256Key key("Jefe");
  CHECK(key.sign_hex("what do ya want for nothing?").view() ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Signing does not disturb the stored state: same answer twice.
  CHECK(key.sign_hex("what do ya want for nothing?").view() ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Messages across SHA-256 block boundaries match the one-shot form.
  for (const std::size_t n : {0UL, 1UL, 55UL, 56UL, 63UL, 64UL, 65UL, 200UL, 1000UL}) {
    const std::string msg(n, 'x');
    CHECK(key.sign_hex(msg).view() == hmac_sha256_hex("Jefe", msg).view());
  }
  const HmacSha256Key copy = key;
  CHECK(copy.sign_hex("abc").view() == key.sign_hex("abc").view());
  CHECK(HmacSha256Key().sign_hex("").view() ==
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");  // HMAC("", "")
}

TEST_CASE("crypto: Ed25519Key signs, verifies and exports the public key") {
  const std::string priv = fastmm::test::fixture("binance/ed25519-test-private.pem");
  const std::string pub_pem = fastmm::test::fixture("binance/ed25519-test-public.pem");
  const Ed25519Key key = Ed25519Key::from_private_pem(priv);
  REQUIRE(key.has_private());
  CHECK(key.public_pem() == pub_pem);
  // Deterministic signatures (RFC 8032): same value as the one-shot helper.
  CHECK(key.sign_base64("hello fastmm") ==
        "BCNp/ayIaamIRXyK+pXncZr5dCxdXEFOu69LPyYJdiRRNs6UeW3U4VihzyuVRyafhQR1JzCzGq2vy+YlFBxSDg==");
  const Ed25519Key pub = Ed25519Key::from_public_pem(pub_pem);
  REQUIRE(pub.valid());
  CHECK_FALSE(pub.has_private());
  const std::string sig = key.sign_base64("payload");
  CHECK(pub.verify_base64("payload", sig));
  CHECK_FALSE(pub.verify_base64("payload!", sig));
  CHECK_FALSE(pub.verify_base64("payload", "not base64"));
  CHECK_FALSE(pub.verify_base64("payload", base64_encode(std::string(63, 'a'))));
  CHECK(pub.sign_base64("x").empty());  // no private key
  char small[10];
  CHECK(key.sign_base64("x", std::span<char>(small)) == 0);
  CHECK_FALSE(Ed25519Key::from_private_pem("junk").valid());
  CHECK_FALSE(Ed25519Key::from_public_pem(priv).valid());
  Ed25519Key moved = Ed25519Key::from_private_pem(priv);
  Ed25519Key target;
  target = std::move(moved);
  CHECK(target.has_private());
}

TEST_CASE("crypto: base64url without padding, RFC 4648 section 5") {
  CHECK(base64url_encode("").empty());
  CHECK(base64url_encode("f") == "Zg");
  CHECK(base64url_encode("fo") == "Zm8");
  CHECK(base64url_encode("foo") == "Zm9v");
  CHECK(base64url_encode("foob") == "Zm9vYg");
  CHECK(base64url_encode(std::string("\xfb\xff\xfe", 3)) == "-__-");  // "+//+" in base64
  std::array<std::uint8_t, 8> out{};
  CHECK(base64url_decode("-__-", out) == 3);
  CHECK(out[0] == 0xfb);
  CHECK(out[2] == 0xfe);
  CHECK(base64url_decode("Zm9vYg", out) == 4);
  CHECK(base64url_decode("Zm9vYg==", out) == SIZE_MAX);  // padding is not base64url's
  CHECK(base64url_decode("+//+", out) == SIZE_MAX);
  CHECK(base64url_decode("Z", out) == SIZE_MAX);
}

namespace {

// RFC 6979 A.2.5, the P-256 key, as SEC 1 and SubjectPublicKeyInfo PEM.
constexpr const char* kP256Private =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49\n"
    "AwEHoUQDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Z5A/4QCLi8\n"
    "maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END EC PRIVATE KEY-----\n";
constexpr const char* kP256Public =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7\n"
    "Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END PUBLIC KEY-----\n";

std::vector<std::uint8_t> unhex(std::string_view h) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2)
    out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
  return out;
}

}  // namespace

TEST_CASE("crypto: EcdsaP256Key verifies RFC 6979 and PyJWT signatures and its own") {
  const EcdsaP256Key key = EcdsaP256Key::from_private_pem(kP256Private);
  REQUIRE(key.has_private());
  CHECK(key.public_pem() == kP256Public);
  const EcdsaP256Key pub = EcdsaP256Key::from_public_pem(kP256Public);
  REQUIRE(pub.valid());
  CHECK_FALSE(pub.has_private());
  // RFC 6979 A.2.5, SHA-256, message "sample": r || s.
  const std::vector<std::uint8_t> rfc = unhex(
      "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716"
      "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8");
  CHECK(pub.verify("sample", rfc));
  CHECK_FALSE(pub.verify("samples", rfc));
  // An ES256 JWT made by PyJWT 2.10.1 with this key: the signature over "header.payload".
  const std::string jwt =
      "eyJhbGciOiJFUzI1NiIsImtpZCI6Im9yZ2FuaXphdGlvbnMvby9hcGlLZXlzL2siLCJub25jZSI6IjAwMTEyMjMzND"
      "Q1NTY2Nzc4ODk5YWFiYmNjZGRlZWZmIiwidHlwIjoiSldUIn0.eyJzdWIiOiJvcmdhbml6YXRpb25zL28vYXBpS2V5"
      "cy9rIiwiaXNzIjoiY2RwIiwibmJmIjoxNzkwMDAwMDAwLCJleHAiOjE3OTAwMDAxMjAsInVyaSI6IkdFVCBhcGkuY29p"
      "bmJhc2UuY29tL2FwaS92My9icm9rZXJhZ2UvYWNjb3VudHMifQ.QQvhyJgPTEnf_PyddxmFW4QBTlEhXDwfKfIsjYog_"
      "EqTNUqeAg7qcDeW7MaSEpfLvBKrwBeztt42OoZ0fDge7Q";
  const std::size_t dot = jwt.rfind('.');
  std::array<std::uint8_t, 70> sig{};
  REQUIRE(base64url_decode(std::string_view(jwt).substr(dot + 1), sig) == kEs256SignatureSize);
  CHECK(pub.verify(std::string_view(jwt).substr(0, dot),
                   std::span<const std::uint8_t>(sig.data(), kEs256SignatureSize)));
  // Its own: random nonces, so two signatures differ and both verify.
  std::array<std::uint8_t, kEs256SignatureSize> a{};
  std::array<std::uint8_t, kEs256SignatureSize> b{};
  REQUIRE(key.sign("header.payload", a));
  REQUIRE(key.sign("header.payload", b));
  CHECK(a != b);
  CHECK(pub.verify("header.payload", a));
  CHECK(pub.verify("header.payload", b));
  a[5] ^= 1;
  CHECK_FALSE(pub.verify("header.payload", a));
  CHECK_FALSE(pub.sign("x", b));  // no private key
  // Not P-256: an Ed25519 key, junk, a public key read as a private one.
  CHECK_FALSE(
      EcdsaP256Key::from_private_pem(fastmm::test::fixture("binance/ed25519-test-private.pem"))
          .valid());
  CHECK_FALSE(EcdsaP256Key::from_private_pem("junk").valid());
  CHECK_FALSE(EcdsaP256Key::from_private_pem(kP256Public).valid());
  EcdsaP256Key moved = EcdsaP256Key::from_private_pem(kP256Private);
  EcdsaP256Key target;
  target = std::move(moved);
  CHECK(target.has_private());
}
