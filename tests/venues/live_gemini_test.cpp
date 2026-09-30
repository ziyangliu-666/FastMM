// Gemini sandbox (opt-in: FASTMM_LIVE_TESTS=1). Public part, no keys: btcusd reference data and
// the book. With GEMINI_SANDBOX_API_KEY / GEMINI_SANDBOX_API_SECRET (a sandbox key for the exchange
// account): a post-only buy far below the market is placed and cancelled, a small IOC buy fills,
// then a second connector (a restart) sweeps and replays the trades, and the sandbox's own trade
// history and open orders are checked against what was booked. The test refuses any host but
// *.sandbox.gemini.com, so production can never be reached from here.
#include "live_test_util.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/gemini/gemini_rest_decoder.hpp"
#include "fastmm/venues/gemini/gemini_venue.hpp"

#include <set>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gemini;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kWs = "wss://ws.sandbox.gemini.com";
constexpr const char* kRest = "https://api.sandbox.gemini.com";

VenueSection sandbox_section(const std::string& key, const std::string& secret) {
  VenueSection s;
  s.name = "gemini-sandbox";
  s.kind = "gemini";
  s.ws_url = kWs;
  s.rest_url = kRest;
  s.testnet = true;
  s.api_key = key;
  s.api_secret = secret;
  return s;
}

struct Session {
  InstrumentTable instruments;
  RecordingSink md{16U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<GeminiVenue> venue;
  Collected oc;
  Collected mdc;

  Session(const VenueSection& s, bool dry_run, std::int64_t resume_from_ms = 0) {
    Instrument spot = make_instrument("btcusd", 0, "BTC", "USD");
    spot.min_notional = Notional{};
    REQUIRE(instruments.add(spot));
    GeminiVenueConfig cfg = make_gemini_config(s, dry_run);
    cfg.cancel_on_order_channel_loss = false;
    venue = std::make_unique<GeminiVenue>(VenueId{0}, cfg);
    const auto ref = venue->load_reference_data(instruments);
    REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue->subscribe(ids);
    if (resume_from_ms > 0) venue->resume_executions(resume_from_ms, {});
    venue->connect(reactor);
  }
  ~Session() {
    venue->disconnect();
    reactor.run_once(0);
  }
  std::size_t ends() {
    oc.take(orders);
    std::size_t n = 0;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End)
        ++n;
    }
    return n;
  }
  template <class Pred>
  bool until(Pred pred, int timeout_ms = 15'000) {
    return pump_until(
        reactor,
        [&] {
          oc.take(orders);
          mdc.take(md);
          return pred();
        },
        timeout_ms);
  }
  void push(const EventHeader& h) {
    REQUIRE(outbound.try_push(&h, h.len));
    venue->on_wake();
  }
  [[nodiscard]] const BookTickerMsg* ticker() const {
    return mdc.last<BookTickerMsg>(EventType::BookTicker);
  }
};

}  // namespace

