#include "fastmm/sim/sim_transport.hpp"

#include <cstdio>
#include <cstring>

namespace fastmm::sim {

namespace {

FixedString<40> decimal_id(std::uint64_t v) noexcept {
  char buf[24];
  const int n = std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(v));
  FixedString<40> s;
  s.assign(std::string_view(buf, static_cast<std::size_t>(n > 0 ? n : 0)));
  return s;
}

// qty resting at `px` in a sorted L2Book side (worst..best order), 0 if absent.
Qty level_qty(const L2Book<256>& book, Side side, Price px) noexcept {
  const auto& v = book.raw(side);
  std::size_t lo = 0;
  std::size_t hi = v.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (better(side, px, v[mid].price)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return (lo < v.size() && v[lo].price == px) ? v[lo].qty : Qty{};
}

Timestamp venue_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

}  // namespace

SimTransport::SimTransport(const SimClock& clock,
                           const InstrumentTable& instruments,
                           const SimTransportConfig& cfg)
    : clock_(clock),
      instruments_(instruments),
      cfg_(cfg),
      me_(instruments.size(), this),
      lat_(cfg.order_out, cfg.ack_in, cfg.md_in, cfg.seed),
      md_wire_(cfg.md_wire_bytes),
      order_wire_(cfg.order_wire_bytes),
      mirror_(new L2Book<256>[instruments.size() == 0 ? 1 : instruments.size()]),
      queue_(cfg.queue_conservatism_bps) {
  me_.set_stp(kStrategyAccount, cfg.stp);
}

void SimTransport::enable_aggregator(Timestamp start) noexcept {
  MdAggregatorConfig mc = cfg_.md;
  mc.venue = cfg_.venue;
  agg_ = std::make_unique<MdAggregator>(instruments_.size(), me_, mc, start);
}

// ---- TransportLike ---------------------------------------------------------------------------

bool SimTransport::send(const EventHeader& m) noexcept {
  FASTMM_ASSERT(m.len <= kOutSlotBytes);
  const Timestamp now = clock_.now();
  switch (m.type) {
    case EventType::OutNewOrder:
      ++stats_.orders_sent;
      break;
    case EventType::OutCancel:
      ++stats_.cancels_sent;
      break;
    case EventType::OutReplace:
      ++stats_.replaces_sent;
      break;
    default:
      return true;  // not an order message: accepted and ignored
  }
  hasher_.add(m);
  const LatencySample s = lat_.order_out();
  if (s.dropped) {
    ++stats_.dropped;
    if (observer_ != nullptr) observer_->on_order_sent(m, now, Timestamp{});
    return true;  // accepted by the transport, lost in the network
  }
  OutSlot slot{};
  slot.len = m.len;
  std::memcpy(slot.bytes, &m, m.len);
  const Timestamp arrival = now + s.delay;
  if (!sched_.push(arrival, slot)) {
    ++stats_.scheduler_full;
    if (observer_ != nullptr) observer_->on_order_sent(m, now, Timestamp{});
    return false;
  }
  if (observer_ != nullptr) observer_->on_order_sent(m, now, arrival);
  return true;
}

std::size_t SimTransport::send(std::span<const EventHeader* const> batch) noexcept {
  std::size_t ok = 0;
  for (const EventHeader* m : batch) {
    if (!send(*m)) break;  // a prefix, like LiveTransport
    ++ok;
  }
  return ok;
}

// ---- venue side ------------------------------------------------------------------------------

void SimTransport::process_order_arrival() noexcept {
  Scheduler::Entry e{};
  if (!sched_.pop(e)) return;
  const auto* h = reinterpret_cast<const EventHeader*>(e.payload.bytes);
  const Timestamp now = e.fire_ts;
  switch (h->type) {
    case EventType::OutNewOrder:
      venue_new(msg_cast<OutNewOrderMsg>(h), now);
      break;
    case EventType::OutCancel:
      venue_cancel(msg_cast<OutCancelMsg>(h), now);
      break;
    case EventType::OutReplace:
      venue_replace(msg_cast<OutReplaceMsg>(h), now);
      break;
    default:
      break;
  }
}

void SimTransport::venue_new(const OutNewOrderMsg& m, Timestamp now) noexcept {
  NewOrder n;
  n.account = kStrategyAccount;
  n.cl_ord_id = m.cl_ord_id;
  n.instrument = m.hdr.instrument;
  n.side = m.side;
  n.type = m.type;
  n.tif = m.tif;
  n.price = m.price;
  n.qty = m.qty;
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_new(n, now);
  } else {
    static_cast<void>(me_.submit(n, now));  // effects arrive through the sink
  }
}

