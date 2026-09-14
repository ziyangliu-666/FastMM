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
    qm.on_order_update(u, inst, h.oms, now, h);
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
    qm.on_order_update(h.oms.on_cancel_ack(m), inst, h.oms, now, h);
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

namespace {
std::vector<ClientOrderId> ids_in_state(const Oms& oms, OrderState state, bool pending_id = false) {
  std::vector<ClientOrderId> ids;
  oms.for_each_open_order([&](Handle<Order>, const Order& o) {
    if (o.state == state) ids.push_back(pending_id ? o.pending_cl_ord_id : o.cl_ord_id);
  });
  return ids;
}
OrderAckMsg ack_msg(ClientOrderId id) {
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck);
  m.cl_ord_id = id;
  m.venue_order_id = "v";
  return m;
}
OrderCancelAckMsg cancel_ack_msg(ClientOrderId id) {
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck);
  m.cl_ord_id = id;
  return m;
}
}  // namespace

TEST_CASE("core.quote_manager: pulling a replaced quote frees its slot for the next requote") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_interval = Duration{};
  p.supports_replace = true;
  QuoteManager qm(p);
  Harness h;
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
  h.ack_all();
  REQUIRE(qm.reconcile(inst, quotes("98.00", "102.00"), h.oms, Timestamp{1}, h) == 2);
  REQUIRE(h.count(QuoteActionKind::Replace) == 2);
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingReplace, true))
    qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, Timestamp{1}, h);
  qm.pull_quotes(inst, h.oms, h);  // the connection dropped
  REQUIRE(h.count(QuoteActionKind::Cancel) == 2);
  // The cancel acks carry the replacement ids, not the ids the slots were created with.
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingCancel))
    qm.on_order_update(h.oms.on_cancel_ack(cancel_ack_msg(id)), inst, h.oms, Timestamp{2}, h);
  CHECK(h.oms.open_count() == 0);
  // Back online: both levels are quoted again (the slots used to wait for a terminal update
  // forever and never place another order).
  CHECK(qm.reconcile(inst, quotes("98.50", "101.50"), h.oms, Timestamp{3}, h) == 2);
  CHECK(h.count(QuoteActionKind::New) == 4);
  CHECK(h.oms.open_count() == 2);
}

TEST_CASE("core.quote_manager: an order that becomes working after a pull is cancelled") {
  const Instrument inst = make_inst();
  QuoteManager qm;
  Harness h;
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);  // both PendingNew
  qm.pull_quotes(inst, h.oms, h);
  CHECK(h.count(QuoteActionKind::Cancel) == 0);  // pending orders cannot be cancelled yet
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
    qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, Timestamp{}, h);
  CHECK(h.count(QuoteActionKind::Cancel) == 2);
  CHECK(ids_in_state(h.oms, OrderState::PendingCancel).size() == 2);
  // A requote after the pull quotes normally again.
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingCancel))
    qm.on_order_update(h.oms.on_cancel_ack(cancel_ack_msg(id)), inst, h.oms, Timestamp{}, h);
  CHECK(qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{1}, h) == 2);
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
    qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, Timestamp{1}, h);
  CHECK(h.count(QuoteActionKind::Cancel) == 2);
  CHECK(h.oms.open_count() == 2);
}

TEST_CASE("core.quote_manager: an order that reuses a stale slot handle is not adopted") {
  const Instrument inst = make_inst();
  QuoteManager qm;
  Harness h;
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
  h.ack_all();
  const Handle<Order> bid = qm.slot_handle(inst.id, Side::Buy, 0);
  REQUIRE(bid.valid());
  // The bid is cancelled at the venue without the manager hearing about it (the handle goes
  // stale), and an unrelated order takes over the freed order slot.
  REQUIRE(h.oms.request_cancel(bid).has_value());
  const ClientOrderId bid_id = h.oms.get(bid).cl_ord_id;
  static_cast<void>(h.oms.on_cancel_ack(cancel_ack_msg(bid_id)));
  NewOrderRequest r{};
  r.instrument = inst.id;
  r.side = Side::Buy;
  r.price = px("50.00");
  r.qty = qt("1");
  const Handle<Order> manual = *h.oms.submit(r, h.oms.next_cl_ord_id(), {});
  REQUIRE(manual.idx == bid.idx);
  h.actions.clear();
  CHECK(qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{1}, h) == 1);
  REQUIRE(h.actions.size() == 1);
  CHECK(h.actions[0].kind == QuoteActionKind::New);  // a new bid, not a replace of the manual order
  CHECK(h.oms.get(manual).state == OrderState::PendingNew);
  CHECK(h.oms.get(manual).price == px("50.00"));
}

