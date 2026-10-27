// Opt-in check of the Alpaca connector on a paper account: FASTMM_LIVE_TESTS=1 with
// ALPACA_API_KEY and ALPACA_API_SECRET exported (scripts/alpaca-paper-check.sh). On SPY it places a
// buy far below the market, replaces and cancels it, buys one share and sells it again with
// marketable limit orders (extended hours, so it runs from 04:00 to 20:00 New York time), drops
// and reopens the connection, starts a second connector over a resting order the way a restarted
// session does, and ends with the kill-switch cancel-all. Paper endpoints only: the config guard
// refuses the live trading host.
#include "live_test_util.hpp"

#include "fastmm/venues/alpaca/alpaca_venue.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::alpaca;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{0};
const InstrumentId kSpy{0};

struct Session {
  InstrumentTable instruments;
  SymbolTable symbols;
  RecordingSink md{4U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  net::Reactor reactor;
  std::unique_ptr<AlpacaVenue> venue;
  Collected oc;
  Collected mc;

  explicit Session(const VenueSection& s) {
    Instrument i = make_instrument("SPY", 0, "SPY", "USD");
    i.asset_class = AssetClass::Equity;
    i.lot = Qty::from_int(1);
    REQUIRE(instruments.add(i));
    AlpacaVenueConfig cfg = make_alpaca_config(s, false);
    cfg.orders.extended_hours = true;
    cfg.record_raw_dir = env_or_empty("FASTMM_LIVE_RECORD_DIR");
    venue = std::make_unique<AlpacaVenue>(kVenue, std::move(cfg));
    const auto ref = venue->load_reference_data(instruments);
    REQUIRE_MESSAGE(ref.has_value(), ref.error());
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, nullptr);
    const InstrumentId ids[] = {kSpy};
    venue->subscribe(ids);
    venue->connect(reactor);
  }
  ~Session() {
    if (venue) venue->disconnect();
    reactor.run_once(0);
  }
  template <class Pred>
  bool wait(Pred pred, int timeout_ms) {
    return pump_until(
        reactor,
        [&] {
          oc.take(orders);
          mc.take(md);
          return pred();
        },
        timeout_ms);
  }
  void send(const EventHeader& h) {
    const EventHeader* batch[] = {&h};
    venue->send_now(batch);
  }
  std::size_t reconciles() const {
    std::size_t n = 0;
    for (const auto& m : oc.all)
      n += RecordingSink::type_of(m) == EventType::Reconcile &&
                   RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End
               ? 1U
               : 0U;
    return n;
  }
  template <class M>
  const M* find(EventType t, ClientOrderId id) const {
    return oc.first_if<M>(t, [&](const M& m) { return m.cl_ord_id == id; });
  }
  // The last position the reconciliations reported.
  Qty reported_position() const {
    Qty q{};
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) != EventType::Reconcile) continue;
      const auto& r = RecordingSink::as<ReconcileMsg>(m);
      if (r.kind == ReconcileMsg::Kind::Position) q = r.position_qty;
    }
    return q;
  }
};

double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

OutNewOrderMsg limit(std::uint32_t seq, Side side, Price px, Qty qty) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kSpy, kVenue);
  n.cl_ord_id =
      make_cl_ord_id(static_cast<std::uint16_t>(wall_now().ns / 1'000'000'000 % 60000), seq);
  n.side = side;
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Day;
  n.price = px;
  n.qty = qty;
  return n;
}

}  // namespace

