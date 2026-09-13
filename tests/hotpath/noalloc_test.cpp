// Proves the hot paths do not allocate after construction/warm-up (5.2). This binary is the
// only one linking the counting operator new (tests/support/alloc_counter.cpp).
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/book/l3_book.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>
#include <vector>

using namespace fastmm;
using fastmm::test::NoAllocScope;

namespace {
template <class T>
void benchmark_sink(const T& v) {
  asm volatile("" : : "g"(&v) : "memory");
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Instrument make_inst() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.symbol = "BTCUSDT";
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  i.min_notional = Notional::from_decimal("0.5").value();
  return i;
}

struct NullTransport {
  std::size_t sent = 0;
  bool send(const EventHeader&) noexcept {
    ++sent;
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> b) noexcept {
    sent += b.size();
    return b.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
};
}  // namespace

TEST_CASE("hotpath.noalloc: L2 apply_level / apply_delta") {
  auto book = std::make_unique<L2Book<256>>();
  std::vector<std::byte> buf(BookDeltaMsg::size_for(20, 20));
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf.data());
  init_header(*d,
              EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              static_cast<std::uint32_t>(buf.size()));
  d->bid_count = d->ask_count = 20;
  for (std::uint32_t i = 0; i < 20; ++i) {
    d->levels()[i] = Level{Price::from_raw(px("100").raw - i * px("0.01").raw), qt("1")};
    d->levels()[20 + i] = Level{Price::from_raw(px("100.01").raw + i * px("0.01").raw), qt("1")};
  }
  {
    NoAllocScope guard(true);
    book->apply_delta(*d);
    for (int i = 0; i < 1000; ++i) book->apply_level(Side::Buy, px("99.995"), Qty::from_int(i % 3));
    benchmark_sink(book->price_for_qty(Side::Buy, qt("5")));
  }
}

TEST_CASE("hotpath.noalloc: L3 add/execute/cancel/replace") {
  auto book = std::make_unique<L3Book<1U << 12, 1U << 12>>(px("0.01"));
  {
    NoAllocScope guard(true);
    for (std::uint64_t i = 1; i <= 1000; ++i)
      book->add(i, (i & 1) ? Side::Buy : Side::Sell, px((i & 1) ? "100" : "101"), qt("1"));
    for (std::uint64_t i = 1; i <= 500; ++i) book->execute(i, qt("1"));
    for (std::uint64_t i = 501; i <= 800; ++i) book->cancel(i);
    for (std::uint64_t i = 801; i <= 1000; ++i) book->replace(i, i + 10000, px("102"), qt("2"));
    benchmark_sink(book->best_bid());
  }
}