TEST_CASE("core.quote_manager: a venue reject backs off new orders on that side") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_interval = Duration{};
  p.reject_backoff = milliseconds(1000);
  p.reject_backoff_max = milliseconds(3000);
  QuoteManager qm(p);
  Harness h;
  const auto reject_ask = [&](RejectReason reason, Timestamp now) {
    for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew)) {
      if (h.oms.get(h.oms.find(id)).side != Side::Sell) continue;
      OrderRejectMsg m{};
      init_header(m, EventType::OrderReject);
      m.cl_ord_id = id;
      m.reason = reason;
      qm.on_order_update(h.oms.on_reject(m), inst, h.oms, now, h);
    }
  };
  const auto news = [&](Side side) {
    std::size_t n = 0;
    for (const auto& a : h.actions) n += (a.kind == QuoteActionKind::New && a.side == side) ? 1 : 0;
    return n;
  };
  const Timestamp t0 = Timestamp{} + milliseconds(10'000);
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0, h);
  reject_ask(RejectReason::InsufficientBalance, t0);  // e.g. no base asset to sell
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
    qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, t0, h);  // the bid is accepted
  REQUIRE(news(Side::Sell) == 1);

  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(500), h);
  CHECK(news(Side::Sell) == 1);  // within the first backoff
  CHECK(qm.stats().kept_backoff == 1);
  CHECK(news(Side::Buy) == 1);  // the bid side is unaffected and still resting

  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(1100), h);
  CHECK(news(Side::Sell) == 2);  // retried after 1 s
  reject_ask(RejectReason::InsufficientBalance, t0 + milliseconds(1100));
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(2600), h);
  CHECK(news(Side::Sell) == 2);  // second reject doubled the backoff to 2 s
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(3200), h);
  CHECK(news(Side::Sell) == 3);
  reject_ask(RejectReason::VenueReject, t0 + milliseconds(3200));
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(6100), h);
  CHECK(news(Side::Sell) == 3);  // capped at 3 s, not 4 s
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(6300), h);
  CHECK(news(Side::Sell) == 4);
  CHECK(qm.stats().reject_backoffs == 3);

  // An accepted ask resets the backoff; a post-only cross does not start one.
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
    qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, t0 + milliseconds(6300), h);
  qm.pull_quotes(inst, h.oms, h);
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingCancel))
    qm.on_order_update(
        h.oms.on_cancel_ack(cancel_ack_msg(id)), inst, h.oms, t0 + milliseconds(6400), h);
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(6500), h);
  CHECK(news(Side::Sell) == 5);
  reject_ask(RejectReason::PostOnlyWouldCross, t0 + milliseconds(6500));
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, t0 + milliseconds(6600), h);
  CHECK(news(Side::Sell) == 6);
  CHECK(qm.stats().reject_backoffs == 3);
}

