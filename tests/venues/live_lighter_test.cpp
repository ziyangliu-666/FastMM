// Lighter, against the real venue (opt-in: FASTMM_LIVE_TESTS=1). Three checks:
//
//   live.lighter: reference data   no keys; orderBookDetails of FASTMM_LIGHTER_SYMBOL (default
//                                  NVDA): market index, decimals, tick, lot, minimums; and the
//                                  time one signature takes on this host.
//   live.lighter: market data      no keys; the order_book snapshot, the ticker, the trades.
//   live.lighter: one post-only    places ONE post-only buy at 80 % of the best bid (the smallest
//                                  size above the minimums), waits for its open on the account
//                                  stream, modifies it, cancels it, and prints send -> reply,
//                                  send -> open (the venue's time) and send -> cancel ack. Needs
//                                  FASTMM_LIGHTER_PLACE_ORDER=1 on top, FASTMM_LIGHTER_ACCOUNT (the
//                                  account index), FASTMM_LIGHTER_API_KEY_INDEX (the slot) and the
//                                  API key in the variable FASTMM_LIGHTER_KEY_ENV names (default
//                                  FASTMM_LIGHTER_API_KEY). FASTMM_LIGHTER_TESTNET=1 for the
//                                  testnet. Spends two units of volume quota on a Premium account.
//
// FASTMM_LIGHTER_RECORD_DIR records the raw frames of every connection (RawRecorder).
#include "live_test_util.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/lighter/lighter_venue.hpp"

#include <fmt/format.h>

#include <chrono>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::lighter;
using namespace fastmm::venues::test;

namespace {

std::string symbol_under_test() {
  std::string s = env_or_empty("FASTMM_LIGHTER_SYMBOL");
  return s.empty() ? std::string("NVDA") : s;
}

VenueSection live_section() {
  VenueSection s;
  s.name = "lighter-live";
  s.kind = "lighter";
  s.testnet = env_or_empty("FASTMM_LIGHTER_TESTNET") == "1";
  s.api_key = env_or_empty("FASTMM_LIGHTER_ACCOUNT");
  std::string key_env = env_or_empty("FASTMM_LIGHTER_KEY_ENV");
  if (key_env.empty()) key_env = "FASTMM_LIGHTER_API_KEY";
  s.api_secret = env_or_empty(key_env.c_str());
  const std::string slot = env_or_empty("FASTMM_LIGHTER_API_KEY_INDEX");
  if (!slot.empty()) s.extra["api_key_index"] = slot;
  s.extra["latency_log_s"] = "0";
  return s;
}

VenueSection public_section() {
  VenueSection s = live_section();
  s.api_key.clear();
  s.api_secret.clear();
  s.extra.erase("api_key_index");
  return s;
}

}  // namespace

TEST_CASE("live.lighter: reference data of the market") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to read Lighter's markets");
    return;
  }
  LighterVenue venue(VenueId{0}, make_lighter_config(public_section(), true));
  InstrumentTable instruments;
  const std::string sym = symbol_under_test();
  REQUIRE(instruments.add(make_instrument(sym.c_str(), 0, "X", "USDC")));
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  const Instrument& i = instruments.get(InstrumentId{0});
  MESSAGE(fmt::format(
      "{}: market {} size decimals {} price decimals {} tick {} lot {} min qty {} min notional {}",
      sym,
      venue.market_of(InstrumentId{0}),
      venue.size_decimals_of(InstrumentId{0}),
      venue.price_decimals_of(InstrumentId{0}),
      DecimalText(i.tick).view(),
      DecimalText(i.lot).view(),
      DecimalText(i.min_qty).view(),
      DecimalText(i.min_notional).view()));
  CHECK(i.tick.is_positive());
  // What one signature costs here.
  const Signer s(Signer::generate_private_key_hex());
  REQUIRE(s.usable());
  CreateOrderTx tx;
  tx.h.account_index = 1;
  tx.market_index = static_cast<std::int16_t>(venue.market_of(InstrumentId{0}));
  constexpr int kN = 200;
  SigBytes sig{};
  const auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < kN; ++k) {
    tx.h.nonce = k;
    REQUIRE(s.sign(tx_hash(tx, kMainnetChainId), sig));
  }
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  MESSAGE(fmt::format("hash + sign: {:.1f} us each", static_cast<double>(us) / kN));
}