TEST_CASE("hotpath.noalloc: risk check, OMS lifecycle, quote manager, journal, log") {
  const Instrument inst = make_inst();
  RiskLimits l;
  l.max_order_qty = qt("10");
  l.max_position = qt("100");
  l.max_open_orders = 64;
  l.price_collar_bps = 100;
  l.stale_md = seconds(1);
  l.orders_per_sec = 1'000'000;
  const Timestamp now{seconds(1).ns};
  auto risk = std::make_unique<RiskEngine>(l, now);
  risk->on_book(inst.id, px("100"), now);
  auto oms = std::make_unique<Oms>();
  oms->warm_up();
  QuoteParams qp;
  qp.min_requote_interval = Duration{};
  auto qm = std::make_unique<QuoteManager>(qp);
  MsgRing ring(1 << 20);
  JournalWriter journal(&ring);
  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().attach_current_thread();
  Position pos{};
  {
    NoAllocScope guard(true);
    // risk
    OrderIntent oi{inst.id, VenueId{0}, Side::Buy, OrderType::Limit, px("99.5"), qt("0.01")};
    RiskInputs in{now, &pos, Qty{}, 0, Price{}};
    CHECK(risk->check_new(oi, inst, in) == RejectReason::None);
    // OMS submit -> ack -> partial fill -> cancel -> cancel ack
    NewOrderRequest r{};
    r.instrument = inst.id;
    r.side = Side::Buy;
    r.price = px("99.5");
    r.qty = qt("0.01");
    const ClientOrderId id = oms->next_cl_ord_id();
    auto h = oms->submit(r, id, now);
    CHECK(h);
    OrderAckMsg a{};
    init_header(a, EventType::OrderAck);
    a.cl_ord_id = id;
    a.venue_order_id = "V1";
    CHECK(oms->on_ack(a).changed);
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill);
    f.cl_ord_id = id;
    f.qty = qt("0.004");
    f.cum_qty = qt("0.004");
    f.exec_id = "e1";
    CHECK(oms->on_fill(f).changed);
    CHECK(oms->request_cancel(*h));
    OrderCancelAckMsg c{};
    init_header(c, EventType::OrderCancelAck);
    c.cl_ord_id = id;
    CHECK(oms->on_cancel_ack(c).terminal);
    // quote manager reconcile with a placer that submits to the OMS
    DesiredQuotes dq;
    dq.bids.push_back({px("99.9"), qt("0.01")});
    dq.asks.push_back({px("100.1"), qt("0.01")});
    auto place = [&](QuoteAction& act) noexcept {
      if (act.kind == QuoteActionKind::New) {
        NewOrderRequest nr{};
        nr.instrument = act.instrument;
        nr.side = act.side;
        nr.price = act.price;
        nr.qty = act.qty;
        nr.user_tag = QuoteManager::make_tag(act.side, act.level);
        act.cl_ord_id = oms->next_cl_ord_id();
        auto hh = oms->submit(nr, act.cl_ord_id, now);
        if (!hh) return false;
        act.handle = *hh;
        return true;
      }
      return oms->request_cancel(act.handle).has_value();
    };
    CHECK(qm->reconcile(inst, dq, *oms, now, place) == 2);
    CHECK(qm->reconcile(inst, dq, *oms, now, place) == 0);
    // journal enqueue
    CHECK(journal.record(a.hdr));
    CHECK(journal.record(f.hdr));
    // log emit (formatting happens on the sink thread, not here)
    FASTMM_LOG_INFO("noalloc {} {} {}", id.value, px("1.5"), Side::Buy);
    FASTMM_LOG_WARN("noalloc warn {}", std::string_view("sv"));
  }
  const std::byte* p = ring.try_peek();
  CHECK(p != nullptr);
}

TEST_CASE("hotpath.noalloc: engine step on BookDelta with BasicMM") {
  InstrumentTable table;
  REQUIRE(table.add(make_inst()));
  SimClock clock{Timestamp{seconds(1000).ns}};
  NullTransport transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure(
      {{"half_spread_bps", "10"}, {"quote_qty", "0.01"}, {"max_inventory", "0.05"}}));
  EngineConfig cfg;
  cfg.risk.max_order_qty = qt("1");
  cfg.risk.max_position = qt("1");
  cfg.risk.max_open_orders = 8;
  cfg.quotes.min_requote_interval = Duration{};
  using E = Engine<BasicMM, SimClock, NullTransport, InlineFeed>;
  auto engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, &journal_ring);
  engine->warm_up();
  engine->start();
  auto push_book = [&](const char* bid, const char* ask, std::uint64_t seq, bool snap) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d,
                snap ? EventType::BookSnapshot : EventType::BookDelta,
                InstrumentId{0},
                VenueId{0},
                BookDeltaMsg::size_for(1, 1));
    if (snap) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->hdr.t0_cycles = clock.cycles();
    d->bid_count = d->ask_count = 1;
    d->last_update_id = seq;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
  };
  push_book("100.00", "100.02", 1, true);
  push_book("100.10", "100.12", 2, false);
  push_book("100.20", "100.22", 3, false);
  {
    NoAllocScope guard(true);
    std::size_t n = 0;
    while (engine->step() > 0) ++n;
    clock.advance(seconds(2));  // fire the stale timer
    engine->step();
    CHECK(n > 0);
  }
  CHECK(engine->stats().book_updates == 3);
  CHECK(transport.sent >= 2);
  CHECK(engine->stats().timers_fired >= 1);
}
