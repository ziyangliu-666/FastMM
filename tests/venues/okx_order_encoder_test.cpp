// OKX v5 signing, order encoding, response decoding, REST decoders and the error map. Formats
// from the docs (https://www.okx.com/docs-v5/en/, read 2026-09-26); the reference signatures were
// computed with Python's hmac/base64, independently of net/crypto.
#include "fastmm/venues/okx/okx_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/okx/okx_auth.hpp"
#include "fastmm/venues/okx/okx_error_map.hpp"
#include "fastmm/venues/okx/okx_rest_decoder.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using fastmm::venues::test::make_instrument;

namespace {

constexpr VenueId kOkx{1};
const InstrumentId kBtc{0};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT")));
    REQUIRE(symbols.build(instruments));
  }
};

Signer test_signer() {
  Credentials c;
  c.api_key = "test-key";
  c.secret.value = "test-secret";
  c.passphrase.value = "test-pass";
  return Signer(c);
}

OutNewOrderMsg new_order(OrderType type, TimeInForce tif) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kBtc, kOkx);
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Sell;
  n.type = type;
  n.tif = tif;
  n.price = Price::from_decimal("60000.1").value();
  n.qty = Qty::from_decimal("2.5").value();
  return n;
}

std::string encode(const OkxOrderEncoder& enc, const EventHeader& h, const OrderShadow* s) {
  char buf[kMaxRequestBytes];
  const std::size_t n = enc.encode_ws(*OrderCommand::from(h), s, buf);
  return {buf, n};
}

TradeResponse decode(OkxResponseDecoder& d, const std::string& body) {
  TradeResponse r;
  const PaddedJson j(body);
  REQUIRE(d.decode(j.view(), r) == ParseStatus::Ok);
  return r;
}

}  // namespace

TEST_CASE("okx.auth: ISO timestamps and signatures match an independent HMAC implementation") {
  CHECK(iso_timestamp(1607418537715).view() == "2020-12-08T09:08:57.715Z");
  CHECK(iso_timestamp(951782400000).view() == "2000-02-29T00:00:00.000Z");
  CHECK(iso_timestamp(1790439440699).view() == "2026-09-26T16:17:20.699Z");
  const Signer s = test_signer();
  // base64(HMAC_SHA256("test-secret", "1538054050GET/users/self/verify"))
  CHECK(s.sign_login(1538054050).view() == "dCiowwyD1J8GrUtvJeV5ghkSUWKx/rcjDXnOOQdJ6UI=");
  // timestamp + method + requestPath (with the query) + body
  CHECK(s.sign_rest("2020-12-08T09:08:57.715Z", "GET", "/api/v5/account/balance?ccy=BTC", "")
            .view() == "5KlCItRxE039QKll2OJlbYeUcSiPGR/z10UR7bbl68o=");
  CHECK(s.sign_rest("2020-12-08T09:08:57.715Z",
                    "POST",
                    "/api/v5/trade/order",
                    R"({"instId":"BTC-USDT-SWAP"})")
            .view() == "pTcfOssGhm9sSvJrX7EenxdYrhHiqBkSG5mJc6vZoQg=");
  const std::string h =
      s.rest_headers(1607418537715, "GET", "/api/v5/account/balance?ccy=BTC", "", true);
  CHECK(h ==
        "OK-ACCESS-KEY: test-key\r\n"
        "OK-ACCESS-SIGN: 5KlCItRxE039QKll2OJlbYeUcSiPGR/z10UR7bbl68o=\r\n"
        "OK-ACCESS-TIMESTAMP: 2020-12-08T09:08:57.715Z\r\n"
        "OK-ACCESS-PASSPHRASE: test-pass\r\n"
        "x-simulated-trading: 1\r\n");
  const std::string p = s.rest_headers(1607418537715, "POST", "/api/v5/trade/order", "{}", false);
  CHECK(p.find("Content-Type: application/json\r\n") != std::string::npos);
  CHECK(p.find("x-simulated-trading") == std::string::npos);
  // Three credentials: without the passphrase the signer is not usable.
  Credentials no_pass;
  no_pass.api_key = "k";
  no_pass.secret.value = "s";
  CHECK_FALSE(Signer(no_pass).usable());

  char buf[kMaxRequestBytes];
  const std::size_t n = OkxOrderEncoder::encode_login(s, 1538054050, buf);
  CHECK(
      std::string_view(buf, n) ==
      R"({"op":"login","args":[{"apiKey":"test-key","passphrase":"test-pass","timestamp":"1538054050","sign":"dCiowwyD1J8GrUtvJeV5ghkSUWKx/rcjDXnOOQdJ6UI="}]})");
}

