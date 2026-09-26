// Venue connector hot paths do not allocate after warm-up: the market-data parser, the private
// (user stream) parser and the order encoder of Binance, Bybit, OKX and Deribit, on the recorded
// fixtures in tests/fixtures/<venue>/. The first pass over the frames is the warm-up (parser
// buffers reach their working size); the measured rounds must then decode and encode everything
// without a single allocation.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance/binance_sbe_md_parser.hpp"
#include "fastmm/venues/binance/binance_user_parser.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"
#include "fastmm/venues/bybit/bybit_auth.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/bybit/bybit_order_encoder.hpp"
#include "fastmm/venues/bybit/bybit_private_parser.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"
#include "fastmm/venues/deribit/deribit_order_encoder.hpp"
#include "fastmm/venues/deribit/deribit_private_parser.hpp"
#include "fastmm/venues/okx/okx_md_feed.hpp"
#include "fastmm/venues/okx/okx_md_parser.hpp"
#include "fastmm/venues/okx/okx_order_encoder.hpp"
#include "fastmm/venues/okx/okx_private_parser.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr int kRounds = 200;

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

std::vector<PaddedJson> frames(std::initializer_list<const char*> names) {
  std::vector<PaddedJson> out;
  for (const char* n : names) out.push_back(padded_fixture(n));
  return out;
}

// Warm-up pass, then kRounds passes under NoAllocScope; every frame must decode each time.
template <class Parser>
void check_decoder_noalloc(Parser& parser, const std::vector<PaddedJson>& in) {
  Scratch s;
  for (const PaddedJson& f : in)
    REQUIRE(parser.decode(f.view(), Timestamp{1}, Cycles{1}, s.span()).ok());
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        if (!parser.decode(f.view(), Timestamp{round}, Cycles{1}, s.span()).ok()) ++failed;
      }
    }
  }
  CHECK(failed == 0);
}

// The Deribit test universe: 0 BTC-15SEP26-77000-C (option), 1 BTC-PERPETUAL (10 USD contracts).
struct DeribitUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  std::array<deribit::TickSchedule, kMaxInstruments> ticks{};
  DeribitUniverse() {
    Instrument call = make_instrument("BTC-15SEP26-77000-C", 2, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.option_type = OptionType::Call;
    call.tick = px("0.0001");
    call.lot = qt("0.1");
    REQUIRE(instruments.add(call));
    Instrument perp = make_instrument("BTC-PERPETUAL", 2, "BTC", "USD");
    perp.asset_class = AssetClass::Perpetual;
    perp.tick = px("0.5");
    perp.lot = qt("1");
    perp.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(perp));
    REQUIRE(symbols.build(instruments));
    ticks[0].base = px("0.0001");
    REQUIRE(ticks[0].steps.push_back(deribit::TickStep{px("0.005"), px("0.0005")}));
    ticks[1].base = px("0.5");
  }
};

// New, cancel and replace commands for one instrument (ids fm000100000001 / 2).
struct Commands {
  OutNewOrderMsg new_order{};
  OutCancelMsg cancel{};
  OutReplaceMsg replace{};
  Commands(InstrumentId id, VenueId venue, const char* price, const char* qty) {
    init_header(new_order, EventType::OutNewOrder, id, venue);
    new_order.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    new_order.side = Side::Buy;
    new_order.type = OrderType::PostOnly;
    new_order.tif = TimeInForce::Gtc;
    new_order.price = px(price);
    new_order.qty = qt(qty);
    init_header(cancel, EventType::OutCancel, id, venue);
    cancel.cl_ord_id = new_order.cl_ord_id;
    cancel.venue_order_id.assign("42710123456");
    init_header(replace, EventType::OutReplace, id, venue);
    replace.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
    replace.orig_cl_ord_id = new_order.cl_ord_id;
    replace.price = new_order.price;
    replace.qty = new_order.qty;
  }
  [[nodiscard]] std::array<OrderCommand, 3> all() const {
    return {*OrderCommand::from(new_order.hdr),
            *OrderCommand::from(cancel.hdr),
            *OrderCommand::from(replace.hdr)};
  }
};

