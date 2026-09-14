#include "fastmm/core/oms.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {
Price px(std::int64_t v) {
  return Price::from_int(v);
}
Qty qt(std::int64_t v) {
  return Qty::from_int(v);
}

NewOrderRequest req(Side s, std::int64_t p, std::int64_t q, InstrumentId inst = InstrumentId{0}) {
  NewOrderRequest r{};
  r.instrument = inst;
  r.venue = VenueId{0};
  r.side = s;
  r.price = px(p);
  r.qty = qt(q);
  return r;
}
OrderAckMsg ack(ClientOrderId id, const char* voi = "V1") {
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck);
  m.cl_ord_id = id;
  m.venue_order_id = voi;
  return m;
}
OrderRejectMsg reject(ClientOrderId id, RejectReason r = RejectReason::VenueReject) {
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject);
  m.cl_ord_id = id;
  m.reason = r;
  return m;
}
OrderCancelAckMsg cancel_ack(ClientOrderId id, std::int64_t cum = 0) {
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck);
  m.cl_ord_id = id;
  m.cum_qty = qt(cum);
  return m;
}
OrderCancelRejectMsg cancel_reject(ClientOrderId id, RejectReason r = RejectReason::VenueReject) {
  OrderCancelRejectMsg m{};
  init_header(m, EventType::OrderCancelReject);
  m.cl_ord_id = id;
  m.reason = r;
  return m;
}
OrderFillMsg fill(
    ClientOrderId id, std::int64_t p, std::int64_t q, std::int64_t cum, const char* exec) {
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill);
  m.cl_ord_id = id;
  m.price = px(p);
  m.qty = qt(q);
  m.cum_qty = qt(cum);
  m.exec_id = exec;
  return m;
}
OrderExpiredMsg expired(ClientOrderId id) {
  OrderExpiredMsg m{};
  init_header(m, EventType::OrderExpired);
  m.cl_ord_id = id;
  return m;
}
}  // namespace

TEST_CASE("core.oms: cl_ord_id generation and session epoch store") {
  const auto path = (tmp_dir() / "epoch.txt").string();
  std::filesystem::remove(path);
  const std::uint16_t e1 = SessionEpochStore::next_epoch(path);
  const std::uint16_t e2 = SessionEpochStore::next_epoch(path);
  CHECK(e1 == 1);
  CHECK(e2 == 2);
  Oms oms(e2);
  const ClientOrderId a = oms.next_cl_ord_id();
  const ClientOrderId b = oms.next_cl_ord_id();
  CHECK(cl_ord_id_epoch(a) == 2);
  CHECK(cl_ord_id_seq(a) == 1);
  CHECK(cl_ord_id_seq(b) == 2);
  CHECK(a != b);
  CHECK(encode_cl_ord_id(a).view() == "fm000200000001");
}

TEST_CASE("core.oms: happy path new -> ack -> partial fill -> fill") {
  Oms oms;
  const ClientOrderId id = oms.next_cl_ord_id();
  auto h = oms.submit(req(Side::Buy, 100, 10), id, Timestamp{1});
  REQUIRE(h);
  CHECK(oms.get(*h).state == OrderState::PendingNew);
  CHECK(oms.open_count() == 1);
  CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == qt(10));
  CHECK(oms.best_own_px(InstrumentId{0}, Side::Buy) == px(100));
  CHECK(oms.submit(req(Side::Buy, 100, 1), id, Timestamp{1}).error() == RejectReason::DuplicateId);

  auto u = oms.on_ack(ack(id));
  CHECK(u.changed);
  CHECK(u.prev == OrderState::PendingNew);
  CHECK(u.order.state == OrderState::Live);
  CHECK(u.order.venue_order_id == "V1");
  // duplicate ack ignored
  u = oms.on_ack(ack(id));
  CHECK(u.action == OmsAction::Ignored);
  CHECK_FALSE(u.changed);

  u = oms.on_fill(fill(id, 100, 4, 4, "e1"));
  CHECK(u.order.state == OrderState::PartiallyFilled);
  CHECK(u.order.cum_qty == qt(4));
  CHECK(u.fill_qty == qt(4));
  CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == qt(6));
  // duplicate exec id
  u = oms.on_fill(fill(id, 100, 4, 4, "e1"));
  CHECK(u.action == OmsAction::Duplicate);
  CHECK(oms.stats().duplicates == 1);
  u = oms.on_fill(fill(id, 100, 6, 10, "e2"));
  CHECK(u.terminal);
  CHECK(u.order.state == OrderState::Filled);
  CHECK_FALSE(u.handle.valid());
  CHECK(oms.open_count() == 0);
  CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == Qty{});
  CHECK(oms.best_own_px(InstrumentId{0}, Side::Buy) == Price{});
  CHECK(oms.classify(id) == OrderClass::RecentlyTerminal);
  CHECK(oms.classify(ClientOrderId{999}) == OrderClass::Unknown);
  CHECK(oms.stats().filled == 1);
  CHECK(oms.stats().fills == 2);
}