TEST_CASE("okx.encoder: order, amend-order and cancel-order frames with instIdCode and clOrdId") {
  Universe u;
  OkxOrderEncoder enc(u.symbols);
  const OutNewOrderMsg post = new_order(OrderType::PostOnly, TimeInForce::Gtc);
  // No instIdCode: nothing on the WebSocket (the venue sends it over REST).
  CHECK(encode(enc, post.hdr, nullptr).empty());
  enc.set_inst_id_code(kBtc, 10459);
  CHECK(
      encode(enc, post.hdr, nullptr) ==
      R"({"id":"nfm000100000001","op":"order","args":[{"instIdCode":10459,"tdMode":"cross","clOrdId":"fm000100000001","side":"sell","ordType":"post_only","px":"60000.1","sz":"2.5"}]})");
  // The id and clOrdId fit OKX's rules: alphanumeric, at most 32 characters.
  const std::string id(make_request_id(RequestKind::Replace, post.cl_ord_id).view());
  CHECK(id.size() <= 32);
  for (const char c : id) CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')));

  OutNewOrderMsg ioc = new_order(OrderType::Limit, TimeInForce::Ioc);
  ioc.side = Side::Buy;
  ioc.reduce_only = 1;
  CHECK(
      encode(enc, ioc.hdr, nullptr) ==
      R"({"id":"nfm000100000001","op":"order","args":[{"instIdCode":10459,"tdMode":"cross","clOrdId":"fm000100000001","side":"buy","ordType":"ioc","px":"60000.1","sz":"2.5","reduceOnly":true}]})");
  const OutNewOrderMsg mkt = new_order(OrderType::Market, TimeInForce::Ioc);
  CHECK(encode(enc, mkt.hdr, nullptr).find(R"("ordType":"market","sz":"2.5"})") !=
        std::string::npos);
  CHECK(OkxOrderEncoder::ord_type_text(OrderType::Limit, TimeInForce::Fok) == "fok");
  CHECK(OkxOrderEncoder::ord_type_text(OrderType::Limit, TimeInForce::Gtc) == "limit");
  OkxOrderEncoder isolated(u.symbols, TdMode::Isolated);
  isolated.set_inst_id_code(kBtc, 10459);
  CHECK(encode(isolated, post.hdr, nullptr).find(R"("tdMode":"isolated")") != std::string::npos);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kBtc, kOkx);
  c.cl_ord_id = post.cl_ord_id;
  CHECK(
      encode(enc, c.hdr, nullptr) ==
      R"({"id":"cfm000100000001","op":"cancel-order","args":[{"instIdCode":10459,"clOrdId":"fm000100000001"}]})");
  c.venue_order_id.assign("1234567890");
  CHECK(
      encode(enc, c.hdr, nullptr) ==
      R"({"id":"cfm000100000001","op":"cancel-order","args":[{"instIdCode":10459,"ordId":"1234567890"}]})");

  // Amend: the venue's clOrdId (the original link id), the engine's new id as reqId, the total
  // size and the new price.
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, kBtc, kOkx);
  r.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
  r.orig_cl_ord_id = post.cl_ord_id;
  r.price = Price::from_decimal("60001").value();
  r.qty = Qty::from_decimal("3").value();
  CHECK(encode(enc, r.hdr, nullptr).empty());  // needs the shadow
  const OrderShadow shadow{
      kBtc, Side::Sell, OrderType::PostOnly, TimeInForce::Gtc, post.cl_ord_id, {}};
  CHECK(
      encode(enc, r.hdr, &shadow) ==
      R"({"id":"rfm000100000002","op":"amend-order","args":[{"instIdCode":10459,"clOrdId":"fm000100000001","reqId":"fm000100000002","newSz":"3","newPx":"60001"}]})");

  // REST takes instId instead.
  RestRequest rr;
  REQUIRE(enc.encode_rest(*OrderCommand::from(post.hdr), nullptr, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.path == "/api/v5/trade/order");
  CHECK(
      rr.body ==
      R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","clOrdId":"fm000100000001","side":"sell","ordType":"post_only","px":"60000.1","sz":"2.5"})");
  REQUIRE(enc.encode_rest(*OrderCommand::from(r.hdr), &shadow, rr));
  CHECK(rr.path == "/api/v5/trade/amend-order");
  REQUIRE(enc.encode_rest(*OrderCommand::from(c.hdr), nullptr, rr));
  CHECK(rr.path == "/api/v5/trade/cancel-order");
  CHECK(rr.body == R"({"instId":"BTC-USDT-SWAP","ordId":"1234567890"})");
}