void SimTransport::venue_cancel(const OutCancelMsg& m, Timestamp now) noexcept {
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_cancel(m.cl_ord_id, now, CancelReason::Requested);
  } else {
    static_cast<void>(me_.cancel(kStrategyAccount, m.cl_ord_id, now));
  }
}

void SimTransport::venue_replace(const OutReplaceMsg& m, Timestamp now) noexcept {
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_replace(m, now);
  } else {
    static_cast<void>(
        me_.replace(kStrategyAccount, m.orig_cl_ord_id, m.cl_ord_id, m.price, m.qty, now));
  }
}

void SimTransport::flush_md(Timestamp now) noexcept {
  if (agg_ != nullptr) agg_->flush(now, &SimTransport::emit_md_thunk, this);
}

void SimTransport::emit_md_thunk(void* ctx, EventHeader& m, Timestamp venue_ts) noexcept {
  static_cast<SimTransport*>(ctx)->push_md_wire(m, venue_ts);
}

void SimTransport::on_source_event(const EventHeader& md) noexcept {
  const Timestamp now = venue_time(md);
  const InstrumentId id = md.instrument;
  if (id.value < instruments_.size()) {
    switch (md.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot: {
        const auto& d = msg_cast<BookDeltaMsg>(&md);
        if (cfg_.fill_model == FillModel::L2Queue) {
          queue_on_delta(d, now);
        } else {
          mirror_on_delta(d, now);
        }
        break;
      }
      case EventType::Trade:
        if (cfg_.fill_model == FillModel::L2Queue) queue_on_trade(msg_cast<TradeMsg>(&md), now);
        break;
      default:
        break;
    }
  }
  if (agg_ != nullptr) return;  // coupled mode publishes its own view of the book
  // Forward a copy with the arrival stamp.
  std::byte* p = md_wire_.try_reserve(md.len);
  if (p == nullptr) {
    ++stats_.wire_full;
    return;
  }
  std::memcpy(p, &md, md.len);
  auto* h = reinterpret_cast<EventHeader*>(p);
  h->exch_ts = now;
  Timestamp arrival = now + lat_.md_in();
  if (arrival < last_md_arrival_) arrival = last_md_arrival_;
  last_md_arrival_ = arrival;
  h->recv_ts = arrival;
  h->t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h->t1_delta = h->t2_delta = 0;
  h->seq = 0;
  md_wire_.commit();
  ++stats_.md_forwarded;
}

// ---- matching model fed with historical levels ------------------------------------------------

void SimTransport::mirror_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept {
  const InstrumentId id = d.hdr.instrument;
  L2Book<256>& book = mirror_[id.value];
  // Levels are synced in two passes, every decrease (on both sides) before any increase.
  // Syncing in message order (bids, then asks) would let a raised bid cross an ask level
  // that the same batch deletes, so historical liquidity would trade against itself and the
  // venue book would drift away from the recorded one.
  Level old_levels[2][256];
  std::size_t n_old[2] = {0, 0};
  if (d.is_snapshot()) {
    // Levels that vanish with the new snapshot must be removed from the venue book too.
    for (Side s : {Side::Buy, Side::Sell}) {
      const auto& v = book.raw(s);
      const auto k = static_cast<std::size_t>(s);
      n_old[k] = v.size();
      for (std::size_t i = 0; i < v.size(); ++i) old_levels[k][i] = v[i];
    }
  }
  book.apply_delta(d);
  for (const bool increases : {false, true}) {
    for (Side s : {Side::Buy, Side::Sell}) {
      const auto k = static_cast<std::size_t>(s);
      for (std::size_t i = 0; i < n_old[k]; ++i) {
        const Price px = old_levels[k][i].price;
        sync_level(id, s, px, level_qty(book, s, px), now, increases);
      }
      for (const Level& l : (s == Side::Buy ? d.bids() : d.asks()))
        sync_level(id, s, l.price, l.qty, now, increases);
    }
  }
}