TEST_CASE("live.gemini: sandbox btcusd book, then place, cancel, an IOC fill and a restart") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Gemini sandbox");
    return;
  }
  REQUIRE(std::string_view(kWs).find("sandbox.gemini.com") != std::string_view::npos);
  REQUIRE(std::string_view(kRest).find("sandbox.gemini.com") != std::string_view::npos);
  const std::string key = env_or_empty("GEMINI_SANDBOX_API_KEY");
  const std::string secret = env_or_empty("GEMINI_SANDBOX_API_SECRET");
  const bool with_keys = !key.empty() && !secret.empty();
  const std::int64_t start_ms = wall_now().ns / 1'000'000;

  std::vector<std::string> exec_ids;  // the fills the first session booked
  std::set<std::string> client_ids;
  {
    Session a(sandbox_section(key, secret), !with_keys);
    REQUIRE(a.until(
        [&] { return a.venue->md_feed()->synced_count() == 1 && a.ticker() != nullptr; }, 30'000));
    const Instrument& inst = a.instruments.get(InstrumentId{0});
    CHECK(inst.tick == Price::from_decimal("0.01").value());
    CHECK(inst.lot == Qty::from_decimal("0.00000001").value());
    if (!with_keys) {
      MESSAGE("skipped order entry: GEMINI_SANDBOX_API_KEY / GEMINI_SANDBOX_API_SECRET not set");
      return;
    }
    REQUIRE(a.until([&] { return a.ends() >= 1 && a.venue->order_channel_live(); }, 30'000));
    const std::uint16_t epoch =
        static_cast<std::uint16_t>(1 + wall_now().ns / 1'000'000'000 % 60000);

    // A post-only buy at 80 % of the bid, then its cancel.
    OutNewOrderMsg n = far_passive_buy(inst, a.ticker()->bid_px, make_cl_ord_id(epoch, 1));
    n.qty = Qty::from_decimal("0.0002").value();
    client_ids.insert(std::string(encode_cl_ord_id(n.cl_ord_id).view()));
    a.push(n.hdr);
    REQUIRE(a.until([&] {
      return a.oc.count(EventType::OrderAck) > 0 || a.oc.count(EventType::OrderReject) > 0;
    }));
    REQUIRE_MESSAGE(
        a.oc.count(EventType::OrderReject) == 0,
        (a.oc.count(EventType::OrderReject) != 0
             ? std::string(a.oc.last<OrderRejectMsg>(EventType::OrderReject)->text.view())
             : std::string()));
    const VenueOrderId venue_id = a.oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id;
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, inst.id, inst.venue);
    c.cl_ord_id = n.cl_ord_id;
    c.venue_order_id = venue_id;
    a.push(c.hdr);
    REQUIRE(a.until([&] {
      return a.oc.count(EventType::OrderCancelAck) > 0 ||
             a.oc.count(EventType::OrderCancelReject) > 0;
    }));
    CHECK(a.oc.count(EventType::OrderCancelAck) == 1);
    CHECK(a.oc.count(EventType::OrderCancelReject) == 0);

    // A small IOC buy above the ask: it fills and the rest (if any) ends as expired.
    OutNewOrderMsg ioc{};
    init_header(ioc, EventType::OutNewOrder, inst.id, inst.venue);
    ioc.cl_ord_id = make_cl_ord_id(epoch, 2);
    ioc.side = Side::Buy;
    ioc.type = OrderType::Limit;
    ioc.tif = TimeInForce::Ioc;
    ioc.price = inst.round_price(Price::from_raw(a.ticker()->ask_px.raw / 1000 * 1002), Side::Buy);
    ioc.qty = Qty::from_decimal("0.0001").value();
    client_ids.insert(std::string(encode_cl_ord_id(ioc.cl_ord_id).view()));
    a.push(ioc.hdr);
    REQUIRE(a.until([&] {
      return a.oc.count(EventType::OrderExpired) + a.oc.count(EventType::OrderCancelAck) >= 2 ||
             a.oc.count(EventType::OrderReject) > 0;
    }));
    CHECK(a.oc.count(EventType::OrderReject) == 0);
    Qty filled{};
    for (const auto* f : [&] {
           std::vector<const OrderFillMsg*> out;
           for (const auto& m : a.oc.all) {
             if (RecordingSink::type_of(m) == EventType::OrderFill)
               out.push_back(&RecordingSink::as<OrderFillMsg>(m));
           }
           return out;
         }()) {
      CHECK(f->cl_ord_id == ioc.cl_ord_id);
      exec_ids.emplace_back(f->exec_id.view());
      filled = filled + f->qty;
    }
    CHECK(filled == ioc.qty);
    CHECK(a.venue->shadow_count() == 0);
    CHECK_FALSE(a.venue->fatal());
    MESSAGE("IOC filled in " << exec_ids.size() << " execution(s)");
  }
  if (!with_keys) return;

  // The sandbox's own record, per order (POST /v1/order/status with include_trades): every
  // execution booked once, nothing left open. Its /v1/mytrades listed no trade at all on
  // 2026-09-30, not even hours later, so the replay is compared with what mytrades returns.
  BlockingHttp http(kRest);
  Signer signer(Credentials{key, Secret<std::string>{secret}});
  std::int64_t nonce = wall_now().ns / 1'000'000 + 10'000;
  std::multiset<std::string> venue_tids;
  for (const std::string& cl : client_ids) {
    const std::string payload = R"({"request":"/v1/order/status","nonce":)" +
                                std::to_string(++nonce) + R"(,"client_order_id":")" + cl +
                                R"(","include_trades":true})";
    const HttpReply r = http.request("POST", "/v1/order/status", signer.rest_headers(payload));
    REQUIRE_MESSAGE(r.ok(), r.status << " " << r.body.substr(0, 200));
    CHECK(r.body.find(R"("is_live":true)") == std::string::npos);
    for (std::size_t at = r.body.find(R"("tid":)"); at != std::string::npos;
         at = r.body.find(R"("tid":)", at + 1)) {
      const std::size_t b = at + 6;
      venue_tids.insert(r.body.substr(b, r.body.find_first_not_of("0123456789", b) - b));
    }
  }
  CHECK(venue_tids == std::multiset<std::string>(exec_ids.begin(), exec_ids.end()));
  std::size_t listed = 0;
  {
    const std::string payload = R"({"request":"/v1/mytrades","nonce":)" + std::to_string(++nonce) +
                                R"(,"symbol":"btcusd","timestamp":)" +
                                std::to_string(start_ms - 1000) + R"(,"limit_trades":500})";
    const HttpReply t = http.request("POST", "/v1/mytrades", signer.rest_headers(payload));
    REQUIRE(t.ok());
    std::vector<TradeRow> trades;
    REQUIRE(decode_trades(t.body, trades).empty());
    for (const TradeRow& r : trades) listed += client_ids.contains(r.client_order_id) ? 1U : 0U;
    MESSAGE("mytrades lists " << listed << " of the " << exec_ids.size() << " execution(s)");
  }

  // A restart: the sweep finds nothing open, and the replay from before the first session books
  // what mytrades lists, with the same ids.
  {
    Session b(sandbox_section(key, secret), false, start_ms - 1000);
    REQUIRE(b.until([&] { return b.ends() >= 1 && b.venue->order_channel_live(); }, 30'000));
    std::size_t replayed = 0;
    for (const auto& m : b.oc.all) {
      if (RecordingSink::type_of(m) == EventType::OrderFill) {
        const auto& f = RecordingSink::as<OrderFillMsg>(m);
        CHECK((f.flags & OrderFillMsg::kReplayed) != 0);
        CHECK(venue_tids.contains(std::string(f.exec_id.view())));
        ++replayed;
      }
      if (RecordingSink::type_of(m) == EventType::Reconcile)
        CHECK(RecordingSink::as<ReconcileMsg>(m).kind != ReconcileMsg::Kind::OpenOrder);
    }
    CHECK(replayed == listed);
  }
}
