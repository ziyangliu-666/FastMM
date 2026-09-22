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

TEST_CASE("crypto: HmacSha256 with a precomputed key equals hmac_sha256") {
  const std::string long_key(131, '\xaa');  // longer than a block: hashed first
  const std::string payload(1000, 'p');
  // EVP_MAC refuses an empty key (hmac_sha256() then fails); the precomputed form does not.
  CHECK(HmacSha256(std::string_view{}).sign_hex("").view() ==
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
  for (const std::string_view key : {std::string_view("Jefe"),
                                     std::string_view("0123456789abcdef0123456789abcdef0123456789"
                                                      "abcdef0123456789abcdef"),  // exactly 64
                                     std::string_view(long_key)}) {
    const HmacSha256 h(key);
    REQUIRE(h.keyed());
    for (const std::string_view data : {std::string_view{},
                                        std::string_view("what do ya want for nothing?"),
                                        std::string_view(payload)}) {
      CHECK(h.sign_hex(data).view() == hmac_sha256_hex(key, data).view());
    }
  }
  CHECK(HmacSha256(long_key)
            .sign_hex("Test Using Larger Than Block-Size Key - Hash Key First")
            .view() == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  Sha256Digest d{};
  CHECK_FALSE(HmacSha256().sign("x", d));
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