TEST_CASE("core.quote_manager: a requote that meets pending orders is applied when they resolve") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_interval = Duration{};
  p.supports_replace = false;
  QuoteManager qm(p);
  Harness h;
  const auto cancel_acks = [&](Timestamp now) {
    for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingCancel))
      qm.on_order_update(h.oms.on_cancel_ack(cancel_ack_msg(id)), inst, h.oms, now, h);
  };

  SUBCASE("the terminal update places the recorded target") {
    qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
    h.ack_all();
    qm.reconcile(inst, DesiredQuotes{}, h.oms, Timestamp{1}, h);  // cancels both
    REQUIRE(h.count(QuoteActionKind::Cancel) == 2);
    CHECK(qm.reconcile(inst, quotes("98.00", "102.00"), h.oms, Timestamp{2}, h) == 0);
    CHECK(qm.stats().skipped_pending == 2);
    cancel_acks(Timestamp{3});
    REQUIRE(h.count(QuoteActionKind::New) == 4);
    CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Buy, 0)).price == px("98.00"));
    CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Sell, 0)).price == px("102.00"));
  }
  SUBCASE("a pull in between drops the target") {
    qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
    h.ack_all();
    qm.reconcile(inst, DesiredQuotes{}, h.oms, Timestamp{1}, h);
    CHECK(qm.reconcile(inst, quotes("98.00", "102.00"), h.oms, Timestamp{2}, h) == 0);
    qm.pull_quotes(inst, h.oms, h);
    cancel_acks(Timestamp{3});
    CHECK(h.count(QuoteActionKind::New) == 2);
    CHECK(h.oms.open_count() == 0);
  }
  SUBCASE("the ack of a pending new applies the target") {
    qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);  // both PendingNew
    CHECK(qm.reconcile(inst, quotes("98.00", "102.00"), h.oms, Timestamp{1}, h) == 0);
    for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
      qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, Timestamp{2}, h);
    CHECK(h.count(QuoteActionKind::Cancel) == 2);  // cancel-then-new toward the recorded target
    cancel_acks(Timestamp{3});
    REQUIRE(h.count(QuoteActionKind::New) == 4);
    CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Buy, 0)).price == px("98.00"));
    CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Sell, 0)).price == px("102.00"));
  }
  SUBCASE("an ack that meets the target within hysteresis keeps the order") {
    qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
    CHECK(qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{1}, h) == 0);
    for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingNew))
      qm.on_order_update(h.oms.on_ack(ack_msg(id)), inst, h.oms, Timestamp{2}, h);
    CHECK(h.count(QuoteActionKind::Cancel) == 0);
    CHECK(h.count(QuoteActionKind::New) == 2);
    CHECK(qm.stats().kept_hysteresis == 2);
  }
}

TEST_CASE("core.quote_manager: resume re-applies quotes paused with keep_desired only") {
  const Instrument inst = make_inst();
  QuoteParams p;
  p.min_requote_interval = Duration{};
  QuoteManager qm(p);
  Harness h;
  qm.reconcile(inst, quotes("99.00", "101.00"), h.oms, Timestamp{}, h);
  h.ack_all();
  qm.pull_quotes(inst, h.oms, h, /*keep_desired=*/true);
  CHECK(qm.pulled(inst.id));
  CHECK(qm.resumable(inst.id));
  REQUIRE(h.count(QuoteActionKind::Cancel) == 2);
  for (const ClientOrderId id : ids_in_state(h.oms, OrderState::PendingCancel))
    qm.on_order_update(h.oms.on_cancel_ack(cancel_ack_msg(id)), inst, h.oms, Timestamp{1}, h);
  CHECK(h.count(QuoteActionKind::New) == 2);  // still paused
  CHECK(qm.resume(inst, h.oms, Timestamp{2}, h) == 2);
  CHECK_FALSE(qm.pulled(inst.id));
  CHECK_FALSE(qm.resumable(inst.id));
  CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Buy, 0)).price == px("99.00"));
  CHECK(h.oms.get(qm.slot_handle(inst.id, Side::Sell, 0)).price == px("101.00"));
  CHECK(qm.resume(inst, h.oms, Timestamp{3}, h) == 0);  // once
  h.ack_all();
  // A pull that stops quoting forgets the quotes, and a pause on top of it has nothing to resume.
  qm.pull_quotes(inst, h.oms, h);
  qm.pull_quotes(inst, h.oms, h, /*keep_desired=*/true);
  CHECK_FALSE(qm.resumable(inst.id));
  CHECK(qm.resume(inst, h.oms, Timestamp{4}, h) == 0);
}
