#include "fastmm/sim/sim_transport.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fastmm::sim {

namespace {

// `v` in decimal into `out`, a zero-initialised message field (its tail stays zero).
void put_decimal(FixedString<40>& out, std::uint64_t v) noexcept {
  const auto r = std::to_chars(out.data(), out.data() + FixedString<40>::kCapacity, v);
  out.set_size(static_cast<std::size_t>(r.ptr - out.data()));
}

Timestamp venue_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

// Latency seed of every venue after the first, which keeps SimTransportConfig::seed (so a
// single-venue run draws exactly what it always drew).
std::uint64_t venue_seed(std::uint64_t seed, VenueId v) noexcept {
  std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (static_cast<std::uint64_t>(v.value) + 1);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

}  // namespace

SimTransport::SimTransport(const SimClock& clock,
                           const InstrumentTable& instruments,
                           const SimTransportConfig& cfg)
    : clock_(clock),
      instruments_(instruments),
      cfg_(cfg),
      me_(cfg.fill_model == FillModel::Matching
              ? std::make_unique<MatchingEngine>(instruments.size(), this)
              : nullptr),
      own_world_(std::make_unique<SimWorld>(instruments.size())),
      world_(own_world_.get()),
      mirror_(world_->books()),
      queue_(cfg.queue_conservatism_bps),
      touch_(world_->touches()),
      tape_(world_->tapes()) {
  if (!cfg.venue.valid() || cfg.venue.value >= kMaxVenues)
    throw std::invalid_argument("sim: default venue id out of range");
  const auto venue_of = [&](const Instrument& i) { return i.venue.valid() ? i.venue : cfg.venue; };
  bool used[kMaxVenues] = {};
  for (const Instrument& i : instruments) {
    const VenueId v = venue_of(i);
    if (v.value >= kMaxVenues) {
      throw std::invalid_argument("sim: instrument " + std::string(i.symbol.view()) +
                                  " is on venue " + std::to_string(v.value) + ", at most " +
                                  std::to_string(kMaxVenues) + " venues are simulated");
    }
    used[v.value] = true;
  }
  if (instruments.size() == 0) used[cfg.venue.value] = true;
  // A pool member of a simulated venue is simulated too, with its own link.
  if (cfg.pools.active) {
    for (std::uint8_t v = 0; v < kMaxVenues; ++v) {
      const VenueId primary = cfg.pools.primary(VenueId{v});
      if (primary.value != v && primary.value < kMaxVenues && used[primary.value]) used[v] = true;
    }
  }
  for (const SimVenueConfig& c : cfg.venues) {
    if (!c.venue.valid() || c.venue.value >= kMaxVenues || !used[c.venue.value]) {
      throw std::invalid_argument("sim: settings for venue " + std::to_string(c.venue.value) +
                                  ", which no instrument trades on");
    }
  }
  std::uint8_t index_of[kMaxVenues] = {};
  for (std::uint8_t v = 0; v < kMaxVenues; ++v) {
    const SimVenueConfig vc = cfg.venue_config(VenueId{v});
    replace_[v] = vc.supports_replace;
    if (!used[v]) continue;
    const std::uint64_t seed = n_links_ == 0 ? cfg.seed : venue_seed(cfg.seed, VenueId{v});
    index_of[v] = static_cast<std::uint8_t>(n_links_);
    links_[n_links_].emplace(VenueId{v}, vc, seed, cfg).index = static_cast<std::uint8_t>(n_links_);
    ++n_links_;
  }
  if (!cfg.accounts.empty())
    accounts_ =
        std::make_unique<SimAccounts>(instruments, cfg.accounts, cfg.initial_margin, &cfg.pools);
  if (cfg.pools.active) order_venues_ = std::make_unique<OrderVenues>();
  if (cfg.own_orders_in_feed) {
    const std::size_t n = instruments.size() == 0 ? 1 : instruments.size();
    own_feed_ = std::make_unique<OwnFeed[]>(n);
    for (std::size_t i = 0; i < n; ++i) {
      OwnFeed& f = own_feed_[i];
      for (std::vector<Level>& v : f.shown) v.reserve(16);
      f.changed.reserve(16);
    }
    own_buf_ = std::make_unique<EventBuf>();
  }
  if (me_ != nullptr) me_->set_stp(kStrategyAccount, cfg.stp);
  for (Link*& l : link_of_inst_) l = &at(0);
  for (const Instrument& i : instruments) {
    const VenueId v = venue_of(i);
    link_of_inst_[i.id.value] = &at(index_of[v.value]);
    if (me_ != nullptr) me_->set_stp(kStrategyAccount, i.id, cfg.venue_config(v).stp);
  }
  if (cfg.fill_model == FillModel::L3Queue) own_world_->set_l3(true);
}

bool SimTransport::venue_budget(VenueId v, OrderBudget& out) const noexcept {
  for (std::size_t k = 0; k < n_links_; ++k) {
    const Link& l = at(k);
    if (l.id != v) continue;
    if (!l.limited()) return false;
    const Timestamp now = clock_.now();
    out.orders_10s = RateWindow{10'000, l.orders_10s.used_at(now), l.orders_10s.limit};
    out.orders_1d = RateWindow{86'400'000, l.orders_1d.used_at(now), l.orders_1d.limit};
    out.venue_known = true;
    out.orders_taken = l.taken;
    return true;
  }
  return false;
}

LatencyModel& SimTransport::latency(VenueId v) noexcept {
  for (std::size_t k = 0; k < n_links_; ++k) {
    if (at(k).id == v) return at(k).lat;
  }
  return at(0).lat;
}

void SimTransport::enable_aggregator(Timestamp start) noexcept {
  MdAggregatorConfig mc = cfg_.md;
  mc.venue = link(InstrumentId{0}).id;
  agg_ = std::make_unique<MdAggregator>(instruments_.size(), *me_, mc, start);
  own_feed_.reset();  // the aggregated book holds our orders
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
  if (cfg_.hash_outbound) hasher_.add(m);
  Link& l = link_for(m);
  // The venue's order count (new orders and replaces; a cancel is free), decided when the order
  // is sent: the arrival refuses one that was past the window.
  bool over_limit = false;
  if (m.type != EventType::OutCancel) ++l.taken;
  if (m.type != EventType::OutCancel && l.limited()) {
    over_limit = l.orders_10s.full(now) || l.orders_1d.full(now);
    if (!over_limit) {
      l.orders_10s.add(now);
      l.orders_1d.add(now);
    }
  }
  LatencySample s = m.type == EventType::OutCancel ? l.lat.cancel_out() : l.lat.order_out();
  if (recorded_ != nullptr) {
    const Duration rec = recorded_->out(
        m.type == EventType::OutCancel ? RecordedLatency::Path::Cancel : RecordedLatency::Path::New,
        l.id,
        m.instrument.value,
        now);
    if (rec > s.delay + recorded_->margin) s.delay = rec;
  }
  if (s.dropped) {
    ++stats_.dropped;
    if (observer_ != nullptr) observer_->on_order_sent(m, now, Timestamp{});
    return true;  // accepted by the transport, lost in the network
  }
  Timestamp arrival = now + s.delay;
  // One connection's messages are taken in one after another (SimVenueConfig::order_service).
  if (l.order_service.ns > 0 && l.last_order_processed.valid() &&
      arrival < l.last_order_processed + l.order_service)
    arrival = l.last_order_processed + l.order_service;
  OutSlot* slot = sched_.push(arrival);
  if (slot == nullptr) {
    ++stats_.scheduler_full;
    if (observer_ != nullptr) observer_->on_order_sent(m, now, Timestamp{});
    return false;
  }
  slot->len = m.len;
  slot->over_limit = over_limit ? 1 : 0;
  std::memcpy(slot->bytes, &m, m.len);
  if (l.order_service.ns > 0) l.last_order_processed = arrival;
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
  if (e.payload.over_limit != 0) {
    // Past the venue's order-count window: a new order is refused, a cancelReplace is refused
    // whole and the original stays.
    if (h->type == EventType::OutNewOrder) {
      const auto& m = msg_cast<OutNewOrderMsg>(h);
      note_order(m.cl_ord_id, link_for(m.hdr));
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueRateLimit, now);
    } else if (h->type == EventType::OutReplace) {
      const auto& m = msg_cast<OutReplaceMsg>(h);
      note_order(m.cl_ord_id, order_link(m.orig_cl_ord_id, m.hdr.instrument));
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueRateLimit, now);
    }
    return;
  }
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
  if (watch_ != nullptr) note_crossed(h->instrument);
}