TEST_CASE("core.oms: transition table and races") {
  Oms oms;
  const InstrumentId inst{0};

  SUBCASE("reject terminates a pending new") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Sell, 100, 1), id, {});
    auto u = oms.on_reject(reject(id, RejectReason::PostOnlyWouldCross));
    CHECK(u.terminal);
    CHECK(u.order.state == OrderState::Rejected);
    CHECK(u.order.reject_reason == RejectReason::PostOnlyWouldCross);
    CHECK_FALSE(oms.is_live(h));
    CHECK(oms.stats().rejected == 1);
  }
  SUBCASE("cancel request only from working states") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Sell, 100, 1), id, {});
    CHECK(oms.request_cancel(h).error() == RejectReason::InvalidState);  // PendingNew
    oms.on_ack(ack(id));
    CHECK(oms.request_cancel(h));
    CHECK(oms.get(h).state == OrderState::PendingCancel);
    CHECK(oms.request_cancel(h).error() == RejectReason::InvalidState);  // already pending
    auto u = oms.on_cancel_ack(cancel_ack(id));
    CHECK(u.order.state == OrderState::Canceled);
    CHECK_FALSE(u.order.has(Order::kUnsolicitedCancel));
    CHECK(oms.request_cancel(h).error() == RejectReason::UnknownOrder);
  }
  SUBCASE("unsolicited cancel") {
    const ClientOrderId id = oms.next_cl_ord_id();
    static_cast<void>(oms.submit(req(Side::Sell, 100, 1), id, {}));
    oms.on_ack(ack(id));
    auto u = oms.on_cancel_ack(cancel_ack(id));
    CHECK(u.terminal);
    CHECK(u.order.has(Order::kUnsolicitedCancel));
    CHECK(oms.stats().unsolicited_cancels == 1);
  }
  SUBCASE("fill after cancel ack is a late fill") {
    const ClientOrderId id = oms.next_cl_ord_id();
    static_cast<void>(oms.submit(req(Side::Sell, 100, 5, InstrumentId{3}), id, {}));
    oms.on_ack(ack(id));
    oms.on_cancel_ack(cancel_ack(id));
    auto u = oms.on_fill(fill(id, 100, 2, 2, "x1"));
    CHECK(u.action == OmsAction::LateFill);
    CHECK(u.known);
    CHECK(u.fill_qty == qt(2));
    CHECK(oms.stats().late_fills == 1);
    // The order is gone, but the terminal record still says what the fill was for.
    CHECK_FALSE(u.handle.valid());
    CHECK(u.order.cl_ord_id == id);
    CHECK(u.order.instrument == InstrumentId{3});
    CHECK(u.order.side == Side::Sell);
    CHECK(u.order.state == OrderState::Canceled);
  }
  SUBCASE("cancel reject after fill is ignored; cancel reject restores state; >3 -> reconcile") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Sell, 100, 5), id, {});
    oms.on_ack(ack(id));
    oms.on_fill(fill(id, 100, 2, 2, "f1"));
    REQUIRE(oms.request_cancel(h));
    for (int i = 1; i <= 4; ++i) {
      auto u = oms.on_cancel_reject(cancel_reject(id));
      CHECK(u.order.state == OrderState::PartiallyFilled);
      CHECK(u.order.cancel_attempts == i);
      CHECK(u.action == (i > 3 ? OmsAction::ReconcileNeeded : OmsAction::None));
      REQUIRE(oms.request_cancel(h));
    }
    oms.on_fill(fill(id, 100, 3, 5, "f2"));  // filled while cancel pending
    auto u = oms.on_cancel_reject(cancel_reject(id));
    CHECK(u.action == OmsAction::Ignored);
    CHECK_FALSE(u.handle.valid());
  }
  SUBCASE("cancel reject with unknown-order means it is gone") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Sell, 100, 5), id, {});
    oms.on_ack(ack(id));
    REQUIRE(oms.request_cancel(h));
    auto u = oms.on_cancel_reject(cancel_reject(id, RejectReason::VenueUnknownOrder));
    CHECK(u.terminal);
    CHECK(u.order.state == OrderState::Canceled);
  }
  SUBCASE("ack for unknown id -> cancel it; unknown fill flagged") {
    auto u = oms.on_ack(ack(ClientOrderId{0xABC}));
    CHECK(u.action == OmsAction::CancelUnknown);
    CHECK_FALSE(u.known);
    CHECK(oms.stats().unknown_ids == 1);
    u = oms.on_fill(fill(ClientOrderId{0xABC}, 1, 1, 1, "u1"));
    CHECK(u.action == OmsAction::UnknownFill);
    CHECK(oms.on_reject(reject(ClientOrderId{0xABC})).action == OmsAction::Ignored);
    CHECK(oms.on_cancel_ack(cancel_ack(ClientOrderId{0xABC})).action == OmsAction::Ignored);
    CHECK(oms.on_expired(expired(ClientOrderId{0xABC})).action == OmsAction::Ignored);
  }
  SUBCASE("expired") {
    const ClientOrderId id = oms.next_cl_ord_id();
    static_cast<void>(oms.submit(req(Side::Sell, 100, 5), id, {}));
    oms.on_ack(ack(id));
    auto u = oms.on_expired(expired(id));
    CHECK(u.order.state == OrderState::Expired);
    CHECK(oms.stats().expired == 1);
  }
  SUBCASE("fill while pending cancel keeps pending; fill of pending new (ack lost)") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Sell, 100, 5), id, {});
    oms.on_ack(ack(id));
    REQUIRE(oms.request_cancel(h));
    auto u = oms.on_fill(fill(id, 100, 1, 1, "p1"));
    CHECK(u.order.state == OrderState::PendingCancel);
    const ClientOrderId id2 = oms.next_cl_ord_id();
    static_cast<void>(oms.submit(req(Side::Sell, 100, 5), id2, {}));
    u = oms.on_fill(fill(id2, 100, 1, 1, "p2"));
    CHECK(u.order.state == OrderState::PartiallyFilled);
    // fill with cum going backwards is clamped, over-fill clamped to qty
    u = oms.on_fill(fill(id2, 100, 1, 0, "p3"));
    CHECK(u.order.cum_qty == qt(2));
    u = oms.on_fill(fill(id2, 100, 100, 100, "p4"));
    CHECK(u.order.state == OrderState::Filled);
    CHECK(u.order.cum_qty == qt(5));
  }
  SUBCASE("replace with a new id: ack, reject, cancel-then-new leg") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Buy, 100, 5), id, {});
    oms.on_ack(ack(id));
    oms.on_fill(fill(id, 100, 1, 1, "r0"));
    const ClientOrderId nid = oms.next_cl_ord_id();
    CHECK(oms.request_replace(h, nid, px(101), qt(7)));
    CHECK(oms.get(h).state == OrderState::PendingReplace);
    CHECK(oms.find(nid) == h);
    CHECK(oms.request_replace(h, oms.next_cl_ord_id(), px(1), qt(1)).error() ==
          RejectReason::InvalidState);
    // Binance-style: old leg cancelled first, then the new leg acked
    auto u = oms.on_cancel_ack(cancel_ack(id, 1));
    CHECK_FALSE(u.terminal);
    CHECK(oms.get(h).state == OrderState::PendingReplace);
    u = oms.on_ack(ack(nid, "V2"));
    CHECK(u.order.state == OrderState::Live);
    CHECK(u.order.cl_ord_id == nid);
    CHECK(u.order.price == px(101));
    CHECK(u.order.qty == qt(7));
    CHECK(u.order.cum_qty == Qty{});  // new venue order
    CHECK(u.order.venue_order_id == "V2");
    CHECK_FALSE(oms.find(id).valid());
    CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == qt(7));
    CHECK(oms.best_own_px(InstrumentId{0}, Side::Buy) == px(101));
    // replace rejected without the old leg cancelled: order keeps working unchanged
    const ClientOrderId nid2 = oms.next_cl_ord_id();
    CHECK(oms.request_replace(h, nid2, px(102), qt(9)));
    u = oms.on_reject(reject(nid2));
    CHECK(u.order.state == OrderState::Live);
    CHECK(u.order.price == px(101));
    CHECK_FALSE(oms.find(nid2).valid());
    // replace rejected after the old leg was cancelled: order is gone
    const ClientOrderId nid3 = oms.next_cl_ord_id();
    CHECK(oms.request_replace(h, nid3, px(102), qt(9)));
    oms.on_cancel_ack(cancel_ack(nid));
    u = oms.on_reject(reject(nid3));
    CHECK(u.terminal);
    CHECK(u.order.state == OrderState::Canceled);
    CHECK(oms.open_count() == 0);
  }
  SUBCASE("in-place amend keeps the id and cum qty") {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = *oms.submit(req(Side::Buy, 100, 5), id, {});
    oms.on_ack(ack(id));
    oms.on_fill(fill(id, 100, 2, 2, "a0"));
    CHECK(oms.request_replace(h, id, px(99), qt(6)));
    auto u = oms.on_ack(ack(id));
    CHECK(u.order.state == OrderState::PartiallyFilled);
    CHECK(u.order.cum_qty == qt(2));
    CHECK(u.order.qty == qt(6));
    CHECK(u.order.price == px(99));
    CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == qt(4));
  }
  SUBCASE("best own price tracking across instruments and sides") {
    auto h1 = *oms.submit(req(Side::Buy, 100, 1), oms.next_cl_ord_id(), {});
    auto h2 = *oms.submit(req(Side::Buy, 101, 1), oms.next_cl_ord_id(), {});
    static_cast<void>(oms.submit(req(Side::Sell, 105, 1), oms.next_cl_ord_id(), {}));
    static_cast<void>(
        oms.submit(req(Side::Sell, 104, 1, InstrumentId{1}), oms.next_cl_ord_id(), {}));
    CHECK(oms.best_own_px(inst, Side::Buy) == px(101));
    CHECK(oms.best_own_px(inst, Side::Sell) == px(105));
    CHECK(oms.best_own_px(InstrumentId{1}, Side::Sell) == px(104));
    CHECK(oms.open_count(inst) == 3);
    const ClientOrderId id2 = oms.get(h2).cl_ord_id;
    oms.on_ack(ack(id2));
    oms.on_cancel_ack(cancel_ack(id2));
    CHECK(oms.best_own_px(inst, Side::Buy) == px(100));
    static_cast<void>(h1);
    std::vector<std::uint32_t> order;
    oms.for_each_open_order([&](Handle<Order> h, const Order&) { order.push_back(h.idx); });
    CHECK(order.size() == 3);
    CHECK(std::is_sorted(order.begin(), order.end()));
    int n = 0;
    oms.for_each_open_order(inst, [&](Handle<Order>, const Order&) { ++n; });
    CHECK(n == 2);
  }
}

