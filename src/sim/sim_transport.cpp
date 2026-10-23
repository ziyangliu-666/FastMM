#include "fastmm/sim/sim_transport.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fastmm::sim {

namespace {

FixedString<40> decimal_id(std::uint64_t v) noexcept {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  FixedString<40> s;
  s.assign(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
  return s;
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
      me_(instruments.size(), this),
      mirror_(new L2Book<256>[instruments.size() == 0 ? 1 : instruments.size()]),
      queue_(cfg.queue_conservatism_bps),
      touch_(new QueueTouch[instruments.size() == 0 ? 1 : instruments.size()]),
      tape_(new TradeTape[instruments.size() == 0 ? 1 : instruments.size()]) {
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
    links_[n_links_++].emplace(VenueId{v}, vc, seed, cfg);
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
      for (std::vector<Level>& v : f.levels) v.reserve(16);
      f.stale.reserve(16);
      f.dirty.reserve(16);
    }
    own_pending_.reserve(n);
    own_buf_ = std::make_unique<EventBuf>();
  }
  me_.set_stp(kStrategyAccount, cfg.stp);
  for (Link*& l : link_of_inst_) l = &at(0);
  for (const Instrument& i : instruments) {
    const VenueId v = venue_of(i);
    link_of_inst_[i.id.value] = &at(index_of[v.value]);
    me_.set_stp(kStrategyAccount, i.id, cfg.venue_config(v).stp);
  }
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
  agg_ = std::make_unique<MdAggregator>(instruments_.size(), me_, mc, start);
  own_feed_.reset();  // the aggregated book holds our orders
  own_pending_.clear();
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
  OutSlot slot{};
  slot.len = m.len;
  slot.over_limit = over_limit ? 1 : 0;
  std::memcpy(slot.bytes, &m, m.len);
  Timestamp arrival = now + s.delay;
  // One connection's messages are taken in one after another (SimVenueConfig::order_service).
  if (l.order_service.ns > 0 && l.last_order_processed.valid() &&
      arrival < l.last_order_processed + l.order_service)
    arrival = l.last_order_processed + l.order_service;
  if (!sched_.push(arrival, slot)) {
    ++stats_.scheduler_full;
    if (observer_ != nullptr) observer_->on_order_sent(m, now, Timestamp{});
    return false;
  }
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
  if (own_feed_ != nullptr) flush_own(now);
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
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_new(n, now);
  } else {
    static_cast<void>(me_.submit(n, now));  // effects arrive through the sink
  }
}

