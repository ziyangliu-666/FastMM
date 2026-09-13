#include "fastmm/core/quote_manager.hpp"

#include "test_support.hpp"

#include <vector>

using namespace fastmm;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

Instrument make_inst() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  return i;
}

// Placer that records actions and drives the OMS like the engine would.
struct Harness {
  Oms oms;
  std::vector<QuoteAction> actions;
  bool reject_new = false;
  bool operator()(QuoteAction& a) {
    actions.push_back(a);
    switch (a.kind) {
      case QuoteActionKind::New: {
        if (reject_new) return false;
        NewOrderRequest r{};
        r.instrument = a.instrument;
        r.side = a.side;
        r.price = a.price;
        r.qty = a.qty;
        r.post_only = a.post_only;
        r.user_tag = QuoteManager::make_tag(a.side, a.level);
        const ClientOrderId id = oms.next_cl_ord_id();
        auto h = oms.submit(r, id, {});
        if (!h) return false;
        a.handle = *h;
        a.cl_ord_id = id;
        return true;
      }
      case QuoteActionKind::Cancel:
        return oms.request_cancel(a.handle).has_value();
      case QuoteActionKind::Replace:
        return oms.request_replace(a.handle, oms.next_cl_ord_id(), a.price, a.qty).has_value();
    }
    return false;
  }
  void ack_all() {
    std::vector<ClientOrderId> ids;
    oms.for_each_open_order([&](Handle<Order>, const Order& o) {
      if (o.state == OrderState::PendingNew) ids.push_back(o.cl_ord_id);
    });
    for (auto id : ids) {
      OrderAckMsg m{};
      init_header(m, EventType::OrderAck);
      m.cl_ord_id = id;
      m.venue_order_id = "v";
      oms.on_ack(m);
    }
  }
  std::size_t count(QuoteActionKind k) const {
    std::size_t n = 0;
    for (const auto& a : actions) n += a.kind == k ? 1 : 0;
    return n;
  }
};

DesiredQuotes quotes(const char* bid, const char* ask, const char* q = "1") {
  DesiredQuotes d;
  d.bids.push_back({px(bid), qt(q)});
  d.asks.push_back({px(ask), qt(q)});
  return d;
}
}  // namespace