TEST_CASE("core.oms: recently-terminal ring evicts oldest") {
  Oms oms;
  ClientOrderId first{};
  for (std::size_t i = 0; i < kRecentlyTerminal + 10; ++i) {
    const ClientOrderId id = oms.next_cl_ord_id();
    if (i == 0) first = id;
    static_cast<void>(oms.submit(req(Side::Buy, 1, 1), id, {}));
    oms.on_reject(reject(id));
  }
  CHECK(oms.classify(first) == OrderClass::Unknown);
  CHECK(oms.classify(make_cl_ord_id(1, kRecentlyTerminal + 10)) == OrderClass::RecentlyTerminal);
  // exec dedupe ring evicts too: the first exec id becomes acceptable again
  const ClientOrderId id = oms.next_cl_ord_id();
  static_cast<void>(oms.submit(req(Side::Buy, 1, 100000), id, {}));
  oms.on_ack(ack(id));
  CHECK(oms.on_fill(fill(id, 1, 1, 0, "first")).action == OmsAction::None);
  for (std::size_t i = 0; i < kRecentlyTerminal; ++i) {
    oms.on_fill(fill(id, 1, 1, 0, ("e" + std::to_string(i)).c_str()));
  }
  CHECK(oms.on_fill(fill(id, 1, 1, 0, "first")).action == OmsAction::None);
}