TEST_CASE("okx.encoder: REST paths and bodies of the control requests") {
  RestRequest rr;
  const OkxOrderEncoder::CancelEntry batch[] = {{"BTC-USDT-SWAP", "1"}, {"BTC-USDT-SWAP", "2"}};
  REQUIRE(OkxOrderEncoder::encode_rest_cancel_batch(batch, rr));
  CHECK(rr.path == "/api/v5/trade/cancel-batch-orders");
  CHECK(rr.body ==
        R"([{"instId":"BTC-USDT-SWAP","ordId":"1"},{"instId":"BTC-USDT-SWAP","ordId":"2"}])");
  std::vector<OkxOrderEncoder::CancelEntry> many(21, {"BTC-USDT-SWAP", "1"});
  CHECK_FALSE(OkxOrderEncoder::encode_rest_cancel_batch(many, rr));  // at most 20
  REQUIRE(OkxOrderEncoder::encode_rest_cancel_all_after(60, rr));
  CHECK(rr.path == "/api/v5/trade/cancel-all-after");
  CHECK(rr.body == R"({"timeOut":"60"})");
  REQUIRE(OkxOrderEncoder::encode_rest_cancel_all_after(0, rr));
  CHECK(rr.body == R"({"timeOut":"0"})");
  CHECK_FALSE(OkxOrderEncoder::encode_rest_cancel_all_after(5, rr));    // 10..120
  CHECK_FALSE(OkxOrderEncoder::encode_rest_cancel_all_after(121, rr));  // 10..120
  OkxOrderEncoder::encode_rest_orders_pending("", rr);
  CHECK(rr.path == "/api/v5/trade/orders-pending?instType=SWAP&limit=100");
  OkxOrderEncoder::encode_rest_orders_pending("123", rr);
  CHECK(rr.path == "/api/v5/trade/orders-pending?instType=SWAP&limit=100&after=123");
  OkxOrderEncoder::encode_rest_fills(false, 1789299700000, 0, "", 100, rr);
  CHECK(rr.method == "GET");
  CHECK(rr.path == "/api/v5/trade/fills?instType=SWAP&begin=1789299700000&limit=100");
  OkxOrderEncoder::encode_rest_fills(true, 1789299700000, 1789299800000, "77", 100, rr);
  CHECK(rr.path ==
        "/api/v5/trade/fills-history?instType=SWAP&after=77&begin=1789299700000&end=1789299800000&"
        "limit=100");
  OkxOrderEncoder::encode_rest_funding_bills(false, 1789299700000, "", 100, rr);
  CHECK(rr.path == "/api/v5/account/bills?instType=SWAP&type=8&begin=1789299700000&limit=100");
  OkxOrderEncoder::encode_rest_funding_bills(true, 1789299700000, "9", 100, rr);
  CHECK(rr.path ==
        "/api/v5/account/bills-archive?instType=SWAP&type=8&after=9&begin=1789299700000&limit=100");
  OkxOrderEncoder::encode_rest_positions(rr);
  CHECK(rr.path == "/api/v5/account/positions?instType=SWAP");
  OkxOrderEncoder::encode_rest_account_config(rr);
  CHECK(rr.path == "/api/v5/account/config");
  char buf[kMaxRequestBytes];
  constexpr std::string_view kChannels[] = {"orders", "positions", "balance_and_position"};
  const std::size_t n = OkxOrderEncoder::encode_private_subscribe("private", kChannels, buf);
  CHECK(
      std::string_view(buf, n) ==
      R"({"id":"private","op":"subscribe","args":[{"channel":"orders","instType":"SWAP"},{"channel":"positions","instType":"SWAP","extraParams":"{\"updateInterval\":\"0\"}"},{"channel":"balance_and_position"}]})");
}