void SimTransport::note_crossed(InstrumentId id) noexcept {
  if (id.value >= crossed_seen_.size()) return;
  bool any = false;
  queue_.for_each(id, [&](QueuePositionModel::Handle32, const QueuedOrder& o) {
    any = any || o.crossed.is_positive();
  });
  if (static_cast<std::uint8_t>(any) == crossed_seen_[id.value]) return;
  crossed_seen_[id.value] = static_cast<std::uint8_t>(any);
  watch_->on_crossed(watch_run_, id, any);
}

void SimTransport::venue_new(const OutNewOrderMsg& m, Timestamp now) noexcept {
  note_order(m.cl_ord_id, link_for(m.hdr));
  if (accounts_ != nullptr) {
    if (m.reduce_only != 0 &&
        accounts_->reducible(m.hdr.instrument, m.side, m.hdr.venue).is_zero()) {
      accounts_->count_refused();
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueReject, now, kReduceOnlyCode);
      return;
    }
    const Price px =
        m.type == OrderType::Market ? sweep_price(m.hdr.instrument, m.side, m.qty, now) : m.price;
    if (!accounts_->admit(m.cl_ord_id,
                          m.hdr.instrument,
                          m.side,
                          px,
                          m.qty,
                          m.reduce_only != 0,
                          {},
                          m.hdr.venue)) {
      accounts_->count_refused();
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::InsufficientBalance, now);
      return;
    }
  }
  NewOrder n;
  n.account = kStrategyAccount;
  n.cl_ord_id = m.cl_ord_id;
  n.instrument = m.hdr.instrument;
  n.side = m.side;
  n.type = m.type;
  n.tif = m.tif;
  n.price = m.price;
  n.qty = m.qty;
  n.reduce_only = m.reduce_only != 0;
  if (queue_mode()) {
    queue_new(n, now);
  } else {
    static_cast<void>(me_->submit(n, now));  // effects arrive through the sink
  }
}

void SimTransport::venue_cancel(const OutCancelMsg& m, Timestamp now) noexcept {
  // A cancel of an order the venue does not hold is refused on the link it came in on.
  if (order_venues_ != nullptr && !order_venues_->contains(m.cl_ord_id.value))
    note_order(m.cl_ord_id, link_for(m.hdr));
  if (queue_mode()) {
    queue_cancel(m.cl_ord_id, m.hdr.instrument, now);
  } else {
    static_cast<void>(me_->cancel(kStrategyAccount, m.cl_ord_id, now));
  }
}

void SimTransport::venue_replace(const OutReplaceMsg& m, Timestamp now) noexcept {
  note_order(m.cl_ord_id, order_link(m.orig_cl_ord_id, m.hdr.instrument));
  // A reduce-only order whose position is closed, or a new leg the account cannot cover: the
  // venue cancels the order and refuses its replacement (Binance cancelReplace).
  const bool closed = accounts_ != nullptr && accounts_->reducible(m.orig_cl_ord_id).is_zero();
  if (accounts_ != nullptr &&
      (closed || !accounts_->admit_replace(m.orig_cl_ord_id, m.cl_ord_id, m.price, m.qty))) {
    accounts_->count_refused();
    if (queue_mode()) {
      queue_cancel(m.orig_cl_ord_id, m.hdr.instrument, now);
    } else {
      static_cast<void>(me_->cancel(kStrategyAccount, m.orig_cl_ord_id, now));
    }
    if (closed) {
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueReject, now, kReduceOnlyCode);
    } else {
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::InsufficientBalance, now);
    }
    return;
  }
  if (queue_mode()) {
    queue_replace(m, now);
  } else {
    static_cast<void>(
        me_->replace(kStrategyAccount, m.orig_cl_ord_id, m.cl_ord_id, m.price, m.qty, now));
  }
}

Price SimTransport::sweep_price(InstrumentId id, Side side, Qty qty, Timestamp now) const noexcept {
  if (id.value >= instruments_.size()) return Price{};
  Int128 paid = 0;  // raw price x raw qty
  Qty left = qty;
  const auto take = [&](const Level& l) {
    const Qty q = min(left, l.qty);
    paid += static_cast<Int128>(l.price.raw) * q.raw;
    left -= q;
    return left.is_positive();
  };
  const Side opp = opposite(side);
  if (queue_mode()) {
    for_each_venue_level(mirror_[id.value], touch_[id.value], opp, now, take);
  } else {
    const SimBook::Levels& levels = me_->book(id).levels(opp);
    for (std::size_t i = levels.size(); i > 0; --i) {
      if (!take(Level{levels[i - 1].price, levels[i - 1].qty})) break;
    }
  }
  if (paid <= 0 || !qty.is_positive()) return Price{};
  return Price::from_raw(static_cast<std::int64_t>((paid + qty.raw - 1) / qty.raw));
}

void SimTransport::flush_md(Timestamp now) noexcept {
  if (agg_ != nullptr) agg_->flush(now, &SimTransport::emit_md_thunk, this);
}

void SimTransport::emit_md_thunk(void* ctx, EventHeader& m, Timestamp venue_ts) noexcept {
  static_cast<SimTransport*>(ctx)->push_md_wire(m, venue_ts);
}

void SimTransport::publish_balances(Timestamp now) noexcept {
  if (accounts_ == nullptr) return;
  for (std::size_t k = 0; k < n_links_; ++k) {
    Link& l = at(k);
    if (!accounts_->enabled(l.id)) continue;
    accounts_->snapshot(l.id, now, [&](BalanceMsg& m) { push_balance(l, m); });
  }
}

