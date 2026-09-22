// Ed25519 API keys for Binance Spot and USDⓈ-M: the Signer, session.logon frames whose signature
// verifies against the public key, unsigned requests once the session is logged on, Ed25519 REST
// signatures, and the config keys (key_type, private_key_file / private_key_env, md_format,
// sbe_ws_url).
#include "venue_test_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include <cstdlib>
#include <map>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr std::int64_t kTs = 1789295199000;
constexpr std::string_view kKey = "ed-key";

std::string private_pem() {
  return fastmm::test::fixture("binance/ed25519-test-private.pem");
}
std::string public_pem() {
  return fastmm::test::fixture("binance/ed25519-test-public.pem");
}

binance::Signer ed_signer() {
  binance::Credentials c;
  c.api_key = std::string(kKey);
  c.type = binance::KeyType::Ed25519;
  c.private_key_pem.value = private_pem();
  return binance::Signer(c);
}

OutNewOrderMsg new_order() {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_decimal("70000").value();
  n.qty = Qty::from_decimal("0.001").value();
  return n;
}

// Value of a top-level JSON string field inside "params" (test frames only, no escapes).
std::string param(std::string_view frame, std::string_view key) {
  const std::string k = "\"" + std::string(key) + "\":\"";
  const std::size_t at = frame.find(k);
  if (at == std::string_view::npos) return {};
  const std::size_t end = frame.find('"', at + k.size());
  return std::string(frame.substr(at + k.size(), end - at - k.size()));
}

}  // namespace

TEST_CASE("binance.ed25519: signer parses the key once and signs verifiably") {
  const binance::Signer s = ed_signer();
  REQUIRE(s.usable());
  CHECK(s.type() == binance::KeyType::Ed25519);
  char sig[net::kEd25519Base64Size];
  const std::size_t n = s.sign_ed25519("apiKey=ed-key&timestamp=1", sig);
  REQUIRE(n == net::kEd25519Base64Size);
  const auto pub = net::Ed25519Key::from_public_pem(public_pem());
  REQUIRE(pub.valid());
  CHECK(pub.verify_base64("apiKey=ed-key&timestamp=1", std::string_view(sig, n)));
  CHECK_FALSE(pub.verify_base64("apiKey=ed-key&timestamp=2", std::string_view(sig, n)));
  CHECK(s.sign("x") == net::ed25519_sign_base64(private_pem(), "x"));

  binance::Credentials broken;
  broken.api_key = "k";
  broken.type = binance::KeyType::Ed25519;
  broken.private_key_pem.value = "-----BEGIN PRIVATE KEY-----\nnope\n-----END PRIVATE KEY-----\n";
  CHECK(broken.usable());                         // present...
  CHECK_FALSE(binance::Signer(broken).usable());  // ...but not a key
}