TEST_CASE("live.alpaca: paper account order lifecycle, fills, reconnect and restart") {
  const std::string key = env_or_empty("ALPACA_API_KEY");
  const std::string secret = env_or_empty("ALPACA_API_SECRET");
  if (!live_tests_enabled() || key.empty() || secret.empty()) {
    MESSAGE("skipped: FASTMM_LIVE_TESTS=1, ALPACA_API_KEY and ALPACA_API_SECRET");
    return;
  }
  VenueSection s;
  s.name = "alpaca-paper";
  s.kind = "alpaca";
  s.ws_url = "wss://stream.data.alpaca.markets/v2/iex";
  s.rest_url = "https://paper-api.alpaca.markets";
  s.api_key = key;
  s.api_secret = secret;
  s.extra["ws_private_url"] = "wss://paper-api.alpaca.markets/stream";
  s.extra["cancel_on_order_channel_loss"] = "false";

  // The last IEX trade, for the prices.
  BlockingHttpOptions opts;
  opts.timeout_ms = 10'000;
  BlockingHttp data("https://data.alpaca.markets", opts);
  const std::string hdr = "APCA-API-KEY-ID: " + key + "\r\nAPCA-API-SECRET-KEY: " + secret + "\r\n";
  const HttpReply last = data.get("/v2/stocks/SPY/trades/latest?feed=iex", hdr);
  REQUIRE_MESSAGE(last.status == 200, last.error);
  const std::size_t at = last.body.find("\"p\":");
  REQUIRE(at != std::string::npos);
  const double px = std::strtod(last.body.c_str() + at + 4, nullptr);
  REQUIRE(px > 1.0);
  const auto cents = [](double v) {
    return Price::from_raw(static_cast<std::int64_t>(v * 100) * 1'000'000);
  };
  MESSAGE("SPY last IEX trade " << px);

  Session a(s);
  auto t = std::chrono::steady_clock::now();
  REQUIRE(a.wait([&] { return a.reconciles() >= 1 && a.venue->order_channel_live(); }, 20'000));
  MESSAGE("trading stream live and swept in " << ms_since(t) << " ms; position "
                                              << a.reported_position().to_double());
  const Qty start_pos = a.reported_position();
  if (a.wait([&] { return a.mc.count(EventType::BookSnapshot) > 0; }, 15'000))
    MESSAGE("first quote after " << ms_since(t) << " ms");
  else
    MESSAGE("no quote in 15 s (IEX quotes from 08:00 New York time)");

  // Far buy: placed, replaced, cancelled.
  const OutNewOrderMsg far = limit(1, Side::Buy, cents(px * 0.8), Qty::from_int(1));
  t = std::chrono::steady_clock::now();
  a.send(far.hdr);
  REQUIRE(a.wait([&] { return a.find<OrderAckMsg>(EventType::OrderAck, far.cl_ord_id); }, 10'000));
  MESSAGE("new -> ack " << ms_since(t) << " ms");
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, kSpy, kVenue);
  r.cl_ord_id = ClientOrderId{far.cl_ord_id.value + 1};
  r.orig_cl_ord_id = far.cl_ord_id;
  r.price = cents(px * 0.79);
  r.qty = Qty::from_int(2);
  t = std::chrono::steady_clock::now();
  a.send(r.hdr);
  REQUIRE(a.wait([&] { return a.find<OrderAckMsg>(EventType::OrderAck, r.cl_ord_id); }, 10'000));
  MESSAGE("replace -> ack " << ms_since(t) << " ms");
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kSpy, kVenue);
  c.cl_ord_id = r.cl_ord_id;
  t = std::chrono::steady_clock::now();
  a.send(c.hdr);
  REQUIRE(a.wait([&] { return a.find<OrderCancelAckMsg>(EventType::OrderCancelAck, r.cl_ord_id); },
                 10'000));
  MESSAGE("cancel -> cancel ack (trade_updates) " << ms_since(t) << " ms");

  // A marketable buy and sell of one share.
  const OutNewOrderMsg buy = limit(3, Side::Buy, cents(px * 1.01), Qty::from_int(1));
  t = std::chrono::steady_clock::now();
  a.send(buy.hdr);
  REQUIRE(
      a.wait([&] { return a.find<OrderFillMsg>(EventType::OrderFill, buy.cl_ord_id); }, 20'000));
  MESSAGE("buy -> fill "
          << ms_since(t) << " ms at "
          << a.find<OrderFillMsg>(EventType::OrderFill, buy.cl_ord_id)->price.to_double());
  const OutNewOrderMsg sell = limit(4, Side::Sell, cents(px * 0.99), Qty::from_int(1));
  t = std::chrono::steady_clock::now();
  a.send(sell.hdr);
  REQUIRE(
      a.wait([&] { return a.find<OrderFillMsg>(EventType::OrderFill, sell.cl_ord_id); }, 20'000));
  MESSAGE("sell -> fill " << ms_since(t) << " ms");

  // Reconnect: the reconciliation replays the two fills (booked already: the same ids) and
  // reports the position the session started with.
  const std::size_t before = a.reconciles();
  a.venue->disconnect();
  a.reactor.run_once(0);
  t = std::chrono::steady_clock::now();
  a.venue->connect(a.reactor);
  REQUIRE(a.wait([&] { return a.reconciles() > before && a.venue->order_channel_live(); }, 20'000));
  MESSAGE("reconnect -> reconciled " << ms_since(t) << " ms");
  CHECK(a.reported_position() == start_pos);

  // Restart: a resting order left by this connector is found by a new one, with its client id.
  const OutNewOrderMsg rest = limit(5, Side::Buy, cents(px * 0.8), Qty::from_int(1));
  a.send(rest.hdr);
  REQUIRE(a.wait([&] { return a.find<OrderAckMsg>(EventType::OrderAck, rest.cl_ord_id); }, 10'000));
  a.venue->disconnect();
  a.venue.reset();
  t = std::chrono::steady_clock::now();
  Session b(s);
  REQUIRE(b.wait([&] { return b.reconciles() >= 1 && b.venue->order_channel_live(); }, 20'000));
  MESSAGE("restart -> swept " << ms_since(t) << " ms");
  CHECK(b.oc.first_if<ReconcileMsg>(EventType::Reconcile, [&](const ReconcileMsg& m) {
    return m.kind == ReconcileMsg::Kind::OpenOrder && m.cl_ord_id == rest.cl_ord_id;
  }) != nullptr);
  CHECK(b.reported_position() == start_pos);
  t = std::chrono::steady_clock::now();
  CHECK(b.venue->cancel_all());
  MESSAGE("kill-switch cancel-all " << ms_since(t) << " ms");
}