TEST_CASE("okx.decoder: order-operation replies, events and REST replies") {
  OkxResponseDecoder d;
  TradeResponse r = decode(
      d,
      R"({"id":"nfm000100000001","op":"order","data":[{"clOrdId":"fm000100000001","ordId":"12345689","tag":"","ts":"1695190491421","sCode":"0","sMsg":"","subCode":""}],"code":"0","msg":"","inTime":"1695190491421339","outTime":"1695190491423240"})");
  CHECK(r.ok());
  CHECK(r.id == "nfm000100000001");
  CHECK(r.op == "order");
  CHECK(r.ord_id == "12345689");
  CHECK(r.cl_ord_id == "fm000100000001");
  r = decode(
      d,
      R"({"id":"nfm000100000001","op":"order","data":[{"clOrdId":"fm000100000001","ordId":"","tag":"","ts":"","sCode":"51008","sMsg":"Order failed. Insufficient USDT margin in account","subCode":"51008_1000"}],"code":"1","msg":"","inTime":"1","outTime":"2"})");
  CHECK_FALSE(r.ok());
  CHECK(r.reason_code() == 51008);
  CHECK(r.reason_msg() == "Order failed. Insufficient USDT margin in account");
  // A request-level failure has no order: its code is the order's.
  r = decode(
      d,
      R"({"id":"cfm000100000001","op":"cancel-order","code":"60013","msg":"Invalid args","data":[]})");
  CHECK_FALSE(r.ok());
  CHECK(r.reason_code() == 60013);
  r = decode(
      d,
      R"({"id":"rfm000100000002","op":"amend-order","data":[{"clOrdId":"fm000100000001","ordId":"2510789768709120","reqId":"fm000100000002","ts":"1695190491421","sCode":"0","sMsg":""}],"code":"0","msg":""})");
  CHECK(r.ok());
  CHECK(r.req_id == "fm000100000002");
  r = decode(d, R"({"event":"login","code":"0","msg":"","connId":"a4d3ae55"})");
  CHECK(r.event == "login");
  CHECK(r.code == 0);
  r = decode(d, R"({"event":"error","code":"60009","msg":"Login failed.","connId":"a4d3ae55"})");
  CHECK(r.event == "error");
  CHECK(r.code == 60009);
  TradeResponse pong;
  const PaddedJson p("pong");
  CHECK(d.decode(p.view(), pong) == ParseStatus::Ignored);
  const PaddedJson bad("{\"id\":");
  CHECK(d.decode(bad.view(), pong) == ParseStatus::Malformed);
}

TEST_CASE("okx.rest_decoder: instruments, time, account, positions, orders, fills, bills") {
  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(fastmm::test::fixture("okx/instruments_btc_usdt_swap.json"), infos)
              .empty());
  REQUIRE(infos.size() == 1);
  const InstrumentInfo& i = infos[0];
  CHECK(i.inst_id == "BTC-USDT-SWAP");
  CHECK(i.inst_id_code == 10459);
  CHECK(i.inst_type == "SWAP");
  CHECK(i.ct_type == "linear");
  CHECK(i.ct_val == Qty::from_decimal("0.01").value());
  CHECK(i.ct_mult == Qty::from_int(1));
  CHECK(i.ct_val_ccy == "BTC");
  CHECK(i.settle_ccy == "USDT");
  CHECK(i.tick == Price::from_decimal("0.1").value());
  CHECK(i.lot == Qty::from_decimal("0.01").value());
  CHECK(i.min_sz == Qty::from_decimal("0.01").value());
  CHECK(i.max_limit_sz == Qty::from_int(100000000));
  CHECK(i.state == "live");
  infos.clear();
  REQUIRE(
      decode_instruments(fastmm::test::fixture("okx/instruments_btc_usdt_swap_demo.json"), infos)
          .empty());
  CHECK(infos[0].inst_id_code == 2021032601102993);  // demo codes differ
  CHECK(infos[0].tick == Price::from_decimal("0.01").value());
  CHECK_FALSE(
      decode_instruments(R"({"code":"51001","msg":"Instrument ID doesn't exist","data":[]})", infos)
          .empty());

  std::int64_t ms = 0;
  REQUIRE(decode_server_time(fastmm::test::fixture("okx/server_time.json"), ms).empty());
  CHECK(ms == 1790439440699);

  AccountConfig ac;
  REQUIRE(
      decode_account_config(
          R"({"code":"0","data":[{"acctLv":"2","posMode":"net_mode","uid":"44705892343619584"}],"msg":""})",
          ac)
          .empty());
  CHECK(ac.pos_mode == "net_mode");
  CHECK(ac.acct_lv == "2");

  std::vector<PositionRecord> pos;
  REQUIRE(
      decode_positions(
          R"({"code":"0","msg":"","data":[{"instId":"BTC-USDT-SWAP","posSide":"net","pos":"-3.5","avgPx":"60123.4","mgnMode":"cross","uTime":"1"}]})",
          pos)
          .empty());
  REQUIRE(pos.size() == 1);
  CHECK(pos[0].qty == Qty::from_decimal("-3.5").value());
  CHECK(pos[0].avg_px == Price::from_decimal("60123.4").value());

  std::vector<PendingOrder> oo;
  REQUIRE(
      decode_pending_orders(
          R"({"code":"0","msg":"","data":[{"instId":"BTC-USDT-SWAP","ordId":"312","clOrdId":"fm000100000001","px":"60000.1","sz":"2","side":"buy","state":"partially_filled","accFillSz":"0.5","ordType":"post_only"}]})",
          oo)
          .empty());
  REQUIRE(oo.size() == 1);
  CHECK(oo[0].acc_fill_sz == Qty::from_decimal("0.5").value());
  CHECK(oo[0].state == "partially_filled");

  std::vector<FillRecord> fills;
  REQUIRE(
      decode_fills(
          R"({"code":"0","msg":"","data":[{"instType":"SWAP","instId":"BTC-USDT-SWAP","tradeId":"123","ordId":"312","clOrdId":"fm000100000001","billId":"9001","subType":"1","tag":"","fillPx":"60000.1","fillSz":"0.5","fillIdxPx":"","fillPnl":"0","fillMarkPx":"","side":"buy","posSide":"net","execType":"M","feeCcy":"USDT","fee":"-0.06","ts":"1789299703460","fillTime":"1789299703453","feeRate":""}]})",
          fills)
          .empty());
  REQUIRE(fills.size() == 1);
  CHECK(fills[0].fee == Notional::from_decimal("-0.06").value());
  CHECK(fills[0].fill_time_ms == 1789299703453);
  CHECK(fills[0].ts_ms == 1789299703460);
  CHECK(fills[0].bill_id == "9001");

  std::vector<BillRecord> bills;
  REQUIRE(
      decode_bills(
          R"({"code":"0","msg":"","data":[{"billId":"7001","instId":"BTC-USDT-SWAP","ccy":"USDT","balChg":"-0.25","bal":"100","type":"8","subType":"173","ts":"1789300000000","pnl":"-0.25","fee":"0"}]})",
          bills)
          .empty());
  REQUIRE(bills.size() == 1);
  CHECK(bills[0].bal_chg == Notional::from_decimal("-0.25").value());
  CHECK(bills[0].type == "8");

  std::vector<std::string> failed;
  REQUIRE(
      decode_cancel_batch(
          R"({"code":"2","msg":"","data":[{"ordId":"1","clOrdId":"","sCode":"0","sMsg":""},{"ordId":"2","clOrdId":"","sCode":"51400","sMsg":"gone"},{"ordId":"3","clOrdId":"","sCode":"50011","sMsg":"Rate limit"}]})",
          failed)
          .empty());
  REQUIRE(failed.size() == 1);
  CHECK(failed[0] == "3: 50011 Rate limit");
  int code = -1;
  std::string msg;
  REQUIRE(decode_envelope(R"({"code":"50113","msg":"Invalid Sign","data":[]})", code, msg));
  CHECK(code == 50113);
}