void SimTransport::publish_positions(Timestamp now,
                                     std::span<const ReconcileMsg> positions) noexcept {
  for (ReconcileMsg m : positions) {
    if (m.hdr.instrument.value >= instruments_.size()) continue;
    Link* l = order_venues_ != nullptr ? link_of_venue(m.hdr.venue) : nullptr;
    if (l == nullptr) l = &link(m.hdr.instrument);
    m.hdr.exch_ts = now;
    const Timestamp arrival = std::max(l->last_order_arrival, now);
    l->last_order_arrival = arrival;
    m.hdr.recv_ts = arrival;
    m.hdr.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
    m.hdr.seq = 0;
    if (!wire_push(*l, kOrderWire, m.hdr)) ++stats_.wire_full;
  }
}

void SimTransport::publish_reconcile(Timestamp ts,
                                     std::uint32_t venues,
                                     std::span<const ReconcileMsg> positions) noexcept {
  for (std::uint32_t v = 0; v < 32; ++v) {
    if (((venues >> v) & 1U) == 0) continue;
    Link* l = link_of_venue(VenueId{static_cast<std::uint8_t>(v)});
    if (l == nullptr) continue;
    const auto push = [&](ReconcileMsg m) {
      m.hdr.venue = l->id;
      m.hdr.exch_ts = ts;
      const Timestamp arrival = std::max(l->last_order_arrival, ts);
      l->last_order_arrival = arrival;
      m.hdr.recv_ts = arrival;
      m.hdr.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
      m.hdr.seq = 0;
      if (!wire_push(*l, kOrderWire, m.hdr)) ++stats_.wire_full;
    };
    ReconcileMsg begin{};
    init_header(begin, EventType::Reconcile, InstrumentId{}, l->id);
    begin.kind = ReconcileMsg::Kind::Begin;
    begin.flags = ReconcileMsg::kExecutionsExact;  // nothing traded before: nothing to estimate
    push(begin);
    for (const ReconcileMsg& p : positions) {
      if (p.hdr.venue == l->id && p.hdr.instrument.value < instruments_.size()) push(p);
    }
    ReconcileMsg end{};
    init_header(end, EventType::Reconcile, InstrumentId{}, l->id);
    end.kind = ReconcileMsg::Kind::End;
    push(end);
  }
}

SimWorld::SimWorld(std::size_t instruments)
    : n_(instruments),
      mirror_(new L2Book<256>[instruments == 0 ? 1 : instruments]),
      touch_(new QueueTouch[instruments == 0 ? 1 : instruments]),
      tape_(new TradeTape[instruments == 0 ? 1 : instruments]) {
  changes_.reserve(std::size_t{2} * 256);
}

void SimWorld::set_l3(bool v) {
  tape_trades_ = !v;
  l3_orders_.clear();
  l3_levels_[0].clear();
  l3_levels_[1].clear();
  if (!v) return;
  l3_orders_.resize(n_);
  l3_levels_[0].resize(n_);
  l3_levels_[1].resize(n_);
}

Qty SimWorld::l3_level(InstrumentId id, Side side, Price px) const noexcept {
  if (id.value >= l3_orders_.size()) return Qty{};
  const auto& lv = l3_levels_[static_cast<std::size_t>(side)][id.value];
  const auto it = lv.find(px.raw);
  return it == lv.end() ? Qty{} : it->second;
}

void SimWorld::l3_level_add(InstrumentId id, Side side, Price px, Qty q, bool add) noexcept {
  auto& lv = l3_levels_[static_cast<std::size_t>(side)][id.value];
  Qty& level = lv[px.raw];
  if (add) {
    level += q;
  } else if (level > q) {
    level -= q;
  } else {
    lv.erase(px.raw);
  }
}

void SimWorld::l3_take(InstrumentId id, std::uint64_t ref, Qty q) noexcept {
  auto& orders = l3_orders_[id.value];
  const auto it = orders.find(ref);
  if (it == orders.end()) {
    ++l3_unknown_;
    return;
  }
  L3Order& o = it->second;
  if (q.is_zero() || q > o.qty) q = o.qty;
  l3_change_ = L3Change{L3Change::Kind::Cancel, o.side, o.price, Price{}, q, o.prio};
  l3_level_add(id, o.side, o.price, q, false);
  o.qty -= q;
  if (o.qty.is_zero()) orders.erase(it);
}

void SimWorld::l3_before(const EventHeader& md) noexcept {
  const InstrumentId id = md.instrument;
  l3_change_ = L3Change{};
  if (id.value >= l3_orders_.size()) return;
  auto& orders = l3_orders_[id.value];
  ++l3_events_;
  switch (md.type) {
    case EventType::OrderAddL3: {
      const auto& m = msg_cast<OrderAddL3Msg>(&md);
      if (!m.qty.is_positive()) return;
      orders[m.order_ref] = L3Order{++l3_arrivals_ << kPrioShift, m.price, m.qty, m.side};
      l3_level_add(id, m.side, m.price, m.qty, true);
      return;
    }
    case EventType::OrderExecL3: {
      const auto& m = msg_cast<OrderExecL3Msg>(&md);
      const auto it = orders.find(m.order_ref);
      if (it == orders.end()) {
        ++l3_unknown_;
        return;
      }
      L3Order& o = it->second;
      const Qty q = min(m.exec_qty, o.qty);
      const Price at = m.exec_price.is_positive() ? m.exec_price : o.price;
      l3_change_ = L3Change{L3Change::Kind::Exec, o.side, o.price, at, q, o.prio};
      l3_level_add(id, o.side, o.price, q, false);
      o.qty -= q;
      if (o.qty.is_zero()) orders.erase(it);
      return;
    }
    case EventType::OrderCancelL3: {
      const auto& m = msg_cast<OrderCancelL3Msg>(&md);
      l3_take(id, m.order_ref, m.canceled_qty);
      return;
    }
    case EventType::OrderReplaceL3: {
      // The replacement joins the back of its level: a new place in the queue.
      const auto& m = msg_cast<OrderReplaceL3Msg>(&md);
      const auto it = orders.find(m.old_order_ref);
      if (it == orders.end()) {
        ++l3_unknown_;
        return;
      }
      const Side side = it->second.side;
      l3_take(id, m.old_order_ref, Qty{});
      if (!m.qty.is_positive()) return;
      orders[m.new_order_ref] = L3Order{++l3_arrivals_ << kPrioShift, m.price, m.qty, side};
      l3_level_add(id, side, m.price, m.qty, true);
      return;
    }
    default:
      return;
  }
}