TEST_CASE("core.oms: pool exhaustion") {
  Oms oms;
  for (std::size_t i = 0; i < kMaxOpenOrders; ++i)
    REQUIRE(oms.submit(req(Side::Buy, 1, 1), oms.next_cl_ord_id(), {}));
  CHECK(oms.submit(req(Side::Buy, 1, 1), oms.next_cl_ord_id(), {}).error() ==
        RejectReason::PoolExhausted);
}

TEST_CASE("core.oms: reconcile") {
  Oms oms;
  const ClientOrderId a = oms.next_cl_ord_id();
  const ClientOrderId b = oms.next_cl_ord_id();
  const ClientOrderId c = oms.next_cl_ord_id();
  auto ha = *oms.submit(req(Side::Buy, 100, 5), a, {});
  static_cast<void>(oms.submit(req(Side::Buy, 99, 5), b, {}));
  auto hc = *oms.submit(req(Side::Sell, 101, 5), c, {});
  oms.on_ack(ack(a));
  oms.on_ack(ack(b));
  oms.on_ack(ack(c));
  REQUIRE(oms.request_cancel(hc));  // pending cancel at reconcile time
  oms.reconcile_begin();
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile);
  m.kind = ReconcileMsg::Kind::OpenOrder;
  m.cl_ord_id = a;
  m.cum_qty = qt(2);  // venue says 2 filled that we missed
  m.venue_order_id = "VA";
  auto u = oms.reconcile_open_order(m);
  CHECK(u.changed);
  CHECK(u.order.cum_qty == qt(2));
  CHECK(oms.get(ha).state == OrderState::Live);
  m.cl_ord_id = c;
  m.cum_qty = Qty{};
  u = oms.reconcile_open_order(m);
  CHECK(u.order.state == OrderState::PendingCancel);  // its cancel is still in flight
  m.cl_ord_id = ClientOrderId{0x777};
  u = oms.reconcile_open_order(m);
  CHECK(u.action == OmsAction::CancelUnknown);
  std::vector<ClientOrderId> canceled;
  oms.reconcile_end([&](const OmsUpdate& x) { canceled.push_back(x.order.cl_ord_id); });
  REQUIRE(canceled.size() == 1);
  CHECK(canceled[0] == b);
  CHECK(oms.open_count() == 2);
  CHECK(oms.classify(b) == OrderClass::RecentlyTerminal);
  CHECK(oms.open_qty(InstrumentId{0}, Side::Buy) == qt(3));
}