TEST_CASE("live.lighter: public market data through the connector") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to read Lighter's market data");
    return;
  }
  LighterVenueConfig cfg = make_lighter_config(public_section(), true);
  cfg.record_raw_dir = env_or_empty("FASTMM_LIGHTER_RECORD_DIR");
  LighterVenue venue(VenueId{0}, cfg);
  InstrumentTable instruments;
  const std::string sym = symbol_under_test();
  REQUIRE(instruments.add(make_instrument(sym.c_str(), 0, "X", "USDC")));
  REQUIRE(venue.load_reference_data(instruments));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);
  Collected mc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        mc.take(md);
        venue.on_timer(net::Reactor::now_ns());
        return mc.count(EventType::BookSnapshot) > 0 && mc.count(EventType::BookTicker) > 0 &&
               mc.count(EventType::BookDelta) > 5;
      },
      30'000));
  const auto* b = mc.last<BookDeltaMsg>(EventType::BookSnapshot);
  const auto* t = mc.last<BookTickerMsg>(EventType::BookTicker);
  MESSAGE(fmt::format(
      "{}: book {}x{} levels, {} updates, bbo {} / {}, {} trade(s); venue time - receive {} ms",
      sym,
      b->bid_count,
      b->ask_count,
      mc.count(EventType::BookDelta),
      DecimalText(t->bid_px).view(),
      DecimalText(t->ask_px).view(),
      mc.count(EventType::Trade),
      (t->hdr.exch_ts.ns - t->hdr.recv_ts.ns) / 1'000'000));
  CHECK(t->bid_px < t->ask_px);
  CHECK(venue.status().books_synced == 1);
  CHECK(venue.status().resyncs == 0);
  venue.disconnect();
}