void SimTransport::sync_level(
    InstrumentId id, Side side, Price px, Qty target, Timestamp now, bool increases) noexcept {
  if (!px.is_positive()) return;
  const Qty cur = me_.account_qty_at(kGeneratorAccount, id, side, px);
  if (target > cur) {
    if (!increases) return;
    NewOrder n;
    n.account = kGeneratorAccount;
    n.cl_ord_id = ClientOrderId{next_queue_order_id_++};
    n.instrument = id;
    n.side = side;
    n.type = OrderType::Limit;
    n.tif = TimeInForce::Gtc;
    n.price = px;
    n.qty = target - cur;
    static_cast<void>(me_.submit(n, now));  // may trade through resting strategy orders
  } else if (target < cur && !increases) {
    me_.reduce_account_qty(kGeneratorAccount, id, side, px, cur - target, now);
  }
}

// ---- L2 queue fill model ---------------------------------------------------------------------

void SimTransport::queue_new(const NewOrder& n, Timestamp now) noexcept {
  const InstrumentId id = n.instrument;
  L2Book<256>& book = mirror_[id.value];
  const bool market = n.type == OrderType::Market;
  if (!n.qty.is_positive() || (!market && !n.price.is_positive())) {
    emit_reject(
        n.cl_ord_id, id, market ? RejectReason::InvalidLot : RejectReason::InvalidTick, now);
    return;
  }
  if (queue_.find(n.cl_ord_id).valid()) {
    emit_reject(n.cl_ord_id, id, RejectReason::DuplicateId, now);
    return;
  }
  const Side opp = opposite(n.side);
  const Level best = opp == Side::Buy ? book.best_bid() : book.best_ask();
  const bool crosses =
      best.qty.is_positive() &&
      (market || (n.side == Side::Buy ? n.price >= best.price : n.price <= best.price));
  if (n.type == OrderType::PostOnly && crosses) {
    emit_reject(n.cl_ord_id, id, RejectReason::PostOnlyWouldCross, now);
    return;
  }
  const std::uint64_t order_id = next_queue_order_id_++;
  emit_ack(n.cl_ord_id, order_id, id, now);
  Qty leaves = n.qty;
  Qty cum{};
  if (n.tif == TimeInForce::Fok) {
    Qty avail{};
    book.for_each_level(opp, [&](const Level& l) {
      if (!market && !at_or_better(opp, l.price, n.price)) return false;
      avail += l.qty;
      return avail < n.qty;
    });
    if (avail < n.qty) {
      ++stats_.expired;
      emit_expired(n.cl_ord_id, order_id, id, Qty{}, now);
      return;
    }
  }
  if (crosses) {
    // Taker against displayed liquidity, best first, at each level's price.
    book.for_each_level(opp, [&](const Level& l) {
      if (leaves.is_zero()) return false;
      if (!market && !at_or_better(opp, l.price, n.price)) return false;
      const Qty q = min(leaves, l.qty);
      cum += q;
      leaves -= q;
      emit_fill(n.cl_ord_id,
                order_id,
                id,
                n.side,
                l.price,
                q,
                cum,
                leaves,
                Liquidity::Taker,
                next_exec_id_++,
                now);
      return true;
    });
  }
  if (leaves.is_zero()) return;
  if (market || n.tif == TimeInForce::Ioc || n.tif == TimeInForce::Fok) {
    ++stats_.expired;
    emit_expired(n.cl_ord_id, order_id, id, cum, now);
    return;
  }
  const Qty ahead = level_qty(book, n.side, n.price);
  const auto h = queue_.place(n.cl_ord_id, order_id, id, n.side, n.price, n.qty, ahead);
  if (!h.valid()) {
    emit_expired(n.cl_ord_id, order_id, id, cum, now);  // queue table full
    return;
  }
  queue_.get(h).cum_qty = cum;
}

void SimTransport::queue_cancel(ClientOrderId id, Timestamp now, CancelReason why) noexcept {
  const auto h = queue_.find(id);
  if (!h.valid()) {
    emit_cancel_reject(id, InstrumentId{}, now);
    return;
  }
  const QueuedOrder o = queue_.get(h);
  queue_.remove(h);
  static_cast<void>(why);
  emit_cancel_ack(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, now);
}