void SimTransport::venue_cancel(const OutCancelMsg& m, Timestamp now) noexcept {
  // A cancel of an order the venue does not hold is refused on the link it came in on.
  if (order_venues_ != nullptr && !order_venues_->contains(m.cl_ord_id.value))
    note_order(m.cl_ord_id, link_for(m.hdr));
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_cancel(m.cl_ord_id, m.hdr.instrument, now);
  } else {
    static_cast<void>(me_.cancel(kStrategyAccount, m.cl_ord_id, now));
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
    if (cfg_.fill_model == FillModel::L2Queue) {
      queue_cancel(m.orig_cl_ord_id, m.hdr.instrument, now);
    } else {
      static_cast<void>(me_.cancel(kStrategyAccount, m.orig_cl_ord_id, now));
    }
    if (closed) {
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::VenueReject, now, kReduceOnlyCode);
    } else {
      emit_reject(m.cl_ord_id, m.hdr.instrument, RejectReason::InsufficientBalance, now);
    }
    return;
  }
  if (cfg_.fill_model == FillModel::L2Queue) {
    queue_replace(m, now);
  } else {
    static_cast<void>(
        me_.replace(kStrategyAccount, m.orig_cl_ord_id, m.cl_ord_id, m.price, m.qty, now));
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
  if (cfg_.fill_model == FillModel::L2Queue) {
    for_each_venue_level(mirror_[id.value], touch_[id.value], opp, now, take);
  } else {
    const SimBook::Levels& levels = me_.book(id).levels(opp);
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
    if (!l->order_wire.try_push(&m.hdr, m.hdr.len)) ++stats_.wire_full;
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
      if (!l->order_wire.try_push(&m.hdr, m.hdr.len)) ++stats_.wire_full;
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
        if (cfg_.fill_model == FillModel::L2Queue) {
          queue_on_trade(msg_cast<TradeMsg>(&md), now);
        } else {
          mirror_on_trade(msg_cast<TradeMsg>(&md), now);
        }
        break;
      case EventType::BookTicker:
        if (cfg_.fill_model == FillModel::L2Queue)
          queue_on_ticker(msg_cast<BookTickerMsg>(&md), now);
        break;
      default:
        break;
    }
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
  // Coupled mode publishes its own view of the book; a venue's mark and funding go through as
  // recorded.
  if (agg_ != nullptr && md.type != EventType::PerpState) {
    if (md.seq != 0) forward_in_order(nullptr, md.seq, md.recv_ts, now);
    return;
  }
  const EventHeader& out =
      own_feed_ != nullptr && id.value < instruments_.size() ? with_own(md) : md;
  if (md.type == EventType::BookTicker && hides_level(msg_cast<BookTickerMsg>(&out))) {
    // A recorded ticker one side of which was only the session's own orders (strip_own): the venue
    // took its other side; the engine, with none of ours there, would see a level nobody holds.
    if (md.seq != 0) forward_in_order(nullptr, md.seq, md.recv_ts, now);
    if (own_feed_ != nullptr) flush_own(now);
    return;
  }
  if (md.seq != 0) {
    forward_in_order(&out, md.seq, md.recv_ts, now);
  } else {
    forward(out, md.recv_ts, now);
  }
  if (own_feed_ != nullptr) flush_own(now);
}

bool SimTransport::hides_level(const BookTickerMsg& m) noexcept {
  return (m.bid_px.is_positive() && m.bid_qty.is_zero()) ||
         (m.ask_px.is_positive() && m.ask_qty.is_zero());
}

void SimTransport::grow_ahead(std::uint64_t seq) {
  std::size_t cap = ahead_.empty() ? 1024 : ahead_.size();
  while (cap < kMaxAhead && seq - next_forward_ >= cap) cap *= 2;
  if (cap == ahead_.size()) return;
  std::vector<Ahead> next(cap);
  for (Ahead& a : ahead_) {
    if (a.seq != 0) next[a.seq & (cap - 1)] = std::move(a);
  }
  ahead_ = std::move(next);
}