TEST_CASE("live.lighter: one post-only far from the market, a modify and its cancel") {
  if (!live_tests_enabled() || env_or_empty("FASTMM_LIGHTER_PLACE_ORDER") != "1") {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 and FASTMM_LIGHTER_PLACE_ORDER=1 to place one order");
    return;
  }
  const VenueSection s = live_section();
  if (s.api_key.empty() || s.api_secret.empty() || !s.extra.contains("api_key_index")) {
    MESSAGE(
        "skipped: FASTMM_LIGHTER_ACCOUNT, FASTMM_LIGHTER_API_KEY_INDEX and the API key not set");
    return;
  }
  LighterVenueConfig cfg = make_lighter_config(s, false);
  cfg.record_raw_dir = env_or_empty("FASTMM_LIGHTER_RECORD_DIR");
  cfg.channels.trades = false;
  cfg.supports_replace = true;
  LighterVenue venue(VenueId{0}, cfg);
  InstrumentTable instruments;
  const std::string sym = symbol_under_test();
  REQUIRE(instruments.add(make_instrument(sym.c_str(), 0, "X", "USDC")));
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  MESSAGE(fmt::format("next nonce {}", venue.next_nonce()));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);
  Price best_bid{};
  REQUIRE(pump_until(
      reactor,
      [&] {
        for (const auto& m : md.drain()) {
          if (RecordingSink::type_of(m) == EventType::BookTicker)
            best_bid = RecordingSink::as<BookTickerMsg>(m).bid_px;
        }
        venue.on_timer(net::Reactor::now_ns());
        return best_bid.is_positive() && venue.order_channel_live() && venue.private_channel_live();
      },
      30'000));
  Collected oc;
  pump_until(
      reactor,
      [&] {
        oc.take(orders);
        venue.on_timer(net::Reactor::now_ns());
        return false;
      },
      2000);  // the start-up sweep
  const Instrument& inst = instruments.get(InstrumentId{0});
  const OutNewOrderMsg n =
      far_passive_buy(inst, best_bid, ClientOrderId{(std::uint64_t{0x7e} << 32) | 1});
  MESSAGE(fmt::format("placing post-only buy {} @ {} (best bid {})",
                      DecimalText(n.qty).view(),
                      DecimalText(n.price).view(),
                      DecimalText(best_bid).view()));
  auto wait = [&](ClientOrderId id, EventType t) -> const EventHeader* {
    const EventHeader* found = nullptr;
    pump_until(
        reactor,
        [&] {
          oc.take(orders);
          venue.on_timer(net::Reactor::now_ns());
          for (const auto& m : oc.all) {
            const auto* h = reinterpret_cast<const EventHeader*>(m.data());
            if (h->type == EventType::OrderReject &&
                RecordingSink::as<OrderRejectMsg>(m).cl_ord_id == id)
              FAIL("rejected: " << std::string(RecordingSink::as<OrderRejectMsg>(m).text.view()));
            if (h->type != t) continue;
            const ClientOrderId got = t == EventType::OrderAck
                                          ? RecordingSink::as<OrderAckMsg>(m).cl_ord_id
                                          : RecordingSink::as<OrderCancelAckMsg>(m).cl_ord_id;
            if (got == id) found = h;
          }
          return found != nullptr;
        },
        15'000);
    return found;
  };
  const std::int64_t sent_ms = wall_now().ns / 1'000'000;
  REQUIRE(outbound.try_push(&n, n.hdr.len));
  venue.on_wake();
  const auto* ack = reinterpret_cast<const OrderAckMsg*>(wait(n.cl_ord_id, EventType::OrderAck));
  REQUIRE(ack != nullptr);
  MESSAGE(fmt::format("open: order index {}, received {} ms after send, venue time send + {} ms",
                      std::string(ack->venue_order_id.view()),
                      ack->hdr.recv_ts.ns / 1'000'000 - sent_ms,
                      ack->hdr.exch_ts.ns / 1'000'000 - sent_ms));
  OutReplaceMsg rp{};
  init_header(rp, EventType::OutReplace, inst.id, inst.venue);
  rp.cl_ord_id = ClientOrderId{(std::uint64_t{0x7e} << 32) | 2};
  rp.orig_cl_ord_id = n.cl_ord_id;
  rp.price = inst.round_price(Price::from_raw(best_bid.raw / 100 * 79), Side::Buy);
  rp.qty = n.qty;
  const std::int64_t modify_ms = wall_now().ns / 1'000'000;
  REQUIRE(outbound.try_push(&rp, rp.hdr.len));
  venue.on_wake();
  const auto* mod = reinterpret_cast<const OrderAckMsg*>(wait(rp.cl_ord_id, EventType::OrderAck));
  REQUIRE(mod != nullptr);
  CHECK((mod->flags & OrderAckMsg::kAmendedInPlace) != 0);
  MESSAGE(fmt::format("modify to {} accepted {} ms after send",
                      DecimalText(rp.price).view(),
                      mod->hdr.recv_ts.ns / 1'000'000 - modify_ms));
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, inst.id, inst.venue);
  c.cl_ord_id = rp.cl_ord_id;
  const std::int64_t cancel_ms = wall_now().ns / 1'000'000;
  REQUIRE(outbound.try_push(&c, c.hdr.len));
  venue.on_wake();
  const auto* ca =
      reinterpret_cast<const OrderCancelAckMsg*>(wait(c.cl_ord_id, EventType::OrderCancelAck));
  REQUIRE(ca != nullptr);
  MESSAGE(fmt::format("cancel acked {} ms after send (venue time send + {} ms)",
                      ca->hdr.recv_ts.ns / 1'000'000 - cancel_ms,
                      ca->hdr.exch_ts.ns / 1'000'000 - cancel_ms));
  CHECK(ca->cum_qty.is_zero());
  CHECK_FALSE(venue.fatal());
  CHECK(venue.shadow_count() == 0);
  MESSAGE(fmt::format(
      "volume quota left: {}, next nonce {}", venue.quota_remaining(), venue.next_nonce()));
  venue.disconnect();
}