// The book half of queue_apply_book() and queue_apply_touch() (core/queue_model.hpp); the runs
// take the queue half.
void SimWorld::before(const EventHeader& md, Timestamp now) noexcept {
  const std::size_t i = md.instrument.value;
  if (i >= n_) return;
  switch (md.type) {
    case EventType::BookDelta:
    case EventType::BookSnapshot: {
      const auto& d = msg_cast<BookDeltaMsg>(&md);
      L2Book<256>& book = mirror_[i];
      TradeTape& tape = tape_[i];
      changes_.clear();
      snapshot_ = d.is_snapshot();
      if (snapshot_) {
        book.apply_delta(d);
      } else {
        const Timestamp since = book.last_update();
        for (Side s : {Side::Buy, Side::Sell}) {
          for (const Level& l : (s == Side::Buy ? d.bids() : d.asks())) {
            Qty old;
            book.apply_level(s, l.price, l.qty, old);
            if (tape.size() != 0) old = tape.after(s, l.price, old, since);
            changes_.push_back(LevelChange{s, l.price, old, l.qty});
          }
        }
        book.set_seq(d.last_update_id);
        book.set_last_update(now);
      }
      tape.forget_through(now);
      break;
    }
    case EventType::BookTicker: {
      QueueTouch& t = touch_[i];
      // the replayed feed has none of ours
      t = queue_touch(msg_cast<BookTickerMsg>(&md), [](Side, Price) { return Qty{}; });
      const L2Book<256>& book = mirror_[i];
      touch_newer_ = t.newer_than(book.seq(), book.last_update());
      touch_seen_ = t;
      if (touch_newer_ && tape_[i].size() != 0) {
        touch_seen_.bid_qty = tape_[i].after(Side::Buy, t.bid_px, t.bid_qty, t.ts);
        touch_seen_.ask_qty = tape_[i].after(Side::Sell, t.ask_px, t.ask_qty, t.ts);
      }
      break;
    }
    case EventType::OrderAddL3:
    case EventType::OrderExecL3:
    case EventType::OrderCancelL3:
    case EventType::OrderReplaceL3:
      if (!l3_orders_.empty()) l3_before(md);
      break;
    default:
      break;
  }
}

void SimWorld::after(const EventHeader& md, Timestamp now) noexcept {
  const std::size_t i = md.instrument.value;
  if (i >= n_ || md.type != EventType::Trade || !tape_trades_) return;
  // A print without an aggressor took from no resting side (queue_on_trade).
  if (const auto& t = msg_cast<TradeMsg>(&md); t.has_aggressor()) tape_[i].add(t, now);
}

void SimTransport::attach_world(SimWorld& world) noexcept {
  world_ = &world;
  mirror_ = world.books();
  touch_ = world.touches();
  tape_ = world.tapes();
  own_world_.reset();
}

void SimTransport::on_source_event(const EventHeader& md) noexcept {
  if (queue_mode()) {
    const Timestamp now = venue_time(md);
    world_->before(md, now);
    on_source_event_run(md);
    world_->after(md, now);
  } else {
    on_source_event_run(md);
  }
}

void SimTransport::on_source_event_run(const EventHeader& md) noexcept {
  venue_event(md);
  feed_event(md);
}

void SimTransport::venue_event(const EventHeader& md) noexcept {
  const Timestamp now = venue_time(md);
  const InstrumentId id = md.instrument;
  if (id.value >= instruments_.size()) return;
  switch (md.type) {
    case EventType::BookDelta:
    case EventType::BookSnapshot: {
      const auto& d = msg_cast<BookDeltaMsg>(&md);
      if (queue_mode()) {
        queue_on_delta(d, now);
      } else {
        mirror_on_delta(d, now);
      }
      break;
    }
    case EventType::Trade:
      if (queue_mode()) {
        queue_on_trade(msg_cast<TradeMsg>(&md), now);
      } else {
        mirror_on_trade(msg_cast<TradeMsg>(&md), now);
      }
      break;
    case EventType::BookTicker:
      if (queue_mode()) queue_on_ticker(msg_cast<BookTickerMsg>(&md), now);
      break;
    case EventType::OrderAddL3:
    case EventType::OrderExecL3:
    case EventType::OrderCancelL3:
    case EventType::OrderReplaceL3:
      // The venue's own record of every order; the engine's book is the source's L2 view of it.
      if (cfg_.fill_model == FillModel::L3Queue) queue_on_l3(id, now);
      if (watch_ != nullptr) note_crossed(id);
      return;
    default:
      break;
  }
  if (watch_ != nullptr) note_crossed(id);
  // A derivative account's unrealised PnL: the venue's mark, else the book's mid.
  if (accounts_ != nullptr && accounts_->derivative(id)) [[unlikely]] {
    if (md.type == EventType::PerpState) {
      const auto& p = msg_cast<PerpStateMsg>(&md);
      if ((p.fields & PerpStateMsg::kMark) != 0) accounts_->mark(id, p.mark_price, true);
    } else if (md.type != EventType::Trade) {
      accounts_->mark(id, venue_mid(id), false);
    }
  }
}

bool SimTransport::marks_account(InstrumentId id) const noexcept {
  return accounts_ != nullptr && id.value < instruments_.size() && accounts_->derivative(id);
}

void SimTransport::feed_event(const EventHeader& md) noexcept {
  const Timestamp now = venue_time(md);
  const InstrumentId id = md.instrument;
  // The venue's own record of every order (the engine's book is the source's L2 view of it), and a
  // recorded ticker one side of which was only the session's own orders (strip_own: the venue took
  // its other side; the engine, with none of ours there, would see a level nobody holds) do not go
  // to the engine. Coupled mode publishes its own view of the book; a venue's mark and funding go
  // through as recorded.
  if ((id.value < instruments_.size() && is_l3_event(md.type)) ||
      (agg_ != nullptr && md.type != EventType::PerpState) ||
      (md.type == EventType::BookTicker && hides_level(msg_cast<BookTickerMsg>(&md)))) {
    if (md.seq != 0) forward_in_order(nullptr, md.seq, md.recv_ts, now);
    return;
  }
  if (md.seq != 0) {
    forward_in_order(&md, md.seq, md.recv_ts, now);
  } else {
    forward(md, md.recv_ts, now);
  }
}

bool SimTransport::hides_level(const BookTickerMsg& m) noexcept {
  return (m.bid_px.is_positive() && m.bid_qty.is_zero()) ||
         (m.ask_px.is_positive() && m.ask_qty.is_zero());
}

void SimTransport::forward(const EventHeader& md, Timestamp recorded, Timestamp now) noexcept {
  // Forward a copy with the arrival stamp, on the wire of the instrument's venue.
  Link& l = link(md.instrument);
  Timestamp arrival = now + l.lat.md_in();
  if (l.md_recorded_arrival && recorded > now) arrival = arrival + (recorded - now);
  if (arrival < l.last_md_arrival) arrival = l.last_md_arrival;
  l.last_md_arrival = arrival;
  // Our quantity that changed on the instrument goes right ahead of its message, at its arrival.
  if (own_feed_ != nullptr && md.instrument.value < instruments_.size()) {
    if (const OwnLevelsMsg* own = own_levels(md.instrument, now); own != nullptr)
      push_md_at(l, own->hdr, arrival);
  }
  if (EventHeader* h = push_md_at(l, md, arrival); h != nullptr) {
    h->exch_ts = now;
    ++stats_.md_forwarded;
  }
}

EventHeader* SimTransport::push_md_at(Link& l, const EventHeader& md, Timestamp arrival) noexcept {
  std::byte* p = reserve_md(l, md.len);
  if (p == nullptr) {
    ++stats_.wire_full;
    return nullptr;
  }
  std::memcpy(p, &md, md.len);
  auto* h = reinterpret_cast<EventHeader*>(p);
  h->recv_ts = arrival;
  h->t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h->t1_delta = h->t2_delta = 0;
  h->seq = 0;
  l.md_wire->commit();
  busy_ |= wire_bit(l, kMdWire);
  return h;
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

void SimTransport::mirror_on_trade(const TradeMsg& t, Timestamp now) noexcept {
  if (!t.price.is_positive() || !t.qty.is_positive()) return;
  NewOrder n;
  n.account = kGeneratorAccount;
  n.cl_ord_id = ClientOrderId{next_queue_order_id_++};
  n.instrument = t.hdr.instrument;
  n.side = t.aggressor;
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Ioc;
  n.price = t.price;
  n.qty = t.qty;
  static_cast<void>(me_->submit(n, now));  // effects arrive through the sink
}

void SimTransport::sync_level(
    InstrumentId id, Side side, Price px, Qty target, Timestamp now, bool increases) noexcept {
  if (!px.is_positive()) return;
  const Qty cur = me_->account_qty_at(kGeneratorAccount, id, side, px);
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
    static_cast<void>(me_->submit(n, now));  // may trade through resting strategy orders
  } else if (target < cur && !increases) {
    me_->reduce_account_qty(kGeneratorAccount, id, side, px, cur - target, now);
  }
}