void SimTransport::queue_replace(const OutReplaceMsg& m, Timestamp now) noexcept {
  const auto h = queue_.find(m.orig_cl_ord_id);
  if (!h.valid()) {
    emit_cancel_reject(m.orig_cl_ord_id, m.hdr.instrument, now);
    emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueUnknownOrder, now);
    return;
  }
  const QueuedOrder o = queue_.get(h);
  if (m.price == o.price && m.qty <= o.leaves()) {
    const std::uint64_t new_order_id = next_queue_order_id_++;
    if (queue_.amend_keep_priority(h, m.cl_ord_id, new_order_id, m.qty)) {
      emit_cancel_ack(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, now);
      emit_ack(m.cl_ord_id, new_order_id, o.instrument, now);
      return;
    }
  }
  queue_.remove(h);
  emit_cancel_ack(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, now);
  NewOrder n;
  n.account = kStrategyAccount;
  n.cl_ord_id = m.cl_ord_id;
  n.instrument = o.instrument;
  n.side = o.side;
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Gtc;
  n.price = m.price;
  n.qty = m.qty;
  queue_new(n, now);
}

void SimTransport::queue_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept {
  const InstrumentId id = d.hdr.instrument;
  L2Book<256>& book = mirror_[id.value];
  if (d.is_snapshot()) {
    book.apply_delta(d);
    // No per-level history across a snapshot: clamp the queue ahead of us to what is shown.
    queue_.for_each([&](QueuePositionModel::Handle32 h, const QueuedOrder& o) {
      if (o.instrument != id) return;
      const Qty shown = level_qty(book, o.side, o.price);
      if (shown < o.ahead) queue_.get(h).ahead = shown;
    });
    static_cast<void>(now);
    return;
  }
  for (Side s : {Side::Buy, Side::Sell}) {
    for (const Level& l : (s == Side::Buy ? d.bids() : d.asks())) {
      const Qty old = level_qty(book, s, l.price);
      book.apply_level(s, l.price, l.qty);
      queue_.on_level_change(id, s, l.price, old, l.qty);
    }
  }
  book.set_seq(d.last_update_id);
  book.set_last_update(now);
}

void SimTransport::queue_on_trade(const TradeMsg& t, Timestamp now) noexcept {
  const InstrumentId id = t.hdr.instrument;
  queue_.on_trade(id,
                  t.price,
                  t.qty,
                  t.aggressor,
                  [&](QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill) {
                    emit_fill(o.cl_ord_id,
                              o.order_id,
                              id,
                              o.side,
                              o.price,
                              fill,
                              o.cum_qty,
                              o.leaves(),
                              Liquidity::Maker,
                              next_exec_id_++,
                              now);
                    if (o.leaves().is_zero()) queue_.remove(h);
                  });
}

// ---- MatchingSink ------------------------------------------------------------------------------

void SimTransport::on_ack(const SimOrder& o, Timestamp ts) {
  if (o.account == kStrategyAccount) emit_ack(o.cl_ord_id, o.order_id, o.instrument, ts);
}
void SimTransport::on_reject(const NewOrder& o, RejectReason r, Timestamp ts) {
  if (o.account == kStrategyAccount) emit_reject(o.cl_ord_id, o.instrument, r, ts);
}
void SimTransport::on_cancel(const SimOrder& o, CancelReason r, Timestamp ts) {
  if (o.account != kStrategyAccount) return;
  if (is_expiry(r)) {
    ++stats_.expired;
    emit_expired(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, ts);
  } else {
    emit_cancel_ack(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, ts);
  }
}
void SimTransport::on_cancel_reject(AccountId a,
                                    ClientOrderId id,
                                    InstrumentId inst,
                                    Timestamp ts) {
  if (a == kStrategyAccount) emit_cancel_reject(id, inst, ts);
}
void SimTransport::on_fill(
    const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp ts) {
  const std::uint64_t exec = next_exec_id_++;
  if (maker.account == kStrategyAccount) {
    emit_fill(maker.cl_ord_id,
              maker.order_id,
              maker.instrument,
              maker.side,
              px,
              qty,
              maker.cum_qty,
              maker.leaves(),
              Liquidity::Maker,
              exec,
              ts);
  }
  if (taker.account == kStrategyAccount) {
    emit_fill(taker.cl_ord_id,
              taker.order_id,
              taker.instrument,
              taker.side,
              px,
              qty,
              taker.cum_qty,
              taker.leaves(),
              Liquidity::Taker,
              exec,
              ts);
  }
}
void SimTransport::on_book_change(InstrumentId id, Side s, Price px, Qty qty, std::uint64_t uid) {
  if (agg_ != nullptr) agg_->on_book_change(id, s, px, qty, uid);
}
void SimTransport::on_trade(
    InstrumentId id, Price px, Qty qty, Side aggr, std::uint64_t tid, Timestamp ts) {
  if (agg_ == nullptr) return;  // historical mode: the source carries its own trades
  TradeMsg t{};
  init_header(t, EventType::Trade, id, cfg_.venue);
  t.price = px;
  t.qty = qty;
  t.trade_id = tid;
  t.aggressor = aggr;
  t.hdr.venue_seq = tid;
  push_md_wire(t.hdr, ts);
}