void SimTransport::forward_in_order(const EventHeader* md,
                                    std::uint64_t seq,
                                    Timestamp recorded,
                                    Timestamp now) noexcept {
  const auto after = [](const Waiting& a, const Waiting& b) { return a.seq > b.seq; };
  if (seq > next_forward_) {
    if (seq - next_forward_ >= ahead_.size() && ahead_.size() < kMaxAhead) grow_ahead(seq);
    if (seq - next_forward_ < ahead_.size()) {
      Ahead& a = ahead_[seq & (ahead_.size() - 1)];
      if (a.seq == 0) {
        a.seq = seq;
        a.recorded = recorded;
        a.venue = now;
        a.has_md = md != nullptr;
        if (md != nullptr) {
          const std::size_t words = (md->len + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
          if (a.bytes.size() < words) a.bytes.resize(words);
          std::memcpy(a.bytes.data(), md, md->len);
        }
        return;
      }
    }
    std::uint32_t slot = kNoSlot;
    if (md != nullptr) {
      if (waiting_free_.empty()) {
        waiting_bytes_.emplace_back();
        slot = static_cast<std::uint32_t>(waiting_bytes_.size() - 1);
      } else {
        slot = waiting_free_.back();
        waiting_free_.pop_back();
      }
      std::vector<std::uint64_t>& b = waiting_bytes_[slot];
      const std::size_t words = (md->len + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
      if (b.size() < words) b.resize(words);
      std::memcpy(b.data(), md, md->len);
    }
    waiting_.push_back(Waiting{seq, recorded, now, slot});
    std::push_heap(waiting_.begin(), waiting_.end(), after);
    return;
  }
  if (md != nullptr) forward(*md, recorded, now);
  if (seq == next_forward_) ++next_forward_;
  // Every position waiting is past next_forward_ until it moves on: the next to go is at it.
  for (;;) {
    if (!ahead_.empty()) {
      Ahead& a = ahead_[next_forward_ & (ahead_.size() - 1)];
      if (a.seq == next_forward_) {
        a.seq = 0;
        if (a.has_md) {
          forward(*reinterpret_cast<const EventHeader*>(a.bytes.data()), a.recorded, a.venue);
        }
        ++next_forward_;
        continue;
      }
    }
    if (waiting_.empty() || waiting_.front().seq > next_forward_) break;
    std::pop_heap(waiting_.begin(), waiting_.end(), after);
    const Waiting w = waiting_.back();
    waiting_.pop_back();
    if (w.slot != kNoSlot) {
      forward(*reinterpret_cast<const EventHeader*>(waiting_bytes_[w.slot].data()),
              w.recorded,
              w.venue);
      waiting_free_.push_back(w.slot);
    }
    if (w.seq == next_forward_) ++next_forward_;
  }
}

void SimTransport::forward(const EventHeader& md, Timestamp recorded, Timestamp now) noexcept {
  // Forward a copy with the arrival stamp, on the wire of the instrument's venue.
  Link& l = link(md.instrument);
  std::byte* p = l.md_wire.try_reserve(md.len);
  if (p == nullptr) {
    ++stats_.wire_full;
    return;
  }
  std::memcpy(p, &md, md.len);
  auto* h = reinterpret_cast<EventHeader*>(p);
  h->exch_ts = now;
  Timestamp arrival = now + l.lat.md_in();
  if (l.md_recorded_arrival && recorded > now) arrival = arrival + (recorded - now);
  if (arrival < l.last_md_arrival) arrival = l.last_md_arrival;
  l.last_md_arrival = arrival;
  h->recv_ts = arrival;
  h->t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  h->t1_delta = h->t2_delta = 0;
  h->seq = 0;
  l.md_wire.commit();
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
  static_cast<void>(me_.submit(n, now));  // effects arrive through the sink
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
    queue_.remove(h);
  } else if (o.leaves().is_zero()) {
    queue_.remove(h);
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
  const Qty ahead = queue_at_placement(
      level_qty(book, n.side, n.price), n.side, n.price, book, touch, &tape_[id.value]);
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
  queue_.get(h).cum_qty = cum;
  queue_.get(h).crossed = cum;
  note_own(id, n.side, n.price);
}

void SimTransport::queue_cancel(ClientOrderId id, InstrumentId route, Timestamp now) noexcept {
  const auto h = queue_.find(id);
  if (!h.valid()) {
    emit_cancel_reject(id, InstrumentId{}, now, route);
    return;
  }
  const QueuedOrder o = queue_.get(h);
  queue_.remove(h);
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
  queue_.remove(h);
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

void SimTransport::queue_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept {
  QueuePositionModel* const models[] = {&queue_};
  const std::size_t i = d.hdr.instrument.value;
  queue_apply_book(mirror_[i], d, now, &tape_[i], models);
  queue_cross(d.hdr.instrument, now);
}

void SimTransport::queue_on_ticker(const BookTickerMsg& m, Timestamp now) noexcept {
  const InstrumentId id = m.hdr.instrument;
  QueueTouch& t = touch_[id.value];
  t = queue_touch(m, [](Side, Price) { return Qty{}; });  // the replayed feed has none of ours
  QueuePositionModel* const models[] = {&queue_};
  static_cast<void>(queue_apply_touch(mirror_[id.value], id, t, &tape_[id.value], models));
  queue_cross(id, now);
}

void SimTransport::queue_on_trade(const TradeMsg& t, Timestamp now) noexcept {
  const InstrumentId id = t.hdr.instrument;
  queue_.on_trade(id,
                  t.price,
                  t.qty,
                  t.aggressor,
                  now,
                  [&](QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty ahead) {
                    queue_fill(h, o, fill, ahead, now);
                  });
  tape_[id.value].add(t, now);
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
  m.venue_order_id = decimal_id(order_id);
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
  m.venue_order_id = decimal_id(order_id);
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
  m.venue_order_id = decimal_id(order_id);
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
  m.venue_order_id = decimal_id(order_id);
  m.exec_id = decimal_id(exec_id);
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
    if (!l.user_wire.try_push(&h, h.len)) ++stats_.wire_full;
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
  if (!l.order_wire.try_push(&h, h.len)) ++stats_.wire_full;
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
    if (!l.user_wire.try_push(&m.hdr, m.hdr.len)) ++stats_.wire_full;
    return;
  }
  const Timestamp arrival = std::max(l.last_order_arrival, m.hdr.exch_ts);
  l.last_order_arrival = arrival;
  m.hdr.recv_ts = arrival;
  m.hdr.t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
  if (!l.order_wire.try_push(&m.hdr, m.hdr.len)) ++stats_.wire_full;
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
  if (!l.md_wire.try_push(&h, h.len)) {
    ++stats_.wire_full;
    return;
  }
  ++stats_.md_forwarded;
}

// ---- our orders in the recorded feed -----------------------------------------------------------

void SimTransport::note_own(InstrumentId id, Side side, Price px) noexcept {
  if (own_feed_ == nullptr || id.value >= instruments_.size() || !px.is_positive()) return;
  OwnFeed& f = own_feed_[id.value];
  const std::pair<Side, Price> key{side, px};
  if (std::find(f.stale.begin(), f.stale.end(), key) == f.stale.end()) f.stale.push_back(key);
  if (std::find(f.dirty.begin(), f.dirty.end(), key) == f.dirty.end()) f.dirty.push_back(key);
  if (!f.pending) {
    f.pending = true;
    own_pending_.push_back(id);
  }
}

Qty SimTransport::model_own_at(InstrumentId id, Side side, Price px) const noexcept {
  if (cfg_.fill_model == FillModel::Matching)
    return me_.account_qty_at(kStrategyAccount, id, side, px);
  Qty sum{};
  queue_.for_each(id, [&](QueuePositionModel::Handle32, const QueuedOrder& o) {
    if (o.side == side && o.price == px) sum += o.leaves();
  });
  return sum;
}

void SimTransport::refresh_own(InstrumentId id, OwnFeed& f) noexcept {
  for (const auto& [side, px] : f.stale) {
    std::vector<Level>& v = f.levels[static_cast<std::size_t>(side)];
    const Qty q = model_own_at(id, side, px);
    auto it = std::find_if(v.begin(), v.end(), [&](const Level& l) { return l.price == px; });
    if (it != v.end()) {
      if (q.is_positive()) {
        it->qty = q;
      } else {
        v.erase(it);
      }
    } else if (q.is_positive()) {
      v.push_back(Level{px, q});
    }
  }
  f.stale.clear();
}

namespace {
Qty qty_at(const std::vector<Level>& v, Price px) noexcept {
  for (const Level& l : v) {
    if (l.price == px) return l.qty;
  }
  return Qty{};
}
Level best_level(const std::vector<Level>& v, Side side) noexcept {
  Level b{};
  for (const Level& l : v) {
    if (b.qty.is_zero() || better(side, l.price, b.price)) b = l;
  }
  return b;
}
}  // namespace

Level SimTransport::recorded_top(InstrumentId id, Side side) const noexcept {
  const OwnFeed& f = own_feed_[id.value];
  const L2Book<256>& book = mirror_[id.value];
  const bool buy = side == Side::Buy;
  if (f.tickers && f.ticker_newer) return buy ? f.recorded_bid : f.recorded_ask;
  return buy ? book.best_bid() : book.best_ask();
}

Qty SimTransport::shown_own(InstrumentId id, Side side, Price px) const noexcept {
  const Level opp = recorded_top(id, opposite(side));
  if (opp.qty.is_positive() && (side == Side::Buy ? px >= opp.price : px <= opp.price))
    return Qty{};
  return qty_at(own_feed_[id.value].levels[static_cast<std::size_t>(side)], px);
}

Level SimTransport::shown_top(InstrumentId id, Side side) const noexcept {
  const OwnFeed& f = own_feed_[id.value];
  Level top = recorded_top(id, side);
  Level own{};
  for (const Level& l : f.levels[static_cast<std::size_t>(side)]) {
    if (!shown_own(id, side, l.price).is_positive()) continue;
    if (own.qty.is_zero() || better(side, l.price, own.price)) own = l;
  }
  if (own.qty.is_zero()) return top;
  if (top.qty.is_zero() || better(side, own.price, top.price)) return own;
  if (own.price == top.price) top.qty += own.qty;
  return top;
}

const EventHeader& SimTransport::with_own(const EventHeader& md) noexcept {
  const InstrumentId id = md.instrument;
  OwnFeed& f = own_feed_[id.value];
  switch (md.type) {
    case EventType::BookTicker: {
      const auto& m = msg_cast<BookTickerMsg>(&md);
      f.tickers = true;
      f.ticker_newer = true;
      f.recorded_bid = Level{m.bid_px, m.bid_qty};
      f.recorded_ask = Level{m.ask_px, m.ask_qty};
      refresh_own(id, f);
      auto& out = own_buf_->as<BookTickerMsg>();
      std::memcpy(&out, &m, sizeof m);
      const Level bid = shown_top(id, Side::Buy);
      const Level ask = shown_top(id, Side::Sell);
      out.bid_px = bid.price;
      out.bid_qty = bid.qty;
      out.ask_px = ask.price;
      out.ask_qty = ask.qty;
      f.sent_bid = bid;
      f.sent_ask = ask;
      return out.hdr;
    }
    case EventType::BookDelta:
    case EventType::BookSnapshot: {
      f.ticker_newer = false;
      refresh_own(id, f);
      const auto& d = msg_cast<BookDeltaMsg>(&md);
      const bool snapshot = d.is_snapshot();
      if (f.dirty.empty() && f.levels[0].empty() && f.levels[1].empty()) return md;
      auto& out = own_buf_->as<BookDeltaMsg>();
      std::memcpy(&out, &d, sizeof(BookDeltaMsg));
      constexpr std::size_t kCap = (kMaxSourceEventBytes - sizeof(BookDeltaMsg)) / sizeof(Level);
      Level* lv = out.levels();
      std::uint32_t count[2] = {0, 0};
      std::size_t n = 0;
      for (const Side side : {Side::Buy, Side::Sell}) {
        const auto k = static_cast<std::size_t>(side);
        const std::vector<Level>& own = f.levels[k];
        const std::size_t first = n;
        for (const Level& l : side == Side::Buy ? d.bids() : d.asks()) {
          Level x = l;
          const Qty q = shown_own(id, side, l.price);
          if (q.is_positive()) {
            x.qty += q;
            ++stats_.own_levels;
          }
          if (n < kCap) lv[n++] = x;
        }
        const auto listed = [&](Price px) {
          for (std::size_t i = first; i < n; ++i) {
            if (lv[i].price == px) return true;
          }
          return false;
        };
        const auto add = [&](Price px) {
          if (listed(px) || n >= kCap) return;
          const Qty total = level_qty(mirror_[id.value], side, px) + shown_own(id, side, px);
          if (snapshot && total.is_zero()) return;
          lv[n++] = Level{px, total};
          ++stats_.own_levels;
        };
        // Our levels that changed since the last depth update; a snapshot lists all of ours.
        for (const auto& [s, px] : f.dirty) {
          if (s == side) add(px);
        }
        if (snapshot) {
          for (const Level& l : own) add(l.price);
        }
        count[k] = static_cast<std::uint32_t>(n - first);
      }
      f.dirty.clear();
      out.bid_count = count[0];
      out.ask_count = count[1];
      out.hdr.len = BookDeltaMsg::size_for(count[0], count[1]);
      return out.hdr;
    }
    default:
      return md;
  }
}

void SimTransport::flush_own(Timestamp now) noexcept {
  for (const InstrumentId id : own_pending_) {
    OwnFeed& f = own_feed_[id.value];
    f.pending = false;
    refresh_own(id, f);
    if (!f.tickers) continue;
    const Level bid = shown_top(id, Side::Buy);
    const Level ask = shown_top(id, Side::Sell);
    if (bid.price == f.sent_bid.price && bid.qty == f.sent_bid.qty &&
        ask.price == f.sent_ask.price && ask.qty == f.sent_ask.qty)
      continue;
    // The venue's real-time top of book changed with our order: its ticker stream says so. No
    // update id of the venue's own: a strategy compares it with the depth book by time. Flagged
    // synthetic: strip_own drops it from this run's journal.
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, id, link(id).id);
    m.hdr.flags |= EventHeader::kSynthetic;
    m.bid_px = bid.price;
    m.bid_qty = bid.qty;
    m.ask_px = ask.price;
    m.ask_qty = ask.qty;
    f.sent_bid = bid;
    f.sent_ask = ask;
    ++stats_.own_tickers;
    push_md_wire(m.hdr, now);
  }
  own_pending_.clear();
}

// ---- engine side -------------------------------------------------------------------------------

Timestamp SimTransport::head_ts(MsgRing& ring) noexcept {
  const std::byte* p = ring.try_peek();
  return p == nullptr ? Timestamp::max() : reinterpret_cast<const EventHeader*>(p)->recv_ts;
}

Timestamp SimTransport::next_inbound_ts() noexcept {
  Timestamp t = Timestamp::max();
  for (std::size_t k = 0; k < n_links_; ++k)
    t = std::min({t, head_ts(at(k).order_wire), head_ts(at(k).user_wire), head_ts(at(k).md_wire)});
  return t;
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
  MsgRing* ord = nullptr;
  Link* md = nullptr;
  Timestamp a = Timestamp::max();
  Timestamp b = Timestamp::max();
  for (std::size_t k = 0; k < n_links_; ++k) {
    Link& l = at(k);
    if (const Timestamp ts = head_ts(l.order_wire); ts < a) {
      a = ts;
      ord = &l.order_wire;
    }
    if (const Timestamp ts = head_ts(l.user_wire); ts < a) {
      a = ts;
      ord = &l.user_wire;
    }
    if (const Timestamp ts = head_ts(l.md_wire); ts < b) {
      b = ts;
      md = &l;
    }
  }
  EventType t = EventType::Padding;
  if (ord != nullptr && a <= b) {
    if (move_head(*ord, feed, t)) ++stats_.order_events_delivered;
  } else if (md != nullptr) {
    if (move_head(md->md_wire, feed, t)) ++stats_.md_delivered;
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

Price SimTransport::venue_best(InstrumentId id, Side side) const noexcept {
  if (id.value >= instruments_.size()) return Price{};
  const L2Book<256>& mirror = mirror_[id.value];
  const Level m = side == Side::Buy ? mirror.best_bid() : mirror.best_ask();
  if (cfg_.fill_model == FillModel::L2Queue ||
      (agg_ == nullptr && me_.book(id).depth(Side::Buy) == 0)) {
    return m.price;
  }
  const MatchingEngine::TopOfBook top = me_.top_of_book(id);
  const Level& t = side == Side::Buy ? top.bid : top.ask;
  return t.qty.is_zero() ? m.price : t.price;
}

MatchingEngine::SideExposure SimTransport::strategy_exposure(InstrumentId id,
                                                             Side side) const noexcept {
  if (cfg_.fill_model == FillModel::Matching) return me_.exposure(kStrategyAccount, id, side);
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
