// Hyperliquid action signing against hyperliquid-python-sdk 0.24.0 (utils/signing.py): the
// msgpack of each action, its connectionId, the EIP-712 digest and the signature byte for byte, and
// the payload JSON the SDK posts. Vectors from tests/fixtures/hyperliquid/gen_signing.py, signed
// with a throwaway key that controls no account.
#include "test_support.hpp"

#include "fastmm/venues/hyperliquid/hl_crypto.hpp"
#include "fastmm/venues/hyperliquid/hl_msgpack.hpp"
#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"

#include <array>
#include <string>
#include <string_view>

using namespace fastmm;
using namespace fastmm::venues::hyperliquid;

namespace {

constexpr std::string_view kKey =
    "0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318";
constexpr std::string_view kAddress = "0x2c7536e3605d9c16a7a3d7b1898e529396a65c23";
constexpr std::uint32_t kNvda = 110002;  // xyz (perp dex 1), index 2 of its meta
constexpr std::uint32_t kBtc = 0;

struct Golden {
  std::string_view name;
  std::uint64_t nonce;
  bool mainnet;
  std::string_view vault;
  std::string_view msgpack;
  std::string_view connection_id;
  std::string_view digest;
  std::string_view r;
  std::string_view s;
  int v;
  std::string_view payload;
};

// clang-format off
constexpr Golden kGolden[] = {
    {"order_alo_cloid", 1791781700000ULL, true, "", "83a474797065a56f72646572a66f72646572739187a161ce0001adb2a162c3a170a63233302e3332a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030326130303035a867726f7570696e67a26e61", "a1849db25ea6f0c04643bf71d3b94c1b750384e36b882e812a81ee04be9a6097", "0dd7f238a3a3935be150f765dc18f04e0711ade920dd77c4565494ee720492c1", "0x5fbf0ad2f6a9cdf11391c13557bb9e9cde68b8d9939ca4c14c1350b24960d620", "0xd062bf05d7cb3738a2055766dfe92bf709819cd52b53f83342461cb2dfdb117", 27,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.32","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d00000000000000010000002a0005"}],"grouping":"na"},"nonce":1791781700000,"signature":{"r":"0x5fbf0ad2f6a9cdf11391c13557bb9e9cde68b8d9939ca4c14c1350b24960d620","s":"0xd062bf05d7cb3738a2055766dfe92bf709819cd52b53f83342461cb2dfdb117","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"order_gtc_reduce_only", 1791781700001ULL, true, "", "83a474797065a56f72646572a66f72646572739186a16100a162c2a170a53836323837a173a5302e303031a172c3a17481a56c696d697481a3746966a3477463a867726f7570696e67a26e61", "e12850ac5dc165212bdc3f9217fd75b0e411766ee8487f4cc41dd294319dcfc8", "da09d385e4f550cc370bfaf768562e0381ca56f61e381176670141b4d7077339", "0xd2cecd2ff698708ed12f1889e26efa2887090270dacb31a1672d582c65ec9573", "0x443186893d41d30c5d200a06ce8bec54cb95de34d738501a927d910ace320a59", 27,
     R"({"action":{"type":"order","orders":[{"a":0,"b":false,"p":"86287","s":"0.001","r":true,"t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"},"nonce":1791781700001,"signature":{"r":"0xd2cecd2ff698708ed12f1889e26efa2887090270dacb31a1672d582c65ec9573","s":"0x443186893d41d30c5d200a06ce8bec54cb95de34d738501a927d910ace320a59","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"order_batch_two", 1791781700002ULL, true, "", "83a474797065a56f72646572a66f72646572739287a161ce0001adb2a162c3a170a53233302e33a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d9223078363636643030303030303030303030303030303130303030303032613030303587a161ce0001adb2a162c2a170a63233302e3431a173a5312e323334a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030303066666666a867726f7570696e67a26e61", "ae9b78b651141a25ec06d5672a63f83c6313f78dcd0f9925a29958e3f2cbc3b6", "d649c50c571731c7fa2310125190005f3aa9eac19196512a2a10211acb1923ec", "0xec0a2e174a5e76180878caf516e06308b98ea3bfca434325c25a0eac79fb7655", "0x580e003a37fc369a9cde23ea74e3860b47e791c9f62357c3d1c728c0ccc28a03", 27,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.3","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d00000000000000010000002a0005"},{"a":110002,"b":false,"p":"230.41","s":"1.234","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d000000000000000100000000ffff"}],"grouping":"na"},"nonce":1791781700002,"signature":{"r":"0xec0a2e174a5e76180878caf516e06308b98ea3bfca434325c25a0eac79fb7655","s":"0x580e003a37fc369a9cde23ea74e3860b47e791c9f62357c3d1c728c0ccc28a03","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"order_ioc", 1791781700003ULL, true, "", "83a474797065a56f72646572a66f72646572739187a161ce0001adb2a162c3a170a3323331a173a431322e35a172c2a17481a56c696d697481a3746966a3496f63a163d92230783636366430303030303030303030303030303031303030303030303066666666a867726f7570696e67a26e61", "40af0cccd4471a0012e4c8a7835a0c4c4d6d136a8933cf13a44c1eca564ebd9a", "035cea443cac765dcfeeb5895262ad7df01ce20c0c1609b7a855a7a39db04876", "0xd4a29c4d60b37337d67de86e1f7f6f677bc877e2273297e03165ac9329fec0a6", "0x14664f034aa0863de1f8e7763278cde2279db58357c1703eeb54093654cc14c", 28,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"231","s":"12.5","r":false,"t":{"limit":{"tif":"Ioc"}},"c":"0x666d000000000000000100000000ffff"}],"grouping":"na"},"nonce":1791781700003,"signature":{"r":"0xd4a29c4d60b37337d67de86e1f7f6f677bc877e2273297e03165ac9329fec0a6","s":"0x14664f034aa0863de1f8e7763278cde2279db58357c1703eeb54093654cc14c","v":28},"vaultAddress":null,"expiresAfter":null})"},
    {"cancel_two", 1791781700004ULL, true, "", "82a474797065a663616e63656ca763616e63656c739282a161ce0001adb2a16fcf000000851cb2775e82a16100a16f01", "fbfc3bb8ba612fedd6a67d122b6b44b6a200d12608e66aabcbfa8443f610f7a2", "824bd739cd75e6a1e2c104741bd275c439a0cb8b00bfcb80201a1eabc25b40fe", "0x5359812c687da3dd3736c66fa8d61437df06157931679b0074be188d6c5e6b35", "0x41075bb321451a0ad6f14a84f35c70f78644ede880f6f8e04aa85b91d1f5722c", 27,
     R"({"action":{"type":"cancel","cancels":[{"a":110002,"o":571712108382},{"a":0,"o":1}]},"nonce":1791781700004,"signature":{"r":"0x5359812c687da3dd3736c66fa8d61437df06157931679b0074be188d6c5e6b35","s":"0x41075bb321451a0ad6f14a84f35c70f78644ede880f6f8e04aa85b91d1f5722c","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"cancel_by_cloid", 1791781700005ULL, true, "", "82a474797065ad63616e63656c4279436c6f6964a763616e63656c739182a56173736574ce0001adb2a5636c6f6964d92230783636366430303030303030303030303030303031303030303030326130303035", "ed5ef462a3298bc56474b802ee6d08227de703e7648c64d07b6225899dc93e5e", "cf0a9ea7e0569ad636e7d4e00ed9b31e5ca960ba0fb88c039ba7288ec99ae17b", "0x6b159ca20afcb96b93e15126373e772b873b2a62a3f12d5ec7349550935102aa", "0x71cb433e63120aef0f12e4c2fcab91da8149559609cd519208f12e61c056c817", 28,
     R"({"action":{"type":"cancelByCloid","cancels":[{"asset":110002,"cloid":"0x666d00000000000000010000002a0005"}]},"nonce":1791781700005,"signature":{"r":"0x6b159ca20afcb96b93e15126373e772b873b2a62a3f12d5ec7349550935102aa","s":"0x71cb433e63120aef0f12e4c2fcab91da8149559609cd519208f12e61c056c817","v":28},"vaultAddress":null,"expiresAfter":null})"},
    {"batch_modify_oid", 1791781700006ULL, true, "", "82a474797065ab62617463684d6f64696679a86d6f6469666965739182a36f6964cf000000851cb2775ea56f7264657287a161ce0001adb2a162c3a170a63233302e3235a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030303066666666", "1b183f24cd0a409bd62b5fbfa30b8d2cb3b78dbbb78b67b38beef8cbb0a55662", "52d369e7d5446047a015d9964142ab3611557e9b7b2fba946ae1db723e8b3641", "0x17fb33369c75c190f3a72ce26ee520162e214e612dc61b8bb5f02854c2e0fc5a", "0xdb4ea8f19cce15d01c20af9b8a98852c235db3e74c23a1382c59e163c71a38", 27,
     R"({"action":{"type":"batchModify","modifies":[{"oid":571712108382,"order":{"a":110002,"b":true,"p":"230.25","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d000000000000000100000000ffff"}}]},"nonce":1791781700006,"signature":{"r":"0x17fb33369c75c190f3a72ce26ee520162e214e612dc61b8bb5f02854c2e0fc5a","s":"0xdb4ea8f19cce15d01c20af9b8a98852c235db3e74c23a1382c59e163c71a38","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"batch_modify_cloid", 1791781700007ULL, true, "", "82a474797065ab62617463684d6f64696679a86d6f6469666965739182a36f6964d92230783636366430303030303030303030303030303031303030303030326130303035a56f7264657287a161ce0001adb2a162c2a170a53233302e35a173a4302e3037a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030303066666666", "ce8be3f31a6610fed4e3a4b0353b5edbd2e45b1b21c4ccea1630402a6f72c4dc", "03e5adc1c1e670ab7d3d49c0864d2c70f28bb56dbc4ce093898ddb94daf50ed4", "0x4b50ad6aacc62b4ec2045e37b33c75813866572b305792507c12ce0dceb4f520", "0x124ae0ee0182c14a192b9cc018b1db875a2b9250461abaf29886b2f8ea1cd79c", 27,
     R"({"action":{"type":"batchModify","modifies":[{"oid":"0x666d00000000000000010000002a0005","order":{"a":110002,"b":false,"p":"230.5","s":"0.07","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d000000000000000100000000ffff"}}]},"nonce":1791781700007,"signature":{"r":"0x4b50ad6aacc62b4ec2045e37b33c75813866572b305792507c12ce0dceb4f520","s":"0x124ae0ee0182c14a192b9cc018b1db875a2b9250461abaf29886b2f8ea1cd79c","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"schedule_cancel_time", 1791781700008ULL, true, "", "82a474797065ae7363686564756c6543616e63656ca474696d65cf000001a12e83ec00", "cdd0a54e84a0b0db74864faee93f8879c59f1d499940f8ef3ef805dc24d251bc", "02c56f817ee96b875459742911495f3cdf9ca15fc317e889e65c3c2abdd366bb", "0xd2d91ae7c43c227899dc049ff7d93cfb60bfcbe29afcf41846204859bcdac3d7", "0x985d14efc782ee5aae0280ea5c5bab3f8e560b2aecb7eef83cd2f25598037c4", 27,
     R"({"action":{"type":"scheduleCancel","time":1791781760000},"nonce":1791781700008,"signature":{"r":"0xd2d91ae7c43c227899dc049ff7d93cfb60bfcbe29afcf41846204859bcdac3d7","s":"0x985d14efc782ee5aae0280ea5c5bab3f8e560b2aecb7eef83cd2f25598037c4","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"schedule_cancel_clear", 1791781700009ULL, true, "", "81a474797065ae7363686564756c6543616e63656c", "ae408e937ae64f55ea9774364932840c227f47c0e15587d8fc9aa629facc32df", "3eec0dfc14628c4efe3ee132577282e09a9b63b8428b352501a26e23659c8ec1", "0x2bf3ac3811c8b707459b9fadafd3f2a042782d31d87c85d0bdb825a6ac33c279", "0x4e1441adb2b61e5c1bef0f310150834a08d75a08119e526d7f770ac220e0a2ff", 27,
     R"({"action":{"type":"scheduleCancel"},"nonce":1791781700009,"signature":{"r":"0x2bf3ac3811c8b707459b9fadafd3f2a042782d31d87c85d0bdb825a6ac33c279","s":"0x4e1441adb2b61e5c1bef0f310150834a08d75a08119e526d7f770ac220e0a2ff","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"order_testnet", 1791781700010ULL, false, "", "83a474797065a56f72646572a66f72646572739187a161ce0001adb2a162c3a170a63233302e3332a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030326130303035a867726f7570696e67a26e61", "5647857ec819cbbb975dc78f33bdce2ef9904ba4150773b443c4f24f7b4972af", "615edc3591df77d85d8ec7e8234bc4b08893bcb08efab5ae73a7ba214d1d86a2", "0x6f6c08c5df3899a4d65351c7b5a33e588464eacc141416fa48f411d8a930bcbc", "0x2d10395996251aa4fe7692c5ff09d2945d9519b94e5548caca1e3e9aa2edc976", 27,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.32","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d00000000000000010000002a0005"}],"grouping":"na"},"nonce":1791781700010,"signature":{"r":"0x6f6c08c5df3899a4d65351c7b5a33e588464eacc141416fa48f411d8a930bcbc","s":"0x2d10395996251aa4fe7692c5ff09d2945d9519b94e5548caca1e3e9aa2edc976","v":27},"vaultAddress":null,"expiresAfter":null})"},
    {"order_vault", 1791781700011ULL, true, "0x1719884eb866cb12b2287399b15f7db5e7d775ea", "83a474797065a56f72646572a66f72646572739187a161ce0001adb2a162c3a170a63233302e3332a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030326130303035a867726f7570696e67a26e61", "bb65e1215c1e982f240d27ebe4216105ce9cb95b0649fdf5588bcdfaad6969b9", "b4e2fa75ee90f25a7aca368ff11af14b06c9c6fb25b60e797a91ec36e7a5b1d8", "0x7e858e4e134cbb6100814becb16f90e8583ab78dcba6a752c64192ebfec64c42", "0x154b28776aef46d60c92bd95aee1041bfcd9cba30aad49c771fd8fb217ee9faf", 28,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.32","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d00000000000000010000002a0005"}],"grouping":"na"},"nonce":1791781700011,"signature":{"r":"0x7e858e4e134cbb6100814becb16f90e8583ab78dcba6a752c64192ebfec64c42","s":"0x154b28776aef46d60c92bd95aee1041bfcd9cba30aad49c771fd8fb217ee9faf","v":28},"vaultAddress":"0x1719884eb866cb12b2287399b15f7db5e7d775ea","expiresAfter":null})"},
    {"order_expires", 1791781700012ULL, true, "", "83a474797065a56f72646572a66f72646572739187a161ce0001adb2a162c3a170a63233302e3332a173a4302e3035a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783636366430303030303030303030303030303031303030303030326130303035a867726f7570696e67a26e61", "a9c27230c521bf5a7a21513078c7d8b1077c4709b66151a30461b2fec5f17291", "7349c750c80ac51ad42270047621d187edce9c83561fe730c2e3174e0e7dacff", "0xcda92e983ac1e5dec175b7bef4fdb3ef856437f0d2393a4e1cc7e1651dfefbc", "0x4b149ab4c5be3edface1be365b71b323ae09c91443eed4904b13cefb6fd33783", 28,
     R"({"action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.32","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":"0x666d00000000000000010000002a0005"}],"grouping":"na"},"nonce":1791781700012,"signature":{"r":"0xcda92e983ac1e5dec175b7bef4fdb3ef856437f0d2393a4e1cc7e1651dfefbc","s":"0x4b149ab4c5be3edface1be365b71b323ae09c91443eed4904b13cefb6fd33783","v":28},"vaultAddress":null,"expiresAfter":1791781760000})"},
};
// clang-format on