// Warm-up encode of every command, then kRounds rounds under NoAllocScope; `encode(cmd, out)`
// returns the bytes written and must never return 0.
template <class Encode>
void check_encoder_noalloc(const std::array<OrderCommand, 3>& cmds, Encode&& encode) {
  std::array<char, 4096> buf{};
  for (const OrderCommand& c : cmds) REQUIRE(encode(c, std::span<char>(buf)) > 0);
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const OrderCommand& c : cmds) {
        if (encode(c, std::span<char>(buf)) == 0) ++failed;
      }
    }
  }
  CHECK(failed == 0);
}

}  // namespace

TEST_CASE("hotpath.noalloc: Binance market-data parser, user parser and order encoder") {
  TestUniverse u;
  {
    binance::BinanceMdParser md(u.symbols, VenueId{0});
    check_decoder_noalloc(md,
                          frames({"binance/depth_update.json",
                                  "binance/depth_update_20.json",
                                  "binance/depth_update_100.json",
                                  "binance/book_ticker.json",
                                  "binance/trade.json"}));
  }
  {
    binance::BinanceUserParser user(u.symbols, u.instruments, VenueId{0});
    check_decoder_noalloc(user,
                          frames({"binance/exec_report_new.json",
                                  "binance/exec_report_trade.json",
                                  "binance/exec_report_canceled.json",
                                  "binance/exec_report_rejected.json"}));
  }
  binance::Credentials creds;
  creds.api_key = "test-key";
  creds.secret.value = "test-secret";
  const binance::Signer signer(creds);
  binance::BinanceOrderEncoder enc(signer, u.symbols, 3000);
  const binance::OrderShadow shadow{
      Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, InstrumentId{0}};
  const Commands cmds(InstrumentId{0}, VenueId{0}, "70000.5", "0.001");
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return enc.encode_ws(c, &shadow, 1789295199000, out);
  });
  // Ed25519 key after session.logon: unsigned requests.
  binance::Credentials ed;
  ed.api_key = "test-key";
  ed.type = binance::KeyType::Ed25519;
  ed.private_key_pem.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  const binance::Signer ed_signer(ed);
  REQUIRE(ed_signer.usable());
  binance::BinanceOrderEncoder session(ed_signer, u.symbols, 3000);
  session.set_session_authenticated(true);
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return session.encode_ws(c, &shadow, 1789295199000, out);
  });
}

TEST_CASE("hotpath.noalloc: Binance SBE market-data decoder") {
  TestUniverse u;
  binance::BinanceSbeMdParser p(u.symbols, VenueId{0});
  std::vector<std::vector<std::byte>> in;
  for (const char* name : {"binance/sbe/depth_diff.hex",
                           "binance/sbe/best_bid_ask.hex",
                           "binance/sbe/trades.hex",
                           "binance/sbe/depth_snapshot20.hex"}) {
    std::vector<std::byte> f;
    int high = -1;
    bool comment = false;
    for (const char c : fastmm::test::fixture(name)) {
      if (c == '\n') comment = false;
      if (c == '#') comment = true;
      if (comment) continue;
      const int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (v < 0) continue;
      if (high < 0) {
        high = v;
      } else {
        f.push_back(static_cast<std::byte>(high * 16 + v));
        high = -1;
      }
    }
    in.push_back(std::move(f));
  }
  alignas(64) static std::byte scratch[kDecoderScratchBytes];
  std::uint64_t emitted = 0;
  auto sink = [&](EventHeader&, MdKind) { ++emitted; };
  for (const auto& f : in)
    REQUIRE(p.decode(f, Timestamp{}, Cycles{}, scratch, sink) == ParseStatus::Ok);
  {
    NoAllocScope guard;
    for (int r = 0; r < 100; ++r) {
      for (const auto& f : in) {
        if (p.decode(f, Timestamp{}, Cycles{}, scratch, sink) != ParseStatus::Ok) FAIL("decode");
      }
    }
  }
  CHECK(emitted == 5 * 101);
}