TEST_CASE("core.oms: reconcile keeps a cancel or replace in flight pending") {
  Oms oms;
  const ClientOrderId a = oms.next_cl_ord_id();
  const ClientOrderId b = oms.next_cl_ord_id();
  const Handle<Order> ha = *oms.submit(req(Side::Buy, 100, 5), a, {});
  const Handle<Order> hb = *oms.submit(req(Side::Sell, 101, 5), b, {});
  oms.on_ack(ack(a));
  oms.on_ack(ack(b));
  REQUIRE(oms.request_cancel(ha));
  const ClientOrderId b2 = oms.next_cl_ord_id();
  REQUIRE(oms.request_replace(hb, b2, px(102), qt(5)));
  oms.reconcile_begin();
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile);
  m.kind = ReconcileMsg::Kind::OpenOrder;
  m.cl_ord_id = a;
  m.cum_qty = qt(1);
  m.venue_order_id = "VA";
  OmsUpdate u = oms.reconcile_open_order(m);
  CHECK(u.order.state == OrderState::PendingCancel);
  CHECK(u.order.cum_qty == qt(1));
  m.cl_ord_id = b;
  m.cum_qty = Qty{};
  u = oms.reconcile_open_order(m);
  CHECK(u.order.state == OrderState::PendingReplace);
  CHECK(u.order.pending_cl_ord_id == b2);
  std::size_t ended = 0;
  oms.reconcile_end([&](const OmsUpdate&) { ++ended; });
  CHECK(ended == 0);
  // The replies to the requests sent before the reconciliation settle both orders.
  u = oms.on_cancel_ack(cancel_ack(a, 1));
  CHECK(u.terminal);
  CHECK(u.order.state == OrderState::Canceled);
  CHECK_FALSE(u.order.has(Order::kUnsolicitedCancel));
  u = oms.on_ack(ack(b2));
  CHECK(u.order.state == OrderState::Live);
  CHECK(u.order.cl_ord_id == b2);
  CHECK(u.order.price == px(102));
  CHECK(oms.open_count() == 1);
  CHECK(oms.open_qty(InstrumentId{0}, Side::Sell) == qt(5));
}