// ---- L2 queue fill model ---------------------------------------------------------------------

// The venue's book after a recorded view at `now`: opposite liquidity that reached our resting
// orders traded with them (QueuePositionModel::on_cross).
void SimTransport::queue_cross(InstrumentId id, Timestamp now) noexcept {
  if (queue_.size() == 0) return;
  QueuePositionModel* const models[] = {&queue_};
  queue_apply_cross(
      mirror_[id.value],
      touch_[id.value],
      id,
      now,
      models,
      [&](std::size_t, QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty ahead) {
        ++stats_.cross_fills;
        queue_fill(h, o, fill, ahead, now);
      });
}

void SimTransport::queue_fill(
    QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty ahead, Timestamp now) noexcept {
  // A reduce-only order fills only while it takes the position towards zero; the rest of it
  // expires (as under the matching model).
  Qty f = fill;
  if (accounts_ != nullptr) {
    const Qty room = accounts_->reducible(o.cl_ord_id);
    if (f > room) {
      o.cum_qty -= f - room;
      f = room;
    }
  }
  if (f.is_positive()) {
    emit_fill(o.cl_ord_id,
              o.order_id,
              o.instrument,
              o.side,
              o.price,
              f,
              o.cum_qty,
              o.leaves(),
              Liquidity::Maker,
              next_exec_id_++,
              now,
              ahead,
              true);
  }
  note_own(o.instrument, o.side, o.price);
  if (f < fill) {
    ++stats_.expired;
    emit_expired(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, now);
    queue_remove(h);
  } else if (o.leaves().is_zero()) {
    queue_remove(h);
  }
}

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
  // The opposite side as the venue has it now: an order is checked and filled against the ticker's
  // level and the depth's levels behind it, never against stale ones (for_each_venue_level).
  const auto for_each_opposite = [&](auto&& f) {
    for_each_venue_level(book, touch_[id.value], opp, now, f);
  };
  Level best{};
  for_each_opposite([&](const Level& l) {
    best = l;
    return false;
  });
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
  // What a reduce-only order may fill: down to a flat position (queue_fill for its resting part).
  const Qty room = accounts_ != nullptr ? accounts_->reducible(n.cl_ord_id) : Qty::max();
  if (n.tif == TimeInForce::Fok && room < n.qty) {
    ++stats_.expired;
    emit_expired(n.cl_ord_id, order_id, id, Qty{}, now);
    return;
  }
  if (n.tif == TimeInForce::Fok) {
    Qty avail{};
    for_each_opposite([&](const Level& l) {
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
    for_each_opposite([&](const Level& l) {
      if (leaves.is_zero() || cum >= room) return false;
      if (!market && !at_or_better(opp, l.price, n.price)) return false;
      const Qty q = min(min(leaves, l.qty), room - cum);
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
  if (market || n.tif == TimeInForce::Ioc || n.tif == TimeInForce::Fok || cum >= room) {
    ++stats_.expired;
    emit_expired(n.cl_ord_id, order_id, id, cum, now);
    return;
  }
  const QueueTouch& touch = touch_[id.value];
  Qty ahead;
  if (cfg_.fill_model == FillModel::L3Queue) {
    // Every recorded order resting at the price came before us.
    ahead = world_->l3_level(id, n.side, n.price);
  } else {
    ahead = queue_at_placement(
        level_qty(book, n.side, n.price), n.side, n.price, book, touch, &tape_[id.value]);
  }
  const auto h = queue_.place(n.cl_ord_id,
                              order_id,
                              id,
                              n.side,
                              n.price,
                              n.qty,
                              ahead,
                              now,
                              queue_view_ts(n.side, n.price, book, touch));
  if (!h.valid()) {
    emit_expired(n.cl_ord_id, order_id, id, cum, now);  // queue table full
    return;
  }
  // The opposite liquidity it took as a taker is still in the recorded book: it crosses the rest
  // of the order, which has taken it already (on_cross).
  if (watch_ != nullptr) watch_->on_rest(watch_run_, id, n.side, n.price, true);
  queue_.get(h).cum_qty = cum;
  queue_.get(h).crossed = cum;
  queue_.get(h).prio = (world_->l3_arrivals() << SimWorld::kPrioShift) | ++own_arrivals_;
  note_own(id, n.side, n.price);
}

void SimTransport::queue_cancel(ClientOrderId id, InstrumentId route, Timestamp now) noexcept {
  const auto h = queue_.find(id);
  if (!h.valid()) {
    emit_cancel_reject(id, InstrumentId{}, now, route);
    return;
  }
  const QueuedOrder o = queue_.get(h);
  queue_remove(h);
  note_own(o.instrument, o.side, o.price);
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
    if (queue_.amend_keep_priority(h, m.cl_ord_id, new_order_id, m.qty, now)) {
      note_own(o.instrument, o.side, o.price);
      emit_cancel_ack(o.cl_ord_id, o.order_id, o.instrument, o.cum_qty, now);
      emit_ack(m.cl_ord_id, new_order_id, o.instrument, now);
      return;
    }
  }
  queue_remove(h);
  note_own(o.instrument, o.side, o.price);
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

// The queue half of a recorded message: the book half is SimWorld::before(), already applied.
void SimTransport::queue_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept {
  const InstrumentId id = d.hdr.instrument;
  // Under l3_queue the order-by-order events have moved the queues already: the view only updates
  // the venue's book (SimWorld::before()) and what it crosses, below.
  if (queue_.size() != 0 && cfg_.fill_model != FillModel::L3Queue) {
    if (world_->snapshot()) {
      const L2Book<256>& book = mirror_[id.value];
      queue_.for_each(id, [&](QueuePositionModel::Handle32 h, const QueuedOrder& o) {
        const Qty shown = level_qty(book, o.side, o.price);
        if (shown < o.ahead) queue_.get(h).ahead = shown;
      });
    } else {
      for (const SimWorld::LevelChange& c : world_->changes())
        queue_.on_level_change(id, c.side, c.px, c.old_qty, c.new_qty);
    }
  }
  queue_cross(id, now);
}

void SimTransport::queue_on_ticker(const BookTickerMsg& m, Timestamp now) noexcept {
  const InstrumentId id = m.hdr.instrument;
  if (world_->touch_newer() && cfg_.fill_model != FillModel::L3Queue)
    queue_.on_touch(id, world_->touch_seen());
  queue_cross(id, now);
}

// The tape gets the trade after the runs took it (SimWorld::after()).
void SimTransport::queue_on_trade(const TradeMsg& t, Timestamp now) noexcept {
  // A print without an aggressor names no resting side to take from; under l3_queue the executions
  // themselves fill (queue_on_l3), and the print is the same execution again.
  if (!t.has_aggressor() || cfg_.fill_model == FillModel::L3Queue) return;
  const InstrumentId id = t.hdr.instrument;
  queue_.on_trade(id,
                  t.price,
                  t.qty,
                  t.aggressor,
                  now,
                  [&](QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty ahead) {
                    queue_fill(h, o, fill, ahead, now);
                  });
}

// The queue half of an order-by-order event: the recorded order's half is SimWorld::before().
void SimTransport::queue_on_l3(InstrumentId id, Timestamp now) noexcept {
  const SimWorld::L3Change& c = world_->l3();
  switch (c.kind) {
    case SimWorld::L3Change::Kind::Cancel:
      queue_.on_l3_cancel(id, c.side, c.px, c.qty, c.prio);
      return;
    case SimWorld::L3Change::Kind::Exec:
      queue_.on_l3_execute(
          id,
          c.side,
          c.px,
          c.exec_px,
          c.qty,
          c.prio,
          now,
          [&](QueuePositionModel::Handle32 h, QueuedOrder& ours, Qty fill, Qty ahead) {
            queue_fill(h, ours, fill, ahead, now);
          });
      return;
    case SimWorld::L3Change::Kind::None:
      return;
  }
}

// ---- MatchingSink ------------------------------------------------------------------------------

void SimTransport::on_ack(const SimOrder& o, Timestamp ts) {
  if (o.account != kStrategyAccount) return;
  note_own(o.instrument, o.side, o.price);
  emit_ack(o.cl_ord_id, o.order_id, o.instrument, ts);
}
void SimTransport::on_reject(const NewOrder& o, RejectReason r, Timestamp ts) {
  if (o.account == kStrategyAccount) emit_reject(o.cl_ord_id, o.instrument, r, ts);
}
void SimTransport::on_cancel(const SimOrder& o, CancelReason r, Timestamp ts) {
  if (o.account != kStrategyAccount) return;
  note_own(o.instrument, o.side, o.price);
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
    const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp now) {
  const std::uint64_t exec = next_exec_id_++;
  // A recorded level processed after our order reached the venue can carry an earlier venue time
  // (the streams are not monotone): the two orders met when the later of them arrived.
  Timestamp ts = now;
  if (maker.created > ts) ts = maker.created;
  if (taker.created > ts) ts = taker.created;
  if (maker.account == kStrategyAccount) {
    note_own(maker.instrument, maker.side, maker.price);
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
  init_header(t, EventType::Trade, id, link(id).id);
  t.price = px;
  t.qty = qty;
  t.trade_id = tid;
  t.aggressor = aggr;
  t.hdr.venue_seq = tid;
  push_md_wire(t.hdr, ts);
}
Qty SimTransport::reducible(const SimOrder& o) {
  if (o.account != kStrategyAccount || accounts_ == nullptr) return o.leaves();
  return min(o.leaves(), accounts_->reducible(o.cl_ord_id));
}

// ---- venue -> engine messages ----------------------------------------------------------------

void SimTransport::emit_ack(ClientOrderId id,
                            std::uint64_t order_id,
                            InstrumentId inst,
                            Timestamp ts) noexcept {
  ++stats_.acks;
  Link& l = order_link(id, inst);
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck, inst, l.id);
  m.cl_ord_id = id;
  put_decimal(m.venue_order_id, order_id);
  push_order_wire(l, m.hdr, ts);
  publish_account(l, ts);
}
void SimTransport::emit_reject(ClientOrderId id,
                               InstrumentId inst,
                               RejectReason r,
                               Timestamp ts,
                               std::int32_t venue_code) noexcept {
  ++stats_.rejects;
  switch (r) {
    case RejectReason::PostOnlyWouldCross:
      ++stats_.rejects_post_only;
      break;
    case RejectReason::VenueReject:
      ++(venue_code == kReduceOnlyCode ? stats_.rejects_other : stats_.rejects_level_full);
      break;
    case RejectReason::InvalidTick:
    case RejectReason::InvalidLot:
    case RejectReason::InstrumentDisabled:
      ++stats_.rejects_invalid;
      break;
    case RejectReason::DuplicateId:
      ++stats_.rejects_duplicate;
      break;
    case RejectReason::InsufficientBalance:
      ++stats_.rejects_balance;
      break;
    case RejectReason::VenueRateLimit:
      ++stats_.rejects_rate_limit;
      break;
    default:
      ++stats_.rejects_other;
      break;
  }
  Link& l = order_link(id, inst);
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject, inst, l.id);
  m.cl_ord_id = id;
  m.reason = r;
  if (r == RejectReason::InsufficientBalance) {
    m.venue_code = -2010;  // Binance: NEW_ORDER_REJECTED, "Account has insufficient balance ..."
    m.text = "Account has insufficient balance";
  } else if (venue_code == kReduceOnlyCode) {
    m.venue_code = kReduceOnlyCode;
    m.text = "ReduceOnly Order is rejected.";
  } else {
    m.venue_code = -static_cast<std::int32_t>(r);
    m.text = to_string(r);
  }
  // A duplicate id names another order, whose hold stays.
  if (r != RejectReason::DuplicateId) {
    if (accounts_ != nullptr) accounts_->close(id);
    forget_order(id);
  }
  push_order_wire(l, m.hdr, ts);
  publish_account(l, ts);
}
void SimTransport::emit_cancel_ack(
    ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept {
  ++stats_.cancel_acks;
  Link& l = order_link(id, inst);
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck, inst, l.id);
  m.cl_ord_id = id;
  put_decimal(m.venue_order_id, order_id);
  m.cum_qty = cum;
  if (accounts_ != nullptr) accounts_->close(id);
  forget_order(id);
  push_order_wire(l, m.hdr, ts);
  publish_account(l, ts);
}
void SimTransport::emit_cancel_reject(ClientOrderId id,
                                      InstrumentId inst,
                                      Timestamp ts,
                                      InstrumentId route) noexcept {
  ++stats_.cancel_rejects;
  Link& l = order_link(id, inst.valid() ? inst : route);
  forget_order(id);  // the venue does not hold it
  OrderCancelRejectMsg m{};
  init_header(m, EventType::OrderCancelReject, inst, l.id);
  m.cl_ord_id = id;
  m.reason = RejectReason::VenueUnknownOrder;
  m.venue_code = -2011;  // Binance: unknown order sent
  m.text = "Unknown order sent.";
  push_order_wire(l, m.hdr, ts);
}
void SimTransport::emit_expired(
    ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept {
  Link& l = order_link(id, inst);
  OrderExpiredMsg m{};
  init_header(m, EventType::OrderExpired, inst, l.id);
  m.cl_ord_id = id;
  put_decimal(m.venue_order_id, order_id);
  m.cum_qty = cum;
  if (accounts_ != nullptr) accounts_->close(id);
  forget_order(id);
  push_order_wire(l, m.hdr, ts);
  publish_account(l, ts);
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
                             Timestamp ts,
                             Qty queue_ahead,
                             bool queue_known) noexcept {
  ++stats_.fills;
  Link& l = order_link(id, inst);
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, inst, l.id);
  m.cl_ord_id = id;
  put_decimal(m.venue_order_id, order_id);
  put_decimal(m.exec_id, exec_id);
  m.price = px;
  m.qty = qty;
  m.cum_qty = cum;
  m.leaves_qty = leaves;
  // The fee is on the contract's notional: a futures contract with a multiplier (Gate's
  // NVDA_USDT is 0.01 NVDA) trades much less than price x qty.
  m.fee = inst.value < instruments_.size()
              ? cfg_.fees.schedule(inst).fee(instruments_[inst].notional(px, qty), liq)
              : cfg_.fees.fee(inst, px, qty, liq);
  m.side = side;
  m.liquidity = liq;
  stats_.fees_charged += m.fee;
  m.hdr.exch_ts = ts;
  if (observer_ != nullptr) {
    FillContext ctx;
    ctx.mid = venue_mid(inst);
    ctx.best_bid = venue_best(inst, Side::Buy);
    ctx.best_ask = venue_best(inst, Side::Sell);
    ctx.queue_ahead = queue_ahead;
    ctx.queue_known = queue_known;
    observer_->on_fill(m, ts, ctx);
  }
  if (accounts_ != nullptr) accounts_->fill(id, px, qty, leaves, m.fee);
  if (!leaves.is_positive()) forget_order(id);
  push_order_wire(l, m.hdr, ts);
  publish_account(l, ts);
}

void SimTransport::push_order_wire(Link& l, EventHeader& h, Timestamp venue_ts) noexcept {
  h.exch_ts = venue_ts;
  if (h.type == EventType::OrderFill && l.lat.own_fill()) {
    // The private stream: in order among fills and balances, apart from the acks.
    Duration d = l.lat.fill_in();
    if (recorded_ != nullptr) {
      const Duration rec = recorded_->in(RecordedLatency::Path::Fill, l.id, venue_ts);
      if (rec > d + recorded_->margin) d = rec;
    }
    Timestamp arrival = venue_ts + d;
    if (arrival < l.last_user_arrival) arrival = l.last_user_arrival;
    l.last_user_arrival = arrival;
    h.recv_ts = arrival;
    h.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
    h.t1_delta = h.t2_delta = 0;
    h.seq = 0;
    if (!wire_push(l, kUserWire, h)) ++stats_.wire_full;
    return;
  }
  Duration d = l.lat.ack_in();
  if (recorded_ != nullptr) {
    const Duration rec = recorded_->in(
        h.type == EventType::OrderFill ? RecordedLatency::Path::Fill : RecordedLatency::Path::Ack,
        l.id,
        venue_ts);
    if (rec > d + recorded_->margin) d = rec;
  }
  Timestamp arrival = venue_ts + d;
  if (arrival < l.last_order_arrival) arrival = l.last_order_arrival;
  l.last_order_arrival = arrival;
  h.recv_ts = arrival;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h.t1_delta = h.t2_delta = 0;
  h.seq = 0;
  if (observer_ != nullptr && h.type != EventType::OrderFill)
    observer_->on_order_event(h, venue_ts);
  if (!wire_push(l, kOrderWire, h)) ++stats_.wire_full;
}

TransferState SimTransport::transfer(
    VenueId from, VenueId to, std::string_view asset, Notional amount, Timestamp ts) noexcept {
  if (accounts_ == nullptr || !accounts_->transfer(from, to, asset, amount))
    return TransferState::Failed;
  for (const VenueId v : {from, to}) {
    if (Link* l = link_of_venue(v); l != nullptr) publish_account(*l, ts);
  }
  return TransferState::Done;
}

Notional SimTransport::move_money(VenueId venue,
                                  std::string_view asset,
                                  Notional amount,
                                  Timestamp ts) noexcept {
  if (accounts_ == nullptr) return Notional{};
  const Notional moved = accounts_->move(venue, asset, amount);
  if (Link* l = link_of_venue(venue); l != nullptr && !moved.is_zero()) publish_account(*l, ts);
  return moved;
}

void SimTransport::publish_snapshot(VenueId venue, Timestamp ts) noexcept {
  Link* l = link_of_venue(venue);
  if (accounts_ == nullptr || l == nullptr || !accounts_->enabled(venue)) return;
  accounts_->snapshot(venue, ts, [&](BalanceMsg& m) { push_balance(*l, m); });
}

void SimTransport::publish_account(Link& l, Timestamp venue_ts) noexcept {
  if (accounts_ != nullptr)
    accounts_->publish(l.id, venue_ts, [&](BalanceMsg& m) { push_balance(l, m); });
}

void SimTransport::push_balance(Link& l, BalanceMsg& m) noexcept {
  if (l.lat.own_fill()) {
    // The private stream: behind the fill that moved it, else the stream's fixed delay.
    const Timestamp arrival =
        std::max(l.last_user_arrival, m.hdr.exch_ts + l.lat.fill_in_params().fixed);
    l.last_user_arrival = arrival;
    m.hdr.recv_ts = arrival;
    m.hdr.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
    if (!wire_push(l, kUserWire, m.hdr)) ++stats_.wire_full;
    return;
  }
  const Timestamp arrival = std::max(l.last_order_arrival, m.hdr.exch_ts);
  l.last_order_arrival = arrival;
  m.hdr.recv_ts = arrival;
  m.hdr.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  if (!wire_push(l, kOrderWire, m.hdr)) ++stats_.wire_full;
}

void SimTransport::push_md_wire(EventHeader& h, Timestamp venue_ts) noexcept {
  Link& l = link(h.instrument);
  h.exch_ts = venue_ts;
  Timestamp arrival = venue_ts + l.lat.md_in();
  if (arrival < l.last_md_arrival) arrival = l.last_md_arrival;
  l.last_md_arrival = arrival;
  h.recv_ts = arrival;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h.t1_delta = h.t2_delta = 0;
  h.seq = 0;
  std::byte* p = reserve_md(l, h.len);
  if (p == nullptr) {
    ++stats_.wire_full;
    return;
  }
  std::memcpy(p, &h, h.len);
  l.md_wire->commit();
  busy_ |= wire_bit(l, kMdWire);
  ++stats_.md_forwarded;
}

std::byte* SimTransport::reserve_md(Link& l, std::uint32_t len) noexcept {
  std::byte* p = l.md_wire->try_reserve(len);
  while (p == nullptr && l.md_wire->capacity() < kMaxMdWireBytes) {
    if (!grow_wire(l.md_wire)) return nullptr;
    p = l.md_wire->try_reserve(len);
  }
  return p;
}

// The messages in flight live in a ring small enough to stay in cache; a burst that fills it
// (recorded arrival times seconds late, say) moves it to one twice the size, in order.
bool SimTransport::grow_wire(std::unique_ptr<MsgRing>& ring) noexcept {
  std::unique_ptr<MsgRing> bigger;
  try {
    bigger = std::make_unique<MsgRing>(ring->capacity() * 2, false);
  } catch (...) {
    return false;
  }
  while (const std::byte* m = ring->try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(m);
    static_cast<void>(bigger->try_push(h, h->len));
    ring->release();
  }
  ring = std::move(bigger);
  return true;
}

// ---- our orders in the recorded feed -----------------------------------------------------------

void SimTransport::note_own(InstrumentId id, Side side, Price px) noexcept {
  if (own_feed_ == nullptr || id.value >= instruments_.size() || !px.is_positive()) return;
  std::vector<std::pair<Side, Price>>& changed = own_feed_[id.value].changed;
  const std::pair<Side, Price> key{side, px};
  if (std::find(changed.begin(), changed.end(), key) != changed.end()) return;
  if (changed.empty() && watch_ != nullptr) watch_->on_own_changed(watch_run_, id);
  changed.push_back(key);
}

const OwnLevelsMsg* SimTransport::own_levels(InstrumentId id, Timestamp now) noexcept {
  OwnFeed& f = own_feed_[id.value];
  if (f.changed.empty()) return nullptr;
  auto& out = own_buf_->as<OwnLevelsMsg>();
  constexpr std::size_t kCap = (kMaxSourceEventBytes - sizeof(OwnLevelsMsg)) / sizeof(Level);
  Level* lv = out.levels();
  std::uint32_t count[2] = {0, 0};
  std::size_t n = 0;
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::vector<Level>& shown = f.shown[static_cast<std::size_t>(side)];
    for (const auto& [s, px] : f.changed) {
      if (s != side) continue;
      const Qty q = model_own_at(id, side, px);
      auto it =
          std::find_if(shown.begin(), shown.end(), [&](const Level& l) { return l.price == px; });
      const Qty was = it != shown.end() ? it->qty : Qty{};
      if (q == was || n >= kCap) continue;
      if (it == shown.end()) {
        shown.push_back(Level{px, q});
      } else if (q.is_positive()) {
        it->qty = q;
      } else {
        *it = shown.back();
        shown.pop_back();
      }
      lv[n++] = Level{px, q};
      ++count[static_cast<std::size_t>(side)];
    }
  }
  f.changed.clear();
  if (n == 0) return nullptr;
  init_header(
      out, EventType::OwnLevels, id, link(id).id, OwnLevelsMsg::size_for(count[0], count[1]));
  out.hdr.flags |= EventHeader::kSynthetic;
  out.hdr.exch_ts = now;
  out.bid_count = count[0];
  out.ask_count = count[1];
  out.first_update_id = out.last_update_id = out.prev_update_id = 0;
  ++stats_.own_levels;
  ++stats_.md_forwarded;
  return &out;
}

Qty SimTransport::model_own_at(InstrumentId id, Side side, Price px) const noexcept {
  if (cfg_.fill_model == FillModel::Matching)
    return me_->account_qty_at(kStrategyAccount, id, side, px);
  Qty sum{};
  queue_.for_each(id, [&](QueuePositionModel::Handle32, const QueuedOrder& o) {
    if (o.side == side && o.price == px) sum += o.leaves();
  });
  return sum;
}

// ---- engine side -------------------------------------------------------------------------------

Timestamp SimTransport::head_ts(MsgRing& ring) noexcept {
  const std::byte* p = ring.try_peek();
  return p == nullptr ? Timestamp::max() : reinterpret_cast<const EventHeader*>(p)->recv_ts;
}

Timestamp SimTransport::next_inbound_ts() noexcept {
  Timestamp t = Timestamp::max();
  for (std::uint32_t m = busy_; m != 0; m &= m - 1) {
    const auto b = static_cast<unsigned>(std::countr_zero(m));
    t = std::min(t, head_ts(wire(at(b / 3), b % 3)));
  }
  return t;
}

SimTransport::Inbound SimTransport::next_inbound() noexcept {
  Timestamp a = Timestamp::max();
  Timestamp b = Timestamp::max();
  for (std::uint32_t m = busy_; m != 0; m &= m - 1) {
    const auto bit = static_cast<unsigned>(std::countr_zero(m));
    const Timestamp ts = head_ts(wire(at(bit / 3), bit % 3));
    if (bit % 3 == kMdWire) {
      b = std::min(b, ts);
    } else {
      a = std::min(a, ts);
    }
  }
  return a <= b ? Inbound{a, false} : Inbound{b, true};
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
  // Earliest head among the order wires (and private streams) and among the md wires; a tie keeps
  // the lower venue, and an order connection's message before its private stream's.
  // The busy wires in bit order are the venues in order, each one's order wire before its private
  // stream before its md wire, so the strict comparisons keep the ties as listed.
  unsigned ord = 32;
  unsigned md = 32;
  Timestamp a = Timestamp::max();
  Timestamp b = Timestamp::max();
  for (std::uint32_t m = busy_; m != 0; m &= m - 1) {
    const auto bit = static_cast<unsigned>(std::countr_zero(m));
    const Timestamp ts = head_ts(wire(at(bit / 3), bit % 3));
    if (bit % 3 == kMdWire) {
      if (ts < b) {
        b = ts;
        md = bit;
      }
    } else if (ts < a) {
      a = ts;
      ord = bit;
    }
  }
  EventType t = EventType::Padding;
  const unsigned take = ord != 32 && a <= b ? ord : md;
  if (take == 32) return t;
  Link& l = at(take / 3);
  MsgRing& ring = wire(l, take % 3);
  if (move_head(ring, feed, t)) {
    if (take == ord) {
      ++stats_.order_events_delivered;
    } else {
      ++stats_.md_delivered;
    }
  }
  if (ring.try_peek() == nullptr) busy_ &= ~(std::uint32_t{1} << take);
  return t;
}

// ---- queries -----------------------------------------------------------------------------------

Price SimTransport::venue_mid(InstrumentId id) const noexcept {
  if (id.value >= instruments_.size()) return Price{};
  if (queue_mode() || (agg_ == nullptr && me_->book(id).depth(Side::Buy) == 0)) {
    return mirror_[id.value].mid();
  }
  const MatchingEngine::TopOfBook top = me_->top_of_book(id);
  if (top.bid.qty.is_zero() || top.ask.qty.is_zero()) return mirror_[id.value].mid();
  return Price::from_raw((top.bid.price.raw + top.ask.price.raw) / 2);
}

Price SimTransport::venue_best(InstrumentId id, Side side) const noexcept {
  if (id.value >= instruments_.size()) return Price{};
  const L2Book<256>& mirror = mirror_[id.value];
  const Level m = side == Side::Buy ? mirror.best_bid() : mirror.best_ask();
  if (queue_mode() || (agg_ == nullptr && me_->book(id).depth(Side::Buy) == 0)) {
    return m.price;
  }
  const MatchingEngine::TopOfBook top = me_->top_of_book(id);
  const Level& t = side == Side::Buy ? top.bid : top.ask;
  return t.qty.is_zero() ? m.price : t.price;
}

MatchingEngine::SideExposure SimTransport::strategy_exposure(InstrumentId id,
                                                             Side side) const noexcept {
  if (cfg_.fill_model == FillModel::Matching) return me_->exposure(kStrategyAccount, id, side);
  MatchingEngine::SideExposure e;
  queue_.for_each(id, [&](QueuePositionModel::Handle32, const QueuedOrder& o) {
    if (o.side != side) return;
    ++e.orders;
    e.leaves += o.leaves();
    if (e.best.is_zero() || better(side, o.price, e.best)) e.best = o.price;
  });
  return e;
}

}  // namespace fastmm::sim