// ---- venue -> engine messages ----------------------------------------------------------------

void SimTransport::emit_ack(ClientOrderId id,
                            std::uint64_t order_id,
                            InstrumentId inst,
                            Timestamp ts) noexcept {
  ++stats_.acks;
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.venue_order_id = decimal_id(order_id);
  push_order_wire(m.hdr, ts);
}
void SimTransport::emit_reject(ClientOrderId id,
                               InstrumentId inst,
                               RejectReason r,
                               Timestamp ts) noexcept {
  ++stats_.rejects;
  switch (r) {
    case RejectReason::PostOnlyWouldCross:
      ++stats_.rejects_post_only;
      break;
    case RejectReason::VenueReject:
      ++stats_.rejects_level_full;
      break;
    case RejectReason::InvalidTick:
    case RejectReason::InvalidLot:
    case RejectReason::InstrumentDisabled:
      ++stats_.rejects_invalid;
      break;
    case RejectReason::DuplicateId:
      ++stats_.rejects_duplicate;
      break;
    default:
      ++stats_.rejects_other;
      break;
  }
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.reason = r;
  m.venue_code = -static_cast<std::int32_t>(r);
  m.text = to_string(r);
  push_order_wire(m.hdr, ts);
}
void SimTransport::emit_cancel_ack(
    ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept {
  ++stats_.cancel_acks;
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.venue_order_id = decimal_id(order_id);
  m.cum_qty = cum;
  push_order_wire(m.hdr, ts);
}
void SimTransport::emit_cancel_reject(ClientOrderId id, InstrumentId inst, Timestamp ts) noexcept {
  ++stats_.cancel_rejects;
  OrderCancelRejectMsg m{};
  init_header(m, EventType::OrderCancelReject, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.reason = RejectReason::VenueUnknownOrder;
  m.venue_code = -2011;  // Binance: unknown order sent
  m.text = "Unknown order sent.";
  push_order_wire(m.hdr, ts);
}
void SimTransport::emit_expired(
    ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept {
  OrderExpiredMsg m{};
  init_header(m, EventType::OrderExpired, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.venue_order_id = decimal_id(order_id);
  m.cum_qty = cum;
  push_order_wire(m.hdr, ts);
}
void SimTransport::emit_fill(ClientOrderId id,
                             std::uint64_t order_id,
                             InstrumentId inst,
                             Side side,
                             Price px,
                             Qty qty,
                             Qty cum,
                             Qty leaves,
                             Liquidity liq,
                             std::uint64_t exec_id,
                             Timestamp ts) noexcept {
  ++stats_.fills;
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, inst, cfg_.venue);
  m.cl_ord_id = id;
  m.venue_order_id = decimal_id(order_id);
  m.exec_id = decimal_id(exec_id);
  m.price = px;
  m.qty = qty;
  m.cum_qty = cum;
  m.leaves_qty = leaves;
  m.fee = cfg_.fees.fee(px, qty, liq);
  m.side = side;
  m.liquidity = liq;
  stats_.fees_charged += m.fee;
  m.hdr.exch_ts = ts;
  if (observer_ != nullptr) observer_->on_fill(m, ts, venue_mid(inst));
  push_order_wire(m.hdr, ts);
}

void SimTransport::push_order_wire(EventHeader& h, Timestamp venue_ts) noexcept {
  h.exch_ts = venue_ts;
  Timestamp arrival = venue_ts + lat_.ack_in();
  if (arrival < last_order_arrival_) arrival = last_order_arrival_;
  last_order_arrival_ = arrival;
  h.recv_ts = arrival;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h.t1_delta = h.t2_delta = 0;
  h.seq = 0;
  if (observer_ != nullptr && h.type != EventType::OrderFill)
    observer_->on_order_event(h, venue_ts);
  if (!order_wire_.try_push(&h, h.len)) ++stats_.wire_full;
}

void SimTransport::push_md_wire(EventHeader& h, Timestamp venue_ts) noexcept {
  h.exch_ts = venue_ts;
  Timestamp arrival = venue_ts + lat_.md_in();
  if (arrival < last_md_arrival_) arrival = last_md_arrival_;
  last_md_arrival_ = arrival;
  h.recv_ts = arrival;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h.t1_delta = h.t2_delta = 0;
  h.seq = 0;
  if (!md_wire_.try_push(&h, h.len)) {
    ++stats_.wire_full;
    return;
  }
  ++stats_.md_forwarded;
}

// ---- engine side -------------------------------------------------------------------------------

Timestamp SimTransport::head_ts(MsgRing& ring) noexcept {
  const std::byte* p = ring.try_peek();
  return p == nullptr ? Timestamp::max() : reinterpret_cast<const EventHeader*>(p)->recv_ts;
}

Timestamp SimTransport::next_inbound_ts() noexcept {
  const Timestamp a = head_ts(order_wire_);
  const Timestamp b = head_ts(md_wire_);
  return a < b ? a : b;
}

bool SimTransport::move_head(MsgRing& ring, InlineFeed& feed, EventType& type) noexcept {
  const std::byte* p = ring.try_peek();
  if (p == nullptr) return false;
  const auto* h = reinterpret_cast<const EventHeader*>(p);
  std::byte* dst = feed.reserve(h->len);
  if (dst == nullptr) return false;
  std::memcpy(dst, p, h->len);
  type = h->type;
  feed.commit();
  ring.release();
  return true;
}

EventType SimTransport::deliver_next_inbound(InlineFeed& feed) noexcept {
  const Timestamp a = head_ts(order_wire_);
  const Timestamp b = head_ts(md_wire_);
  EventType t = EventType::Padding;
  if (a == Timestamp::max() && b == Timestamp::max()) return t;
  if (a <= b) {
    if (move_head(order_wire_, feed, t)) ++stats_.order_events_delivered;
  } else {
    if (move_head(md_wire_, feed, t)) ++stats_.md_delivered;
  }
  return t;
}

// ---- queries -----------------------------------------------------------------------------------

Price SimTransport::venue_mid(InstrumentId id) const noexcept {
  if (id.value >= instruments_.size()) return Price{};
  if (cfg_.fill_model == FillModel::L2Queue ||
      (agg_ == nullptr && me_.book(id).depth(Side::Buy) == 0)) {
    return mirror_[id.value].mid();
  }
  const MatchingEngine::TopOfBook top = me_.top_of_book(id);
  if (top.bid.qty.is_zero() || top.ask.qty.is_zero()) return mirror_[id.value].mid();
  return Price::from_raw((top.bid.price.raw + top.ask.price.raw) / 2);
}

MatchingEngine::SideExposure SimTransport::strategy_exposure(InstrumentId id,
                                                             Side side) const noexcept {
  if (cfg_.fill_model == FillModel::Matching) return me_.exposure(kStrategyAccount, id, side);
  MatchingEngine::SideExposure e;
  queue_.for_each([&](QueuePositionModel::Handle32, const QueuedOrder& o) {
    if (o.instrument != id || o.side != side) return;
    ++e.orders;
    e.leaves += o.leaves();
    if (e.best.is_zero() || better(side, o.price, e.best)) e.best = o.price;
  });
  return e;
}

}  // namespace fastmm::sim