TEST_CASE("hotpath.noalloc: Binance USD-M market-data parser, user parser and order encoder") {
  TestUniverse u;
  {
    binance_usdm::BinanceUsdmMdParser md(u.symbols, VenueId{0});
    check_decoder_noalloc(md,
                          frames({"binance_usdm/depth_update.json",
                                  "binance_usdm/book_ticker.json",
                                  "binance_usdm/agg_trade.json"}));
  }
  {
    binance_usdm::BinanceUsdmUserParser user(u.symbols, u.instruments, VenueId{0});
    check_decoder_noalloc(user,
                          frames({"binance_usdm/order_update_new.json",
                                  "binance_usdm/order_update_trade.json",
                                  "binance_usdm/order_update_canceled.json",
                                  "binance_usdm/order_update_expired.json",
                                  "binance_usdm/account_update.json"}));
  }
  binance::Credentials creds;
  creds.api_key = "test-key";
  creds.secret.value = "test-secret";
  const binance::Signer signer(creds);
  binance_usdm::BinanceUsdmOrderEncoder enc(signer, u.symbols, 3000);
  binance_usdm::OrderShadow shadow;
  shadow.instrument = InstrumentId{0};
  shadow.type = OrderType::PostOnly;
  const Commands cmds(InstrumentId{0}, VenueId{0}, "70000.5", "0.001");
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return enc.encode_ws(c, &shadow, 1789295199000, out);
  });
}

TEST_CASE("hotpath.noalloc: Bybit market-data parser, private parser and order encoder") {
  TestUniverse u;
  {
    bybit::BybitMdParser md(u.symbols, VenueId{1});
    check_decoder_noalloc(md,
                          frames({"bybit/orderbook50_snapshot.json",
                                  "bybit/orderbook50_delta.json",
                                  "bybit/orderbook1_snapshot.json",
                                  "bybit/public_trade.json"}));
  }
  {
    bybit::BybitPrivateParser priv(u.symbols, u.instruments, VenueId{1});
    check_decoder_noalloc(priv,
                          frames({"bybit/private_order_new.json",
                                  "bybit/private_order_cancelled.json",
                                  "bybit/private_order_rejected.json",
                                  "bybit/private_execution.json",
                                  "bybit/private_wallet.json"}));
  }
  bybit::Credentials creds;
  creds.api_key = "test-key";
  creds.secret.value = "test-secret";
  const bybit::Signer signer(creds);
  const bybit::BybitOrderEncoder enc(signer, u.symbols, 5000);
  const Commands cmds(InstrumentId{2}, VenueId{1}, "60000.1", "0.001");
  const bybit::OrderShadow shadow{InstrumentId{2},
                                  Side::Buy,
                                  OrderType::PostOnly,
                                  TimeInForce::Gtc,
                                  cmds.new_order.cl_ord_id,
                                  {}};
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return enc.encode_ws(c, &shadow, 1789299700000, out);
  });
}