std::string hex(std::span<const std::uint8_t> b) {
  std::string s(b.size() * 2, '0');
  hex_lower(b, s.data());
  return s;
}

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

OrderWire wire(std::uint32_t asset,
               bool buy,
               const char* sz,
               const char* p,
               std::string_view tif,
               bool reduce_only = false,
               const Cloid& cloid = {}) {
  OrderWire o;
  o.asset = asset;
  o.buy = buy;
  o.sz = wire_text(qty(sz));
  o.px = wire_text(px(p));
  o.tif = tif;
  o.reduce_only = reduce_only;
  o.cloid = cloid;
  return o;
}

Cloid cl1() {
  return cloid_of(ClientOrderId{0x00010000002a0005ULL});
}
Cloid cl2() {
  return cloid_of(ClientOrderId{0x000100000000ffffULL});
}

// The action of each golden case, built the way the connector builds it.
bool build(std::string_view name, ActionBuilder& a) {
  if (name == "order_alo_cloid" || name == "order_testnet" || name == "order_vault" ||
      name == "order_expires") {
    const OrderWire o[] = {wire(kNvda, true, "0.05", "230.32", "Alo", false, cl1())};
    return a.order(o);
  }
  if (name == "order_gtc_reduce_only") {
    const OrderWire o[] = {wire(kBtc, false, "0.001", "86287", "Gtc", true)};
    return a.order(o);
  }
  if (name == "order_batch_two") {
    const OrderWire o[] = {wire(kNvda, true, "0.05", "230.3", "Alo", false, cl1()),
                           wire(kNvda, false, "1.234", "230.41", "Alo", false, cl2())};
    return a.order(o);
  }
  if (name == "order_ioc") {
    const OrderWire o[] = {wire(kNvda, true, "12.5", "231", "Ioc", false, cl2())};
    return a.order(o);
  }
  if (name == "cancel_two") {
    const CancelWire c[] = {{kNvda, 571712108382ULL}, {kBtc, 1}};
    return a.cancel(c);
  }
  if (name == "cancel_by_cloid") {
    const CancelCloidWire c[] = {{kNvda, cl1()}};
    return a.cancel_by_cloid(c);
  }
  if (name == "batch_modify_oid") {
    ModifyWire m;
    m.oid = 571712108382ULL;
    m.order = wire(kNvda, true, "0.05", "230.25", "Alo", false, cl2());
    return a.batch_modify({&m, 1});
  }
  if (name == "batch_modify_cloid") {
    ModifyWire m;
    m.by_cloid = cl1();
    m.order = wire(kNvda, false, "0.07", "230.5", "Alo", false, cl2());
    return a.batch_modify({&m, 1});
  }
  if (name == "schedule_cancel_time") return a.schedule_cancel(1791781760000);
  if (name == "schedule_cancel_clear") return a.schedule_cancel(0);
  return false;
}

}  // namespace