TEST_CASE("okx.error_map: documented codes") {
  CHECK(parse_code("0") == 0);
  CHECK(parse_code("51008") == 51008);
  CHECK(parse_code("51008_1000") == 51008);
  CHECK(parse_code("") == -1);
  CHECK(parse_code("x1") == -1);
  CHECK(map_error(0).action == VenueAction::None);
  CHECK(map_error(50011).action == VenueAction::RateLimit);
  CHECK(map_error(50011).reason == RejectReason::VenueRateLimit);
  CHECK(map_error(50061).action == VenueAction::RateLimit);
  CHECK(map_error(50102).action == VenueAction::ResyncClock);
  CHECK(map_error(50113).action == VenueAction::Fatal);
  CHECK(map_error(50105).action == VenueAction::Fatal);
  CHECK(map_error(60009).action == VenueAction::Fatal);
  CHECK(map_error(60024).action == VenueAction::Fatal);
  CHECK(map_error(59113).action == VenueAction::Fatal);
  CHECK(map_error(50121).action == VenueAction::HardStop);
  CHECK(map_error(50004).action == VenueAction::Reconcile);
  CHECK(map_error(51008).reason == RejectReason::InsufficientBalance);
  CHECK(map_error(51016).reason == RejectReason::DuplicateId);
  CHECK(map_error(51121).reason == RejectReason::InvalidLot);
  CHECK(map_error(51006).reason == RejectReason::PriceCollar);
  CHECK(map_error(51400).reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error(51400).action == VenueAction::Reconcile);
  CHECK(map_error(51511).reason == RejectReason::PostOnlyWouldCross);
  CHECK(map_error(51001).action == VenueAction::DisableInstrument);
  CHECK(map_error(51000, "Parameter px error").reason == RejectReason::InvalidTick);
  CHECK(map_error(50001).action == VenueAction::Backoff);
  CHECK(map_error(51010).action == VenueAction::Fatal);
  CHECK(map_error(12345).known == false);
  CHECK(map_http_status(429).action == VenueAction::RateLimit);
  CHECK(map_http_status(503).action == VenueAction::Backoff);
}