TEST_CASE("hotpath.noalloc: OKX market-data parser and feed, private parser and order encoder") {
  InstrumentTable instruments;
  Instrument swap = make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT");
  swap.contract_multiplier = qt("0.01");
  REQUIRE(instruments.add(swap));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  // The data frames of the recorded production session (books snapshot and updates, bbo-tbt,
  // trades), in order.
  std::vector<PaddedJson> md_frames;
  {
    const std::string raw = fastmm::test::fixture("okx/raw_md_stream.jsonl");
    std::size_t pos = 0;
    while (pos < raw.size()) {
      std::size_t end = raw.find('\n', pos);
      if (end == std::string::npos) end = raw.size();
      const std::string line = raw.substr(pos, end - pos);
      pos = end + 1;
      const std::size_t tab = line.find('\t');
      if (tab != std::string::npos && line.find("\"data\"") != std::string::npos)
        md_frames.emplace_back(line.substr(tab + 1));
    }
  }
  REQUIRE(md_frames.size() > 100);
  {
    okx::OkxMdParser md(symbols, VenueId{1});
    check_decoder_noalloc(md, md_frames);
  }
  {
    // The feed, with the checksum shadow in use: a book whose snapshot carried a checksum keeps
    // every level's text. A snapshot and an update that removes and adds levels, over and over.
    fastmm::venues::test::RecordingSink sink(8U << 20);
    okx::OkxMdFeed feed(symbols, VenueId{1}, sink.sink, okx::ResubscribeRequester{});
    REQUIRE(feed.add_instrument(InstrumentId{0}));
    feed.on_connected();
    const PaddedJson snap(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["8476.98","415","0","13"],["8477","7","0","2"]],"bids":[["8476.97","256","0","12"],["8475.55","101","0","1"]],"ts":"1597026383085","checksum":2123921068,"prevSeqId":-1,"seqId":10}]})");
    const PaddedJson update(
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[["8477","0","0","0"]],"bids":[],"ts":"1597026383085","checksum":214565906,"prevSeqId":10,"seqId":11}]})");
    int failed = 0;
    for (int warm = 0; warm < 2; ++warm) {
      REQUIRE(feed.on_message(snap.view(), 1) == ParseStatus::Ok);
      REQUIRE(feed.on_message(update.view(), 1) == ParseStatus::Ok);
      static_cast<void>(sink.drain());
    }
    {
      NoAllocScope guard;
      for (int round = 0; round < kRounds; ++round) {
        if (feed.on_message(snap.view(), 1) != ParseStatus::Ok) ++failed;
        if (feed.on_message(update.view(), 1) != ParseStatus::Ok) ++failed;
        while (sink.ring.try_peek() != nullptr) sink.ring.release();
      }
    }
    CHECK(failed == 0);
    CHECK(feed.stats().checksum_errors == 0);
    CHECK(feed.stats().checksums_checked >= 2U * kRounds);
  }
  {
    okx::OkxPrivateParser priv(symbols, instruments, VenueId{1});
    const std::string order =
        R"({"arg":{"channel":"orders","instType":"SWAP","uid":"77"},"data":[{"instId":"BTC-USDT-SWAP","ordId":"312","clOrdId":"fm000100000001","px":"60000.1","sz":"3","side":"sell","accFillSz":"1","state":"partially_filled","fillPx":"60000.1","tradeId":"4463701411","fillSz":"1","fillTime":"1789299703453","fillFee":"-0.018","fillFeeCcy":"USDT","execType":"T","uTime":"1789299703470","reqId":"","amendResult":"","cancelSource":"","code":"0","msg":""}]})";
    std::string live = order;
    live.replace(live.find("partially_filled"), 16, "live");
    live.replace(live.find(R"("fillSz":"1")"), 12, R"("fillSz":"0")");
    std::string cancel = order;
    cancel.replace(cancel.find("partially_filled"), 16, "canceled");
    std::vector<PaddedJson> private_frames;
    private_frames.emplace_back(live);
    private_frames.emplace_back(order);
    private_frames.emplace_back(cancel);
    private_frames.emplace_back(
        R"({"arg":{"channel":"positions","instType":"SWAP","uid":"77"},"eventType":"event_update","data":[{"instId":"BTC-USDT-SWAP","posSide":"net","pos":"-3.5","avgPx":"60123.4","uTime":"1"}]})");
    check_decoder_noalloc(priv, private_frames);
  }
  okx::OkxOrderEncoder enc(symbols);
  enc.set_inst_id_code(InstrumentId{0}, 10459);
  const Commands cmds(InstrumentId{0}, VenueId{1}, "60000.1", "2");
  const okx::OrderShadow shadow{InstrumentId{0},
                                Side::Buy,
                                OrderType::PostOnly,
                                TimeInForce::Gtc,
                                cmds.new_order.cl_ord_id,
                                {}};
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return enc.encode_ws(c, &shadow, out);
  });
}

TEST_CASE("hotpath.noalloc: Deribit market-data parser, private parser and order encoder") {
  DeribitUniverse u;
  {
    deribit::DeribitMdParser md(u.symbols, u.instruments, VenueId{2});
    check_decoder_noalloc(md,
                          frames({"deribit/book_option_snapshot.json",
                                  "deribit/book_option_change.json",
                                  "deribit/book_perp_snapshot.json",
                                  "deribit/book_perp_change_1.json",
                                  "deribit/ticker_option.json",
                                  "deribit/ticker_perp.json",
                                  "deribit/trades_perp.json"}));
  }
  {
    deribit::DeribitPrivateParser priv(u.symbols, u.instruments, VenueId{2});
    check_decoder_noalloc(priv,
                          frames({"deribit/user_orders_open.json",
                                  "deribit/user_orders_cancelled.json",
                                  "deribit/user_trades.json"}));
  }
  const deribit::DeribitOrderEncoder enc(u.symbols, u.instruments, u.ticks, true);
  const Commands cmds(InstrumentId{0}, VenueId{2}, "0.0065", "0.5");
  deribit::OrderShadow shadow;
  shadow.instrument = InstrumentId{0};
  shadow.side = Side::Buy;
  shadow.type = OrderType::PostOnly;
  shadow.label = cmds.new_order.cl_ord_id;
  shadow.venue_order_id.assign("42710123456");
  shadow.qty = qt("0.5");
  check_encoder_noalloc(cmds.all(), [&](const OrderCommand& c, std::span<char> out) {
    return enc.encode(c, &shadow, "access-token", out);
  });
}

#endif  // FASTMM_HOTPATH_VENUES
