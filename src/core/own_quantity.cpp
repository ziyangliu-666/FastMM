#include "fastmm/core/own_quantity.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace fastmm {

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kForever = std::numeric_limits<std::int64_t>::max();

Qty less(Qty a, Qty b) noexcept {
  return b >= a ? Qty{} : a - b;
}

// Venue time of an event: exch_ts, else recv_ts.
Timestamp venue_ts(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

}  // namespace

void VenueTime::offer(const EventHeader& h) noexcept {
  if (h.exch_ts.valid() && (from_recv || !ts.valid())) {
    ts = h.exch_ts;
    from_recv = false;
  } else if (!ts.valid()) {
    ts = h.recv_ts;
    from_recv = true;
  }
}

std::uint64_t numeric_exec_id(const ExecId& id) noexcept {
  const std::string_view v = id.view();
  std::uint64_t n = 0;
  const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  return ec == std::errc{} && ptr == v.data() + v.size() ? n : 0;
}

// Segments of one price level sorted by start, with the running maximum of their ends: a lookup
// walks back from the last segment starting at or before t while one may still cover t.
// The levels sit in one open-addressing table (most lookups are of a level we never quoted, a
// miss in one or two probes) and their segments in flat arrays, a level's contiguous.
struct OwnQuantity::Index {
  struct Entry {
    std::int64_t px = 0;
    std::uint32_t inst_side = 0;  // inst << 1 | side
    std::uint32_t level = 0;      // 1 + index into `first`, 0: empty
  };
  [[nodiscard]] static std::uint64_t mix(std::uint32_t inst_side, std::int64_t px) noexcept {
    std::uint64_t x = static_cast<std::uint64_t>(px) * 0x9E37'79B9'7F4A'7C15ULL;
    x ^= static_cast<std::uint64_t>(inst_side) + 0x632B'E59B'D9B4'E019ULL + (x << 6U);
    return x ^ (x >> 29U);
  }
  [[nodiscard]] static std::uint64_t hash(std::uint32_t inst_side, std::int64_t px) noexcept {
    return mix(inst_side, px) * 0xBF58'476D'1CE4'E5B9ULL >> 32U;
  }
  // A bit per (level hash mod the filter's size): a level we never quoted, nearly every lookup,
  // is told apart in one load from a bit array that stays in cache.
  [[nodiscard]] std::size_t filter_bit(std::uint32_t inst_side, std::int64_t px) const noexcept {
    return static_cast<std::size_t>(mix(inst_side, px) * 0x94D0'49BB'1331'11EBULL >> filter_shift);
  }
  [[nodiscard]] bool maybe(std::uint32_t is, std::int64_t px) const noexcept {
    const std::size_t b = filter_bit(is, px);
    return ((filter[b / 64] >> (b % 64)) & 1U) != 0;
  }
  [[nodiscard]] static std::uint32_t inst_side(std::uint32_t inst, Side side) noexcept {
    return inst << 1U | static_cast<std::uint32_t>(side);
  }
  // The level's index, or -1.
  [[nodiscard]] std::int64_t find(std::uint32_t is, std::int64_t px) const noexcept {
    if (table.empty() || !maybe(is, px)) return -1;
    for (std::size_t i = hash(is, px) & mask;; i = (i + 1) & mask) {
      const Entry& e = table[i];
      if (e.level == 0) return -1;
      if (e.px == px && e.inst_side == is) return static_cast<std::int64_t>(e.level) - 1;
    }
  }
  std::vector<Entry> table;
  std::size_t mask = 0;
  std::vector<std::uint64_t> filter;
  unsigned filter_shift = 64;
  std::vector<std::uint32_t> first;  // per level, its segments [first[l], first[l + 1])
  std::vector<std::int64_t> starts;  // sorted by start within a level
  std::vector<std::int64_t> ends;    // running maximum of the segments' ends
  std::vector<std::int64_t> seg_end;
  std::vector<Qty> qty;
};

OwnQuantity::OwnQuantity(bool keep_all, Duration retention)
    : keep_all_(keep_all),
      retention_(retention),
      lives_(std::make_unique<Pool<Life, kMaxOpenOrders>>()),
      by_id_(std::make_unique<OpenHashMap<ClientOrderId, Handle<Life>, kMaxOpenOrders * 2>>()) {
  scratch_.reserve(kMaxOpenOrders);
}

void OwnQuantity::warm_up() noexcept {
  lives_->warm_up();
}

OwnQuantity::~OwnQuantity() = default;

template <class F>
void OwnQuantity::for_each_segment(const Life& s, F&& f) noexcept {
  if (!s.acked || !s.resting) return;
  const std::int64_t start = s.ack.ts.ns;
  const std::int64_t stop = s.stop;
  if (stop <= start) return;
  std::int64_t from = start;
  Qty leaves = s.qty;
  for (std::uint32_t i = 0; i < s.nfills; ++i) {
    const std::int64_t at = std::clamp(s.fills[i].at.ts.ns, start, stop);
    if (at > from && leaves.is_positive()) f(from, at, leaves);
    from = std::max(from, at);
    leaves = less(leaves, s.fills[i].qty);
  }
  if (stop > from && leaves.is_positive()) f(from, stop, leaves);
}

bool OwnQuantity::empty() const noexcept {
  if (index_) return index_->starts.empty();
  bool none = dropped_.empty();
  lives_->for_each([&](Handle<Life>, const Life& s) {
    for_each_segment(s, [&](std::int64_t, std::int64_t, Qty) { none = false; });
  });
  return none;
}

OwnQuantity::Life* OwnQuantity::find(ClientOrderId id) noexcept {
  const Handle<Life>* h = by_id_->find(id);
  return h == nullptr ? nullptr : &lives_->get(*h);
}

OwnQuantity::Life* OwnQuantity::add(const Life& l) noexcept {
  if (const Handle<Life>* h = by_id_->find(l.id)) {
    Life& s = lives_->get(*h);
    if (s.inst != l.inst) {
      by_inst_.clear(InstBits::bucket(s.inst.value), h->idx);
      by_inst_.set(InstBits::bucket(l.inst.value), h->idx);
    }
    s = l;
    return &s;
  }
  if (lives_->full()) {
    // The oldest order that no longer changes what the feed shows: never acknowledged (lost, or
    // refused by the transport), or already over.
    Handle<Life> oldest{};
    std::uint64_t born = std::numeric_limits<std::uint64_t>::max();
    lives_->for_each([&](Handle<Life> h, const Life& s) {
      if ((!s.acked || s.ended) && s.born < born) {
        born = s.born;
        oldest = h;
      }
    });
    if (!oldest.valid()) return nullptr;
    ++stats_.evicted;
    drop(lives_->get(oldest).id);
  }
  const Handle<Life> h = lives_->allocate();
  if (!h.valid() || by_id_->insert(l.id, h).first == nullptr) {
    if (h.valid()) lives_->free(h);
    return nullptr;
  }
  Life& s = lives_->get(h);
  s = l;
  s.born = born_++;
  by_inst_.set(InstBits::bucket(s.inst.value), h.idx);
  return &s;
}

void OwnQuantity::drop(ClientOrderId id) noexcept {
  const Handle<Life>* h = by_id_->find(id);
  if (h == nullptr) return;
  const Handle<Life> hh = *h;
  const Life& s = lives_->get(hh);
  if (keep_all_) {
    for_each_segment(s, [&](std::int64_t a, std::int64_t b, Qty q) {
      dropped_.push_back(Segment{a, b, q, s.px, s.inst, s.side});
    });
  }
  by_inst_.clear(InstBits::bucket(s.inst.value), hh.idx);
  by_id_->erase(id);
  lives_->free(hh);
}

void OwnQuantity::on_outbound(const EventHeader& h) noexcept {
  Life s{};
  s.inst = h.instrument;
  s.venue = h.venue;
  if (h.type == EventType::OutNewOrder) {
    const auto& m = msg_cast<OutNewOrderMsg>(&h);
    s.id = m.cl_ord_id;
    s.side = m.side;
    s.resting =
        m.type != OrderType::Market && m.tif != TimeInForce::Ioc && m.tif != TimeInForce::Fok;
    s.px = m.price;
    s.qty = m.qty;
  } else if (h.type == EventType::OutReplace) {
    const auto& m = msg_cast<OutReplaceMsg>(&h);
    s.id = m.cl_ord_id;
    s.px = m.price;
    s.qty = m.qty;
    s.replaces = m.orig_cl_ord_id;
    s.resting = true;
    if (Life* orig = find(m.orig_cl_ord_id)) {
      s.side = orig->side;
      orig->replaced_by = m.cl_ord_id;
    }
  } else {
    return;
  }
  ++stats_.orders;
  static_cast<void>(add(s));
}

void OwnQuantity::on_inbound(const EventHeader& h) noexcept {
  switch (h.type) {
    case EventType::OrderAck:
    case EventType::OrderFill:
    case EventType::OrderCancelAck:
    case EventType::OrderExpired:
      if (h.exch_ts.valid()) {
        ++venue_times_;
        if (h.exch_ts.ns % kMs != 0) ms_ = false;
      }
      break;
    case EventType::OrderReject:
    case EventType::Reconcile:
      break;
    default:
      return;
  }
  const Timestamp vts = venue_ts(h);
  if (vts > latest_) latest_ = vts;
  switch (h.type) {
    case EventType::OrderAck:
      on_ack(h, msg_cast<OrderAckMsg>(&h).cl_ord_id);
      break;
    case EventType::OrderFill:
      on_fill(h, msg_cast<OrderFillMsg>(&h));
      break;
    case EventType::OrderCancelAck:
      on_cancel_ack(h, msg_cast<OrderCancelAckMsg>(&h).cl_ord_id);
      break;
    case EventType::OrderExpired:
      if (Life* s = find(msg_cast<OrderExpiredMsg>(&h).cl_ord_id)) end(*s, h);
      break;
    case EventType::OrderReject:
      on_reject(msg_cast<OrderRejectMsg>(&h).cl_ord_id);
      break;
    case EventType::Reconcile:
      on_reconcile(h, msg_cast<ReconcileMsg>(&h));
      break;
    default:
      break;
  }
  if (!keep_all_ && latest_.ns - last_gc_ >= retention_.ns / 2) gc(latest_);
}

void OwnQuantity::on_ack(const EventHeader& h, ClientOrderId id) noexcept {
  Life* s = find(id);
  if (s == nullptr) {
    ++stats_.unknown_acks;
    return;
  }
  const Timestamp before = s->ack.ts;
  s->ack.offer(h);
  Life* orig = s->replaces.valid() ? find(s->replaces) : nullptr;
  if (s->acked) {
    if (s->ack.ts != before) {
      rebuild(*s);
      if (orig != nullptr) rebuild(*orig);
    }
    return;
  }
  s->acked = true;
  rebuild(*s);
  if (orig != nullptr) {
    if (orig->ended) {
      rebuild(*orig);  // it rests until this ack when that is later than its end
    } else {
      end(*orig, h, true);
    }
  }
}

void OwnQuantity::on_fill(const EventHeader& h, const OrderFillMsg& m) noexcept {
  Life* s = find(m.cl_ord_id);
  if (s == nullptr) return;
  Qty filled;
  for (std::uint32_t i = 0; i < s->nfills; ++i) filled += s->fills[i].qty;
  const std::uint64_t exec = numeric_exec_id(m.exec_id);
  bool seen = false;
  for (std::uint32_t i = 0; exec != 0 && i < s->nfills; ++i)
    seen = seen || s->fills[i].exec == exec;
  if (!s->ended && !seen) {
    Fill f{};
    f.exec = exec;
    f.at.offer(h);
    f.qty = m.qty;
    if (s->nfills < kMaxFills) {
      s->fills[s->nfills++] = f;
    } else {
      ++stats_.merged_fills;
      Fill& last = s->fills[kMaxFills - 1];
      last.qty += f.qty;
      last.at = f.at;
      last.exec = exec;
    }
    filled += m.qty;
  }
  const bool last =
      (m.flags & OrderFillMsg::kReplayed) == 0 ? m.leaves_qty.is_zero() : m.cum_qty >= s->qty;
  if (last || filled >= s->qty) {
    end(*s, h);
  } else {
    rebuild(*s);
  }
}

void OwnQuantity::on_cancel_ack(const EventHeader& h, ClientOrderId id) noexcept {
  Life* s = find(id);
  if (s == nullptr) return;
  // A second cancel ack may carry the venue time the first did not.
  if (s->ended) {
    if (!s->cancelled) return;
    const Timestamp before = s->end.ts;
    s->end.offer(h);
    if (s->end.ts != before) rebuild(*s);
    return;
  }
  end(*s, h, true);
}

void OwnQuantity::on_reject(ClientOrderId id) noexcept {
  Life* s = find(id);
  if (s == nullptr || s->acked || s->rejected) return;
  drop(id);  // it never rested
}

// Begin, OpenOrder..., End for one venue: an acked order the snapshot does not list is gone. With a
// sent watermark, orders above it were not yet sent when the snapshot was taken.
void OwnQuantity::on_reconcile(const EventHeader& h, const ReconcileMsg& m) noexcept {
  switch (m.kind) {
    case ReconcileMsg::Kind::Begin:
      reconcile_venue_ = h.venue;
      watermark_ =
          (m.flags & ReconcileMsg::kSentWatermark) != 0 ? m.sent_watermark : ClientOrderId{};
      lives_->for_each([&](Handle<Life>, Life& s) {
        if (s.venue == h.venue) s.seen = false;
      });
      break;
    case ReconcileMsg::Kind::OpenOrder:
      if (Life* s = find(m.cl_ord_id)) {
        s->seen = true;
      } else if (m.orig_qty > m.cum_qty) {
        on_resting(m.cl_ord_id, h.instrument, h.venue, m.side, m.price, m.orig_qty - m.cum_qty);
      }
      break;
    case ReconcileMsg::Kind::End: {
      // Collected first, then ended in pool order.
      scratch_.clear();
      lives_->for_each([&](Handle<Life>, const Life& s) {
        if (s.venue != h.venue || !s.acked || s.ended || s.seen) return;
        if (watermark_.valid() && s.id.value > watermark_.value) return;
        scratch_.push_back(s.id);
      });
      for (const ClientOrderId id : scratch_) {
        if (Life* s = find(id)) end(*s, h);
      }
      break;
    }
    default:
      break;
  }
}

void OwnQuantity::on_resting(
    ClientOrderId id, InstrumentId inst, VenueId venue, Side side, Price px, Qty leaves) noexcept {
  if (!id.valid() || !inst.valid() || !px.is_positive() || !leaves.is_positive()) return;
  if (find(id) != nullptr) return;
  Life l{};
  l.id = id;
  l.inst = inst;
  l.venue = venue;
  l.side = side;
  l.resting = true;
  l.acked = true;
  l.seen = true;
  l.px = px;
  l.qty = leaves;
  l.ack.ts = Timestamp{1};
  l.ack.from_recv = false;
  if (Life* added = add(l)) rebuild(*added);
}

void OwnQuantity::on_gone(ClientOrderId id, Timestamp t) noexcept {
  Life* s = find(id);
  if (s == nullptr || s->ended) return;
  if (!s->acked) {
    drop(id);
    return;
  }
  EventHeader h{};
  h.exch_ts = t;
  end(*s, h);
}

void OwnQuantity::end(Life& s, const EventHeader& h, bool cancelled) noexcept {
  if (s.ended) return;
  s.ended = true;
  s.cancelled = cancelled;
  s.end.offer(h);
  rebuild(s);
}

// When the order stopped resting: its end, or its successor's ack when a replace took its place
// later, plus the rest of the end's millisecond with millisecond order times. Invalid while open.
Timestamp OwnQuantity::gone(const Life& s) noexcept {
  if (!s.ended) return Timestamp{};
  Timestamp t = s.end.ts;
  if (const Life* next = s.replaced_by.valid() ? find(s.replaced_by) : nullptr;
      next != nullptr && next->acked && next->ack.ts > t)
    t = next->ack.ts;
  if (ms_order_times() && !s.end.from_recv) t = Timestamp{t.ns + kMs - 1};
  return t;
}

// The order's ack, fills or end changed: when it stopped resting, which its segments end at.
void OwnQuantity::rebuild(Life& s) noexcept {
  const Timestamp g = gone(s);
  s.stop = g.valid() ? g.ns + 1 : kForever;  // gone is inclusive
}

// Forgets the orders that ended before the horizon and rested no later than it.
void OwnQuantity::gc(Timestamp now) noexcept {
  last_gc_ = now.ns;
  const std::int64_t horizon = now.ns - retention_.ns;
  scratch_.clear();
  lives_->for_each([&](Handle<Life>, const Life& s) {
    if (!s.ended || s.end.ts.ns >= horizon) return;
    bool rests = false;
    for_each_segment(s,
                     [&](std::int64_t, std::int64_t end, Qty) { rests = rests || end >= horizon; });
    if (!rests) scratch_.push_back(s.id);
  });
  for (const ClientOrderId id : scratch_) drop(id);
}

Qty OwnQuantity::own_at(InstrumentId inst, Side side, Price px, Timestamp t) const noexcept {
  if (index_) {
    const Index& x = *index_;
    const std::int64_t l = x.find(Index::inst_side(inst.value, side), px.raw);
    if (l < 0) return Qty{};
    const std::uint32_t lo = x.first[static_cast<std::size_t>(l)];
    const std::uint32_t hi = x.first[static_cast<std::size_t>(l) + 1];
    auto i = static_cast<std::size_t>(
        std::upper_bound(x.starts.begin() + lo, x.starts.begin() + hi, t.ns) - x.starts.begin());
    Qty q;
    while (i > lo && x.ends[i - 1] > t.ns) {
      --i;
      if (x.seg_end[i] > t.ns) q += x.qty[i];
    }
    return q;
  }
  Qty q;
  by_inst_.for_each(InstBits::bucket(inst.value), [&](std::uint32_t slot) {
    const Life& s = lives_->get(Handle<Life>{slot});
    if (s.px != px || s.side != side || s.inst != inst) return;
    for_each_segment(s, [&](std::int64_t start, std::int64_t end, Qty leaves) {
      if (start <= t.ns && t.ns < end) q += leaves;
    });
  });
  for (const Segment& g : dropped_) {
    if (g.inst == inst && g.side == side && g.px == px && g.start <= t.ns && t.ns < g.end)
      q += g.qty;
  }
  return q;
}

void OwnQuantity::index() {
  index_ = std::make_unique<Index>();
  Index& x = *index_;
  // Levels in order of their first segment, each level's segments in recorded order.
  struct Key {
    std::uint32_t inst_side;
    std::int64_t px;
    bool operator==(const Key&) const noexcept = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept { return Index::hash(k.inst_side, k.px); }
  };
  std::vector<Segment> all = dropped_;
  lives_->for_each([&](Handle<Life>, const Life& s) {
    for_each_segment(s, [&](std::int64_t a, std::int64_t b, Qty q) {
      all.push_back(Segment{a, b, q, s.px, s.inst, s.side});
    });
  });
  std::unordered_map<Key, std::uint32_t, KeyHash> ids;
  std::vector<std::vector<const Segment*>> levels;
  std::vector<Key> keys;
  for (const Segment& g : all) {
    const Key key{Index::inst_side(g.inst.value, g.side), g.px.raw};
    const auto [it, fresh] = ids.try_emplace(key, static_cast<std::uint32_t>(levels.size()));
    if (fresh) {
      levels.emplace_back();
      keys.push_back(key);
    }
    levels[it->second].push_back(&g);
  }
  std::size_t cap = 16;
  while (cap < levels.size() * 4) cap *= 2;
  x.table.assign(cap, Index::Entry{});
  x.mask = cap - 1;
  unsigned fk = 12;  // 2^fk filter bits, 16 or more per level
  while (fk < 26 && (std::size_t{1} << fk) < levels.size() * 16) ++fk;
  x.filter.assign((std::size_t{1} << fk) / 64, 0);
  x.filter_shift = 64 - fk;
  x.first.reserve(levels.size() + 1);
  for (std::size_t l = 0; l < levels.size(); ++l) {
    std::size_t i = Index::hash(keys[l].inst_side, keys[l].px) & x.mask;
    while (x.table[i].level != 0) i = (i + 1) & x.mask;
    x.table[i] = Index::Entry{keys[l].px, keys[l].inst_side, static_cast<std::uint32_t>(l + 1)};
    const std::size_t fb = x.filter_bit(keys[l].inst_side, keys[l].px);
    x.filter[fb / 64] |= 1ULL << (fb % 64);
    std::vector<const Segment*>& segs = levels[l];
    std::stable_sort(segs.begin(), segs.end(), [](const Segment* a, const Segment* b) {
      return a->start < b->start;
    });
    x.first.push_back(static_cast<std::uint32_t>(x.starts.size()));
    std::int64_t m = std::numeric_limits<std::int64_t>::min();
    for (const Segment* g : segs) {
      m = std::max(m, g->end);
      x.starts.push_back(g->start);
      x.ends.push_back(m);
      x.seg_end.push_back(g->end);
      x.qty.push_back(g->qty);
    }
  }
  x.first.push_back(static_cast<std::uint32_t>(x.starts.size()));
}

}  // namespace fastmm