TEST_CASE("hyperliquid: keccak256 matches eth_utils.keccak") {
  CHECK(hex(keccak256(std::string_view(""))) ==
        "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  CHECK(hex(keccak256(std::string_view("abc"))) ==
        "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
  const std::string two_blocks(200, 'a');  // longer than one 136-byte block
  CHECK(hex(keccak256(std::string_view(two_blocks))) ==
        "96ea54061def936c4be90b518992fdc6f12f535068a256229aca54267b4d084d");
}

TEST_CASE("hyperliquid: the signer derives the key's address") {
  const Signer s(kKey);
  REQUIRE(s.usable());
  CHECK(s.address() == kAddress);
  const Signer no_prefix(kKey.substr(2));
  CHECK(no_prefix.address() == kAddress);
  const Signer bad("0x1234");
  CHECK_FALSE(bad.usable());
  CHECK_FALSE(bad.error().empty());
  const Signer zero("0x0000000000000000000000000000000000000000000000000000000000000000");
  CHECK_FALSE(zero.usable());
}

TEST_CASE("hyperliquid: actions, hashes and signatures match the SDK byte for byte") {
  const Signer signer(kKey);
  REQUIRE(signer.usable());
  for (const Golden& g : kGolden) {
    CAPTURE(g.name);
    ActionBuilder a;
    REQUIRE(build(g.name, a));
    CHECK(hex(a.msgpack()) == g.msgpack);
    const std::int64_t expires = g.name == "order_expires" ? 1791781760000 : 0;
    const Hash32 cid = connection_id(a.msgpack(), g.nonce, g.vault, expires);
    CHECK(hex(cid) == g.connection_id);
    const Hash32 digest = agent_digest(cid, g.mainnet);
    CHECK(hex(digest) == g.digest);
    Signature sig;
    REQUIRE(signer.sign(digest, sig));
    char r[kHexQuantityChars];
    char s[kHexQuantityChars];
    CHECK(std::string_view(r, hex_quantity(sig.r, r)) == g.r);
    CHECK(std::string_view(s, hex_quantity(sig.s, s)) == g.s);
    CHECK(sig.v == g.v);
    if (expires == 0) {  // the connector sends no expiresAfter
      const ActionSigner as(signer, g.mainnet, std::string(g.vault));
      std::array<char, kMaxRequestBytes> buf{};
      const std::size_t n = as.write_payload(a, g.nonce, buf);
      CHECK(std::string_view(buf.data(), n) == g.payload);
    }
  }
}

TEST_CASE("hyperliquid: the post frame wraps the payload") {
  const Signer signer(kKey);
  ActionBuilder a;
  const CancelWire c[] = {{kNvda, 571712108382ULL}, {kBtc, 1}};
  REQUIRE(a.cancel(c));
  const ActionSigner as(signer, true);
  std::array<char, kMaxRequestBytes> buf{};
  const std::size_t n = as.write_post(a, 1791781700004ULL, 7, buf);
  CHECK(std::string_view(buf.data(), n) ==
        std::string(R"({"method":"post","id":7,"request":{"type":"action","payload":)") +
            std::string(kGolden[4].payload) + "}}");
  const Signer none;
  const ActionSigner unusable(none, true);
  CHECK(unusable.write_post(a, 1, 1, buf) == 0);
}

TEST_CASE("hyperliquid: nonces strictly increase") {
  const Signer signer(kKey);
  ActionSigner as(signer, true);
  CHECK(as.next_nonce(1000) == 1000);
  CHECK(as.next_nonce(1000) == 1001);
  CHECK(as.next_nonce(999) == 1002);
  CHECK(as.next_nonce(2000) == 2000);
}

TEST_CASE("hyperliquid: cloids carry FastMM's client order ids") {
  const ClientOrderId id{0x00010000002a0005ULL};
  CHECK(cloid_of(id).view() == "0x666d00000000000000010000002a0005");
  CHECK(cl_ord_id_of_cloid("0x666d00000000000000010000002a0005") == id);
  CHECK(cl_ord_id_of_cloid("0x666D00000000000000010000002A0005") == id);
  CHECK_FALSE(cl_ord_id_of_cloid("0x72da709e3d8bc07e3bd16a344c62c95a"));  // another client's
  CHECK_FALSE(cl_ord_id_of_cloid(""));
  CHECK_FALSE(cl_ord_id_of_cloid("0x666d000000000000000000000000000"));
}

TEST_CASE("hyperliquid: prices keep 5 significant figures and 6 - szDecimals decimals") {
  // The float_to_wire texts the SDK sends.
  CHECK(wire_text(px("230.32")).view() == "230.32");
  CHECK(wire_text(px("86287")).view() == "86287");
  CHECK(wire_text(qty("0.001")).view() == "0.001");
  CHECK(wire_text(qty("12.5")).view() == "12.5");
  CHECK(wire_text(px("0.000123")).view() == "0.000123");
  CHECK(wire_text(Price{}).view() == "0");
  CHECK(wire_text(Price::from_raw(-150000000)).view() == "-1.5");
  // NVDA (szDecimals 3): 230.xx has 2 decimals; XYZ100 (szDecimals 4) an integer price.
  CHECK(price_decimals(px("230.32"), 3) == 2);
  CHECK(tick_at(px("230.32"), 3) == px("0.01"));
  CHECK(price_decimals(px("30958"), 4) == 0);
  CHECK(tick_at(px("30958"), 4) == px("1"));
  CHECK(price_decimals(px("1234567"), 0) == 0);  // integer prices are always allowed
  // Below 1 the first significant digit counts, and the decimal cap still applies.
  CHECK(price_decimals(px("0.012345"), 0) == 6);
  CHECK(price_decimals(px("0.012345"), 2) == 4);
  CHECK(price_decimals(px("5.9489"), 1) == 4);
  // Rounding never makes an order more aggressive.
  CHECK(round_price(px("230.326"), 3, true) == px("230.32"));
  CHECK(round_price(px("230.321"), 3, false) == px("230.33"));
  CHECK(round_price(px("230.32"), 3, false) == px("230.32"));
  CHECK(round_price(px("99.9991"), 3, false) == px("100"));
  CHECK(round_price(px("30958.6"), 4, true) == px("30958"));
  CHECK(round_size(qty("0.05678"), 3) == qty("0.056"));
  CHECK(round_size(qty("1.9"), 0) == qty("1"));
}

TEST_CASE("hyperliquid: msgpack integers and strings take their smallest form") {
  std::array<std::uint8_t, 80> buf{};
  MsgpackWriter m(buf);
  m.uint(127);
  m.uint(128);
  m.uint(65535);
  m.uint(65536);
  m.uint(1ULL << 32);
  m.str(std::string(32, 'x'));
  REQUIRE(m.ok());
  const std::string h = hex(m.bytes());
  CHECK(h.substr(0, 2) == "7f");
  CHECK(h.substr(2, 4) == "cc80");
  CHECK(h.substr(6, 6) == "cdffff");
  CHECK(h.substr(12, 10) == "ce00010000");
  CHECK(h.substr(22, 18) == "cf0000000100000000");
  CHECK(h.substr(40, 4) == "d920");
  std::array<std::uint8_t, 2> tiny{};
  MsgpackWriter small(tiny);
  small.str("abc");
  CHECK_FALSE(small.ok());
}