TEST_CASE("core.oms: reconcile spares orders sent after the request and other venues' orders") {
  Oms oms;
  const ClientOrderId a = oms.next_cl_ord_id();
  static_cast<void>(oms.submit(req(Side::Buy, 100, 5), a, {}));
  oms.on_ack(ack(a));
  NewOrderRequest other = req(Side::Sell, 101, 5);
  other.venue = VenueId{1};
  const ClientOrderId d = oms.next_cl_ord_id();  // another venue: not part of this snapshot
  static_cast<void>(oms.submit(other, d, {}));
  oms.on_ack(ack(d));
  const ClientOrderId b = oms.next_cl_ord_id();  // cancelled at the venue while disconnected
  static_cast<void>(oms.submit(req(Side::Buy, 99, 5), b, {}));
  oms.on_ack(ack(b));
  const ClientOrderId e = oms.next_cl_ord_id();  // sent, but the snapshot missed it
  static_cast<void>(oms.submit(req(Side::Sell, 102, 5), e, {}));
  // The venue requested its open orders after sending e.
  oms.reconcile_begin(VenueId{0}, e);
  const ClientOrderId c = oms.next_cl_ord_id();  // sent after the request
  static_cast<void>(oms.submit(req(Side::Sell, 103, 5), c, {}));
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile);
  m.kind = ReconcileMsg::Kind::OpenOrder;
  m.cl_ord_id = a;
  static_cast<void>(oms.reconcile_open_order(m));
  std::vector<ClientOrderId> ended;
  ClientOrderId placed{};
  oms.reconcile_end(
      [&](const OmsUpdate& x) {
        CHECK(x.terminal);
        CHECK(x.order.state == OrderState::Canceled);
        ended.push_back(x.order.cl_ord_id);
        if (!placed.valid()) {  // callers may place orders from the callback
          placed = oms.next_cl_ord_id();
          REQUIRE(oms.submit(req(Side::Buy, 98, 1), placed, {}));
        }
      },
      VenueId{0});
  REQUIRE(ended.size() == 2);
  CHECK(ended[0] == b);
  CHECK(ended[1] == e);
  CHECK(oms.classify(a) == OrderClass::Open);
  CHECK(oms.classify(d) == OrderClass::Open);
  CHECK(oms.classify(placed) == OrderClass::Open);
  REQUIRE(oms.classify(c) == OrderClass::Open);
  CHECK(oms.get(oms.find(c)).state == OrderState::PendingNew);
  // c's ack is applied as usual.
  OmsUpdate u = oms.on_ack(ack(c));
  CHECK(u.changed);
  CHECK(u.order.state == OrderState::Live);
  // e was taken for gone, yet the venue acks it: it must be cancelled, not ignored.
  u = oms.on_ack(ack(e));
  CHECK(u.known);
  CHECK(u.action == OmsAction::CancelUnknown);
  // A late ack for an order the venue itself cancelled is still ignored.
  const ClientOrderId f = oms.next_cl_ord_id();
  static_cast<void>(oms.submit(req(Side::Buy, 97, 1), f, {}));
  oms.on_ack(ack(f));
  oms.on_cancel_ack(cancel_ack(f));
  CHECK(oms.on_ack(ack(f)).action == OmsAction::Ignored);
}
