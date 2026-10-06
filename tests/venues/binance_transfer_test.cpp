// Binance internal transfers (binance/binance_transfer.hpp): the signed universalTransfer POST and
// history GET, the answers, the one-leg spot and two-leg USDⓈ-M transfers against a scripted
// server, and the connector keys that carry the master key.
#include "fastmm/venues/binance/binance_transfer.hpp"

#include "test_support.hpp"

#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;

namespace {

Signer test_signer() {
  Credentials c;
  c.api_key = "master-key";
  c.secret.value = "master-secret";
  return Signer(c);
}

TransferRequest request(std::string from, std::string to) {
  TransferRequest r;
  r.client_id = "fm0mabc12345";
  r.from = VenueId{0};
  r.to = VenueId{1};
  r.from_account = std::move(from);
  r.to_account = std::move(to);
  r.asset = "USDT";
  r.amount = *Notional::parse("250.5");
  r.created_ms = 1'790'000'000'000;
  return r;
}

HttpReply reply(int status, std::string body) {
  HttpReply r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

// The query of a target "path?query", and its value of `key` (still percent-encoded).
std::string param(const std::string& target, const std::string& key) {
  const std::size_t q = target.find('?');
  std::string rest = q == std::string::npos ? "" : target.substr(q + 1);
  while (!rest.empty()) {
    const std::size_t amp = rest.find('&');
    const std::string kv = rest.substr(0, amp);
    const std::size_t eq = kv.find('=');
    if (kv.substr(0, eq) == key) return kv.substr(eq + 1);
    if (amp == std::string::npos) break;
    rest = rest.substr(amp + 1);
  }
  return "<absent>";
}

// A server that answers each request from a script and keeps what it was sent.
struct Script {
  std::deque<HttpReply> replies;
  std::vector<BlockingRequest> sent;
  TransferClient::Send send() {
    return [this](std::string_view, const BlockingControl::Build& build) {
      BlockingRequest q;
      REQUIRE(build(q));
      sent.push_back(q);
      REQUIRE_FALSE(replies.empty());
      HttpReply r = replies.front();
      replies.pop_front();
      return r;
    };
  }
};

std::int64_t fixed_clock() noexcept {
  return 1'790'000'001'000;
}

TransferSettings settings(std::string_view account_type) {
  TransferSettings s;
  s.venue = "binance";
  s.credentials.api_key = "master-key";
  s.credentials.secret.value = "master-secret";
  s.account_type = std::string(account_type);
  return s;
}

}  // namespace

TEST_CASE("binance.transfer: the POST is signed over its sorted parameters") {
  const Signer signer = test_signer();
  const auto legs = transfer_legs(request("a@example.com", "b@example.com"), "SPOT");
  REQUIRE(legs.size() == 1);
  BlockingRequest q;
  REQUIRE(encode_transfer(
      signer, legs[0], "USDT", *Notional::parse("250.5"), 1'790'000'001'000, 10'000, q));
  CHECK(q.method == "POST");
  CHECK(q.headers == "X-MBX-APIKEY: master-key\r\n");
  CHECK(q.body.empty());
  const std::string payload =
      "amount=250.5&asset=USDT&clientTranId=fm0mabc12345&fromAccountType=SPOT&"
      "fromEmail=a%40example.com&recvWindow=10000&timestamp=1790000001000&toAccountType=SPOT&"
      "toEmail=b%40example.com";
  CHECK(q.target ==
        std::string(kTransferPath) + "?" + payload + "&signature=" + signer.sign(payload));

  // The master account has no email: the parameter is left out.
  const auto from_master = transfer_legs(request("", "b@example.com"), "SPOT");
  REQUIRE(encode_transfer(signer, from_master[0], "USDT", *Notional::parse("1"), 1, 5000, q));
  CHECK(param(q.target, "fromEmail") == "<absent>");
  CHECK(param(q.target, "toEmail") == "b%40example.com");

  // Nothing to sign with, nothing to move: not built.
  CHECK_FALSE(encode_transfer(Signer{}, legs[0], "USDT", *Notional::parse("1"), 1, 5000, q));
  CHECK_FALSE(encode_transfer(signer, legs[0], "USDT", Notional{}, 1, 5000, q));
}

TEST_CASE("binance.transfer: the history GET names the sender and the client id") {
  const Signer signer = test_signer();
  const auto legs = transfer_legs(request("a@example.com", "b@example.com"), "SPOT");
  BlockingRequest q;
  REQUIRE(encode_transfer_query(signer, legs[0], 1'789'999'940'000, 1'790'000'001'000, 10'000, q));
  CHECK(q.method == "GET");
  const std::string payload =
      "clientTranId=fm0mabc12345&fromEmail=a%40example.com&recvWindow=10000&"
      "startTime=1789999940000&timestamp=1790000001000";
  CHECK(q.target ==
        std::string(kTransferPath) + "?" + payload + "&signature=" + signer.sign(payload));
  CHECK(param(q.target, "toEmail") == "<absent>");  // the endpoint takes one of the two
}

TEST_CASE("binance.transfer: a USD-M transfer goes through the receiver's spot wallet") {
  const auto legs = transfer_legs(request("a@example.com", "b@example.com"), "USDT_FUTURE");
  REQUIRE(legs.size() == 2);
  CHECK(legs[0].client_id == "fm0mabc12345a");
  CHECK(legs[0].from_email == "a@example.com");
  CHECK(legs[0].to_email == "b@example.com");
  CHECK(legs[0].from_type == "USDT_FUTURE");
  CHECK(legs[0].to_type == "SPOT");
  CHECK(legs[1].client_id == "fm0mabc12345b");
  CHECK(legs[1].from_email == "b@example.com");
  CHECK(legs[1].to_email == "b@example.com");
  CHECK(legs[1].from_type == "SPOT");
  CHECK(legs[1].to_type == "USDT_FUTURE");
}

TEST_CASE("binance.transfer: the POST's answers") {
  TransferResult r =
      decode_transfer_reply(reply(200, R"({"tranId":11945860693,"clientTranId":"x"})"));
  CHECK(r.state == TransferState::Pending);
  CHECK(r.venue_ref == "11945860693");
  r = decode_transfer_reply(reply(400, R"({"code":-9000,"msg":"Insufficient balance"})"));
  CHECK(r.state == TransferState::Failed);
  CHECK(r.detail.find("-9000") != std::string::npos);
  CHECK(decode_transfer_reply(reply(503, "")).state == TransferState::Unknown);
  HttpReply lost;
  lost.error = "connection reset";
  CHECK(decode_transfer_reply(lost).state == TransferState::Unknown);
  CHECK(decode_transfer_reply(reply(200, "{}")).state == TransferState::Unknown);
}

TEST_CASE("binance.transfer: the history's answers") {
  const char* body = R"({"result":[
    {"tranId":92275823339,"fromEmail":"a@example.com","toEmail":"b@example.com","asset":"USDT",
     "amount":"250.5","createTimeStamp":1790000001000,"fromAccountType":"SPOT",
     "toAccountType":"SPOT","status":"SUCCESS","clientTranId":"other"},
    {"tranId":92275823340,"fromEmail":"a@example.com","toEmail":"b@example.com","asset":"USDT",
     "amount":"250.5","createTimeStamp":1790000001000,"fromAccountType":"SPOT",
     "toAccountType":"SPOT","status":"SUCCESS","clientTranId":"fm0mabc12345"}],"totalCount":2})";
  TransferResult r = decode_transfer_history(reply(200, body), "fm0mabc12345");
  CHECK(r.state == TransferState::Done);
  CHECK(r.venue_ref == "92275823340");
  CHECK(decode_transfer_history(reply(200, body), "fm0nope").state == TransferState::NotFound);
  CHECK(decode_transfer_history(reply(200, R"({"result":[],"totalCount":0})"), "x").state ==
        TransferState::NotFound);
  CHECK(decode_transfer_history(reply(200, R"({"totalCount":0})"), "x").state ==
        TransferState::NotFound);
  CHECK(decode_transfer_history(
            reply(200, R"({"result":[{"clientTranId":"x","status":"PROCESS"}]})"), "x")
            .state == TransferState::Pending);
  CHECK(decode_transfer_history(
            reply(200, R"({"result":[{"clientTranId":"x","status":"FAILURE"}]})"), "x")
            .state == TransferState::Failed);
  CHECK(decode_transfer_history(reply(500, ""), "x").state == TransferState::Unknown);
}

TEST_CASE("binance.transfer: a spot transfer is one request, its state one query") {
  Script s;
  TransferClient c(settings("SPOT"), s.send(), &fixed_clock);
  s.replies.push_back(reply(200, R"({"tranId":1,"clientTranId":"fm0mabc12345"})"));
  const TransferRequest req = request("a@example.com", "b@example.com");
  CHECK(c.transfer(req).state == TransferState::Pending);
  REQUIRE(s.sent.size() == 1);
  CHECK(param(s.sent[0].target, "clientTranId") == "fm0mabc12345");
  CHECK(param(s.sent[0].target, "timestamp") == "1790000001000");

  s.replies.push_back(
      reply(200, R"({"result":[{"tranId":1,"clientTranId":"fm0mabc12345","status":"SUCCESS"}]})"));
  CHECK(c.status(req).state == TransferState::Done);
  REQUIRE(s.sent.size() == 2);
  CHECK(s.sent[1].method == "GET");
  CHECK(param(s.sent[1].target, "startTime") == "1789999940000");  // created less a minute
}

TEST_CASE("binance.transfer: a USD-M transfer sends both legs and finishes a missing second one") {
  Script s;
  TransferClient c(settings("USDT_FUTURE"), s.send(), &fixed_clock);
  const TransferRequest req = request("a@example.com", "b@example.com");
  // The second leg is refused (the first not credited yet): the transfer is still pending.
  s.replies.push_back(reply(200, R"({"tranId":1,"clientTranId":"fm0mabc12345a"})"));
  s.replies.push_back(reply(400, R"({"code":-9000,"msg":"Insufficient balance"})"));
  const TransferResult r = c.transfer(req);
  CHECK(r.state == TransferState::Pending);
  REQUIRE(s.sent.size() == 2);
  CHECK(param(s.sent[0].target, "fromAccountType") == "USDT_FUTURE");
  CHECK(param(s.sent[1].target, "toAccountType") == "USDT_FUTURE");

  // status(): the first leg is done, the second unknown to the venue: it is sent again.
  s.replies.push_back(
      reply(200, R"({"result":[{"tranId":1,"clientTranId":"fm0mabc12345a","status":"SUCCESS"}]})"));
  s.replies.push_back(reply(200, R"({"result":[],"totalCount":0})"));
  s.replies.push_back(reply(200, R"({"tranId":2,"clientTranId":"fm0mabc12345b"})"));
  CHECK(c.status(req).state == TransferState::Pending);
  REQUIRE(s.sent.size() == 5);
  CHECK(s.sent[4].method == "POST");
  CHECK(param(s.sent[4].target, "clientTranId") == "fm0mabc12345b");
  CHECK(param(s.sent[3].target, "fromEmail") == "b%40example.com");  // the second leg's sender

  // Both legs done.
  s.replies.push_back(
      reply(200, R"({"result":[{"tranId":1,"clientTranId":"fm0mabc12345a","status":"SUCCESS"}]})"));
  s.replies.push_back(
      reply(200, R"({"result":[{"tranId":2,"clientTranId":"fm0mabc12345b","status":"SUCCESS"}]})"));
  CHECK(c.status(req).state == TransferState::Done);

  // A first leg the venue has no record of: the whole transfer is unknown to it.
  s.replies.push_back(reply(200, R"({"result":[],"totalCount":0})"));
  CHECK(c.status(req).state == TransferState::NotFound);
}

TEST_CASE("binance.transfer: the connector keys name the master key's variables") {
  ::setenv("FASTMM_TEST_TRANSFER_KEY", "k", 1);
  ::setenv("FASTMM_TEST_TRANSFER_SECRET", "s", 1);
  std::map<std::string, std::string> extra{
      {"transfer_api_key_env", "FASTMM_TEST_TRANSFER_KEY"},
      {"transfer_api_secret_env", "FASTMM_TEST_TRANSFER_SECRET"},
      {"sub_account_email", "b@example.com"}};
  TransferSettings s = read_transfer_settings("binance_b", extra, "SPOT", false);
  CHECK(s.usable());
  CHECK(s.credentials.api_key == "k");
  CHECK(s.email == "b@example.com");
  CHECK(s.rest_url == kDefaultTransferRestUrl);
  // A dry run reads no key.
  CHECK_FALSE(read_transfer_settings("binance_b", extra, "SPOT", true).usable());
  // Only one of the two, or an unset variable.
  extra.erase("transfer_api_secret_env");
  CHECK_THROWS_AS(static_cast<void>(read_transfer_settings("b", extra, "SPOT", false)),
                  std::invalid_argument);
  extra["transfer_api_secret_env"] = "FASTMM_TEST_TRANSFER_UNSET";
  ::unsetenv("FASTMM_TEST_TRANSFER_UNSET");
  CHECK_THROWS_AS(static_cast<void>(read_transfer_settings("b", extra, "SPOT", false)),
                  std::invalid_argument);
  // Without the keys there are no transfers, and the default Venue answer says so.
  CHECK_FALSE(read_transfer_settings("b", {}, "SPOT", false).usable());
}

TEST_CASE("binance.transfer: the connectors announce transfers only with the master key") {
  ::setenv("FASTMM_TEST_TRANSFER_KEY", "k", 1);
  ::setenv("FASTMM_TEST_TRANSFER_SECRET", "s", 1);
  std::map<std::string, std::string> extra{
      {"transfer_api_key_env", "FASTMM_TEST_TRANSFER_KEY"},
      {"transfer_api_secret_env", "FASTMM_TEST_TRANSFER_SECRET"},
      {"sub_account_email", "a@example.com"}};
  VenueSectionView v;
  v.name = "binance";
  v.ws_url = "wss://stream.testnet.binance.vision";
  v.rest_url = "https://testnet.binance.vision";
  v.api_key = "trade-key";
  v.api_secret = "trade-secret";
  v.extra = &extra;
  BinanceVenue with(VenueId{0}, make_binance_config(v, false));
  CHECK(with.caps().internal_transfer);
  CHECK(with.transfer_account() == "a@example.com");
  const std::map<std::string, std::string> none;
  v.extra = &none;
  BinanceVenue without(VenueId{0}, make_binance_config(v, false));
  CHECK_FALSE(without.caps().internal_transfer);
  CHECK(without.transfer(request("", "b@example.com")).state == TransferState::Failed);

  VenueSection u;
  u.name = "usdm";
  u.kind = "binance_usdm";
  u.ws_url = "wss://fstream.binancefuture.com";
  u.rest_url = "https://testnet.binancefuture.com";
  u.api_key = "trade-key";
  u.api_secret = "trade-secret";
  u.extra = extra;
  const binance_usdm::BinanceUsdmVenueConfig uc = binance_usdm::make_binance_usdm_config(u, false);
  CHECK(uc.transfer.account_type == "USDT_FUTURE");
  CHECK(uc.transfer.usable());
}