TEST_CASE("binance.ed25519: spot session.logon is signed, requests after it are not") {
  TestUniverse u;
  const binance::Signer s = ed_signer();
  binance::BinanceOrderEncoder enc(s, u.symbols, 3000);
  char buf[binance::kMaxRequestBytes];
  std::size_t len = enc.encode_ws_logon("logon-o", kTs, buf);
  REQUIRE(len > 0);
  const std::string_view logon(buf, len);
  CHECK(logon.starts_with(
      R"({"id":"logon-o","method":"session.logon","params":{"apiKey":"ed-key","recvWindow":3000,"timestamp":1789295199000,"signature":")"));
  const auto pub = net::Ed25519Key::from_public_pem(public_pem());
  CHECK(pub.verify_base64("apiKey=ed-key&recvWindow=3000&timestamp=1789295199000",
                          param(logon, "signature")));

  // Before logon completes, an order is signed with Ed25519 like any other request.
  const OutNewOrderMsg n = new_order();
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  const std::string signed_place(buf, len);
  CHECK(signed_place.find(R"("apiKey":"ed-key")") != std::string::npos);
  CHECK(pub.verify_base64(
      "apiKey=ed-key&newClientOrderId=fm000100000001&newOrderRespType=ACK&price=70000&"
      "quantity=0.001&recvWindow=3000&side=BUY&symbol=BTCUSDT&timestamp=1789295199000&"
      "type=LIMIT_MAKER",
      param(signed_place, "signature")));

  enc.set_session_authenticated(true);
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  CHECK(
      std::string_view(buf, len) ==
      R"({"id":"nfm000100000001","method":"order.place","params":{"newClientOrderId":"fm000100000001","newOrderRespType":"ACK","price":"70000","quantity":"0.001","recvWindow":3000,"side":"BUY","symbol":"BTCUSDT","timestamp":1789295199000,"type":"LIMIT_MAKER"}})");
  // session.logon itself is always signed, even while a session is up (re-logon after revoke).
  len = enc.encode_ws_logon("logon-o", kTs, buf);
  CHECK(std::string_view(buf, len).find("\"signature\":\"") != std::string_view::npos);
  CHECK(enc.session_authenticated());
}

TEST_CASE("binance.ed25519: REST queries carry a percent-encoded base64 signature") {
  TestUniverse u;
  const binance::Signer s = ed_signer();
  binance::BinanceOrderEncoder enc(s, u.symbols, 3000);
  binance::RestRequest rr;
  const OutNewOrderMsg n = new_order();
  REQUIRE(enc.encode_rest(*OrderCommand::from(n.hdr), nullptr, kTs, rr));
  const std::string_view q = rr.query.view();
  const std::size_t at = q.rfind("&signature=");
  REQUIRE(at != std::string_view::npos);
  std::string sig(q.substr(at + 11));
  // Undo %2B / %2F / %3D.
  std::string decoded;
  for (std::size_t i = 0; i < sig.size(); ++i) {
    if (sig[i] == '%' && i + 2 < sig.size()) {
      decoded += static_cast<char>(std::stoi(sig.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      decoded += sig[i];
    }
  }
  const auto pub = net::Ed25519Key::from_public_pem(public_pem());
  CHECK(pub.verify_base64(q.substr(0, at), decoded));
}

TEST_CASE("binance.ed25519: USDⓈ-M session.logon and unsigned requests") {
  TestUniverse u;
  const binance::Signer s = ed_signer();
  binance_usdm::BinanceUsdmOrderEncoder enc(s, u.symbols, 5000);
  char buf[binance::kMaxRequestBytes];
  std::size_t len = enc.encode_ws_logon("logon", kTs, buf);
  REQUIRE(len > 0);
  const std::string_view logon(buf, len);
  CHECK(logon.find(R"("method":"session.logon")") != std::string_view::npos);
  const auto pub = net::Ed25519Key::from_public_pem(public_pem());
  CHECK(pub.verify_base64("apiKey=ed-key&recvWindow=5000&timestamp=1789295199000",
                          param(logon, "signature")));
  const OutNewOrderMsg n = new_order();
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(std::string_view(buf, len).find("\"signature\":\"") != std::string_view::npos);
  enc.set_session_authenticated(true);
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  const std::string_view v(buf, len);
  CHECK(v.find("apiKey") == std::string_view::npos);
  CHECK(v.find("signature") == std::string_view::npos);
  CHECK(v.find(R"("timeInForce":"GTX")") != std::string_view::npos);
  CHECK(v.find(R"("timestamp":1789295199000)") != std::string_view::npos);
}

TEST_CASE("binance.ed25519: config keys for key files, env, md_format and the SBE url") {
  const std::string key_file =
      (fastmm::test::fixtures_dir() / "binance" / "ed25519-test-private.pem").string();
  std::map<std::string, std::string> extra{{"key_type", "ed25519"}, {"private_key_file", key_file}};
  binance::VenueSectionView v;
  v.name = "bn";
  v.ws_url = "wss://demo-stream.binance.com/stream";
  v.ws_api_url = "wss://demo-ws-api.binance.com/ws-api/v3";
  v.rest_url = "https://demo-api.binance.com";
  v.api_key = "ed-key";
  v.extra = &extra;
  binance::BinanceVenueConfig c = binance::make_binance_config(v, false);
  CHECK(c.credentials.type == binance::KeyType::Ed25519);
  CHECK(binance::Signer(c.credentials).usable());
  CHECK(c.md_format == binance::MdFormat::Json);

  // The PEM from an environment variable (wins over the file).
  ::setenv("FASTMM_TEST_ED25519_PEM", private_pem().c_str(), 1);
  extra["private_key_env"] = "FASTMM_TEST_ED25519_PEM";
  extra["private_key_file"] = "/nonexistent.pem";
  c = binance::make_binance_config(v, false);
  CHECK(binance::Signer(c.credentials).usable());
  ::unsetenv("FASTMM_TEST_ED25519_PEM");
  // Missing key: live refuses, dry run carries on.
  CHECK_THROWS_AS(static_cast<void>(binance::make_binance_config(v, false)), std::invalid_argument);
  CHECK_NOTHROW(static_cast<void>(binance::make_binance_config(v, true)));
  extra.erase("private_key_env");
  extra["private_key_file"] = key_file;

  extra["md_format"] = "sbe";
  c = binance::make_binance_config(v, false);
  CHECK(c.md_format == binance::MdFormat::Sbe);
  CHECK(c.sbe_ws_url == "wss://demo-stream-sbe.binance.com/stream");
  extra["sbe_ws_url"] = "wss://example.test:9443/stream";
  CHECK(binance::make_binance_config(v, true).sbe_ws_url == "wss://example.test:9443/stream");
  extra.erase("sbe_ws_url");
  extra["md_format"] = "protobuf";
  CHECK_THROWS_AS(static_cast<void>(binance::make_binance_config(v, true)), std::invalid_argument);
  // SBE streams take Ed25519 keys only.
  extra["md_format"] = "sbe";
  extra["key_type"] = "hmac";
  CHECK_THROWS_AS(static_cast<void>(binance::make_binance_config(v, true)), std::invalid_argument);
  extra["key_type"] = "rsa";
  CHECK_THROWS_AS(static_cast<void>(binance::make_binance_config(v, true)), std::invalid_argument);

  CHECK(binance::derive_sbe_ws_url("wss://stream.binance.com:9443/stream") ==
        "wss://stream-sbe.binance.com:9443/stream");
  CHECK(binance::derive_sbe_ws_url("wss://stream.testnet.binance.vision/stream") ==
        "wss://stream-sbe.testnet.binance.vision/stream");
  CHECK(binance::derive_sbe_ws_url("wss://demo-stream.binance.com/ws") ==
        "wss://demo-stream-sbe.binance.com/ws");
  CHECK(binance::derive_sbe_ws_url("ws://127.0.0.1:9080/stream").empty());
}