TEST_CASE("core.quote_manager: place, hysteresis, interval, cancel-then-new sequencing") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_ticks = 2;
  p.min_requote_interval = milliseconds(50);
  p.min_qty_bps = 8000;
  p.supports_replace = false;
  QuoteManager qm(p);
  Harness h;
  Timestamp now{seconds(1).ns};

  CHECK(qm.reconcile(inst, quotes("99.98", "100.02"), h.oms, now, h) == 2);
  CHECK(h.count(QuoteActionKind::New) == 2);
  CHECK(h.oms.open_count() == 2);
  CHECK(qm.slot_handle(inst.id, Side::Buy, 0).valid());
  // pending orders are never touched
  h.actions.clear();
  CHECK(qm.reconcile(inst, quotes("99.90", "100.10"), h.oms, now, h) == 0);
  CHECK(qm.stats().skipped_pending == 2);
  h.ack_all();
  // within hysteresis (1 tick away < 2): keep
  CHECK(qm.reconcile(inst, quotes("99.97", "100.03"), h.oms, now, h) == 0);
  CHECK(qm.stats().kept_hysteresis == 2);
  // beyond hysteresis but inside the interval: keep
  now += milliseconds(10);
  CHECK(qm.reconcile(inst, quotes("99.90", "100.10"), h.oms, now, h) == 0);
  CHECK(qm.stats().kept_interval == 2);
  // interval elapsed: cancel first, new only after the terminal update
  now += milliseconds(50);
  CHECK(qm.reconcile(inst, quotes("99.90", "100.10"), h.oms, now, h) == 2);
  CHECK(h.count(QuoteActionKind::Cancel) == 2);
  CHECK(h.count(QuoteActionKind::New) == 0);  // no new yet
  CHECK(h.oms.open_count() == 2);
  // re-reconcile while cancels are in flight: nothing new (slot awaiting terminal)
  CHECK(qm.reconcile(inst, quotes("99.90", "100.10"), h.oms, now, h) == 0);
  // cancel acks arrive -> new orders placed at the latest desired prices
  std::vector<Order> pending;
  h.oms.for_each_open_order([&](Handle<Order>, const Order& o) { pending.push_back(o); });
  for (const Order& o : pending) {
    OrderCancelAckMsg m{};
    init_header(m, EventType::OrderCancelAck);
    m.cl_ord_id = o.cl_ord_id;
    auto u = h.oms.on_cancel_ack(m);
    qm.on_order_update(u, inst, now, h);
  }
  CHECK(h.count(QuoteActionKind::New) == 2);
  CHECK(h.oms.open_count() == 2);
  CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Buy, 0)).price == px("99.90"));
  CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Sell, 0)).price == px("100.10"));
  h.ack_all();
  // insufficient leaves triggers a requote even at the same price
  {
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill);
    f.cl_ord_id = h.oms.get(qm.slot_handle(inst.id, Side::Buy, 0)).cl_ord_id;
    f.qty = qt("0.5");
    f.cum_qty = qt("0.5");
    f.exec_id = "x";
    h.oms.on_fill(f);
  }
  now += milliseconds(100);
  h.actions.clear();
  CHECK(qm.reconcile(inst, quotes("99.90", "100.10"), h.oms, now, h) == 1);
  CHECK(h.actions[0].kind == QuoteActionKind::Cancel);
  CHECK(h.actions[0].side == Side::Buy);
  // dropping a level cancels it; pull cancels everything and suppresses re-quote
  now += milliseconds(100);
  h.actions.clear();
  DesiredQuotes only_ask;
  only_ask.asks.push_back({px("100.10"), qt("1")});
  CHECK(qm.reconcile(inst, only_ask, h.oms, now, h) == 0);  // bid slot already pending cancel
  CHECK(qm.pull_quotes(inst, h.oms, h) == 1);               // the ask
  CHECK(qm.pulled(inst.id));
  pending.clear();
  h.oms.for_each_open_order([&](Handle<Order>, const Order& o) { pending.push_back(o); });
  for (const Order& o : pending) {
    OrderCancelAckMsg m{};
    init_header(m, EventType::OrderCancelAck);
    m.cl_ord_id = o.cl_ord_id;
    qm.on_order_update(h.oms.on_cancel_ack(m), inst, now, h);
  }
  CHECK(h.oms.open_count() == 0);
  CHECK(h.count(QuoteActionKind::New) == 0);
  // rejected new is counted, slot stays empty
  h.reject_new = true;
  CHECK(qm.reconcile(inst, quotes("99", "101"), h.oms, now, h) == 0);
  CHECK(qm.stats().rejected >= 2);
  CHECK_FALSE(qm.slot_handle(inst.id, Side::Buy, 0).valid());
}

TEST_CASE("core.quote_manager: replace when the venue supports it, multi-level") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_ticks = 1;
  p.min_requote_interval = Duration{};
  p.supports_replace = true;
  QuoteManager qm(p);
  Harness h;
  DesiredQuotes d;
  for (int i = 0; i < 3; ++i) {
    d.bids.push_back({Price::from_raw(px("100").raw - (i + 1) * px("0.01").raw), qt("1")});
    d.asks.push_back({Price::from_raw(px("100").raw + (i + 1) * px("0.01").raw), qt("1")});
  }
  CHECK(qm.reconcile(inst, d, h.oms, {}, h) == 6);
  h.ack_all();
  // shift the whole ladder by one tick: 6 replaces, no cancels
  for (auto& l : d.bids) l.price = Price::from_raw(l.price.raw - px("0.01").raw);
  for (auto& l : d.asks) l.price = Price::from_raw(l.price.raw - px("0.01").raw);
  h.actions.clear();
  CHECK(qm.reconcile(inst, d, h.oms, Timestamp{1}, h) == 6);
  CHECK(h.count(QuoteActionKind::Replace) == 6);
  CHECK(h.count(QuoteActionKind::Cancel) == 0);
  CHECK(qm.stats().replaces == 6);
  // all pending replace now: untouched
  CHECK(qm.reconcile(inst, d, h.oms, Timestamp{2}, h) == 0);
}
