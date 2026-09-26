#include "fastmm/core/own_quantity.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

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
struct OwnQuantity::Index {
  struct Key {
    std::uint32_t inst;
    std::uint8_t side;
    std::int64_t px;
    bool operator==(const Key&) const noexcept = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
      std::uint64_t x = static_cast<std::uint64_t>(k.px) * 0x9E37'79B9'7F4A'7C15ULL;
      x ^= (static_cast<std::uint64_t>(k.inst) << 1U | k.side) + 0x632B'E59B'D9B4'E019ULL +
           (x << 6U);
      return x ^ (x >> 29U);
    }
  };
  struct Slot {
    std::vector<Segment> segs;
    std::vector<std::int64_t> ends;
  };
  std::unordered_map<Key, Slot, KeyHash> levels;
};

OwnQuantity::OwnQuantity(bool keep_all, Duration retention)
    : keep_all_(keep_all),
      retention_(retention),
      lives_(std::make_unique<Pool<Life, kMaxOpenOrders>>()),
      by_id_(std::make_unique<OpenHashMap<ClientOrderId, Handle<Life>, kMaxOpenOrders * 2>>()) {
  scratch_.reserve(kMaxOpenOrders);
}

void OwnQuantity::prepare(std::size_t instruments) {
  for (std::size_t i = 0; i < instruments; ++i)
    static_cast<void>(segs(InstrumentId{static_cast<std::uint32_t>(i)}));
}

OwnQuantity::~OwnQuantity() = default;

std::vector<OwnQuantity::Segment>& OwnQuantity::segs(InstrumentId id) {
  if (id.value >= by_inst_.size()) {
    by_inst_.resize(id.value + 1U);
    if (!keep_all_) {
      for (std::vector<Segment>& v : by_inst_) v.reserve(64);
    }
  }
  return by_inst_[id.value];
}

bool OwnQuantity::empty() const noexcept {
  if (index_) return index_->levels.empty();
  return std::all_of(
      by_inst_.begin(), by_inst_.end(), [](const std::vector<Segment>& v) { return v.empty(); });
}

OwnQuantity::Life* OwnQuantity::find(ClientOrderId id) noexcept {
  const Handle<Life>* h = by_id_->find(id);
  return h == nullptr ? nullptr : &lives_->get(*h);
}

OwnQuantity::Life* OwnQuantity::add(const Life& l) noexcept {
  if (Life* s = find(l.id)) {
    *s = l;
    return s;
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
  return &s;
}

void OwnQuantity::drop(ClientOrderId id) noexcept {
  const Handle<Life>* h = by_id_->find(id);
  if (h == nullptr) return;
  const Handle<Life> hh = *h;
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
      if (Life* s = find(m.cl_ord_id)) s->seen = true;
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

// The order's segments from its ack, fills and end: leaves step down at each fill.
void OwnQuantity::rebuild(Life& s) noexcept {
  std::vector<Segment>& v = segs(s.inst);
  std::erase_if(v, [&](const Segment& g) { return g.owner == s.id; });
  if (!s.acked || !s.resting) return;
  const std::int64_t start = s.ack.ts.ns;
  const Timestamp g = gone(s);
  const std::int64_t stop = g.valid() ? g.ns + 1 : kForever;  // gone is inclusive
  if (stop <= start) return;
  std::int64_t from = start;
  Qty leaves = s.qty;
  const auto emit = [&](std::int64_t a, std::int64_t b, Qty q) {
    v.push_back(Segment{a, b, q, s.px, s.id, s.side});
  };
  for (std::uint32_t i = 0; i < s.nfills; ++i) {
    const std::int64_t at = std::clamp(s.fills[i].at.ts.ns, start, stop);
    if (at > from && leaves.is_positive()) emit(from, at, leaves);
    from = std::max(from, at);
    leaves = less(leaves, s.fills[i].qty);
  }
  if (stop > from && leaves.is_positive()) emit(from, stop, leaves);
}

void OwnQuantity::gc(Timestamp now) noexcept {
  last_gc_ = now.ns;
  const std::int64_t horizon = now.ns - retention_.ns;
  for (std::vector<Segment>& v : by_inst_)
    std::erase_if(v, [&](const Segment& g) { return g.end < horizon; });
  scratch_.clear();
  lives_->for_each([&](Handle<Life>, const Life& s) {
    if (s.ended && s.end.ts.ns < horizon) scratch_.push_back(s.id);
  });
  for (const ClientOrderId id : scratch_) drop(id);
}

Qty OwnQuantity::own_at(InstrumentId inst, Side side, Price px, Timestamp t) const noexcept {
  if (index_) {
    const auto it =
        index_->levels.find(Index::Key{inst.value, static_cast<std::uint8_t>(side), px.raw});
    if (it == index_->levels.end()) return Qty{};
    const Index::Slot& lvl = it->second;
    auto i = static_cast<std::size_t>(
        std::upper_bound(lvl.segs.begin(),
                         lvl.segs.end(),
                         t.ns,
                         [](std::int64_t v, const Segment& s) { return v < s.start; }) -
        lvl.segs.begin());
    Qty q;
    while (i > 0 && lvl.ends[i - 1] > t.ns) {
      --i;
      if (lvl.segs[i].end > t.ns) q += lvl.segs[i].qty;
    }
    return q;
  }
  if (inst.value >= by_inst_.size()) return Qty{};
  Qty q;
  for (const Segment& g : by_inst_[inst.value]) {
    if (g.side == side && g.px == px && g.start <= t.ns && t.ns < g.end) q += g.qty;
  }
  return q;
}

void OwnQuantity::index() {
  index_ = std::make_unique<Index>();
  for (std::size_t k = 0; k < by_inst_.size(); ++k) {
    for (const Segment& g : by_inst_[k]) {
      index_
          ->levels[Index::Key{
              static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(g.side), g.px.raw}]
          .segs.push_back(g);
    }
  }
  for (auto& [key, lvl] : index_->levels) {
    std::stable_sort(lvl.segs.begin(), lvl.segs.end(), [](const Segment& a, const Segment& b) {
      return a.start < b.start;
    });
    lvl.ends.resize(lvl.segs.size());
    std::int64_t m = std::numeric_limits<std::int64_t>::min();
    for (std::size_t i = 0; i < lvl.segs.size(); ++i) {
      m = std::max(m, lvl.segs[i].end);
      lvl.ends[i] = m;
    }
  }
}

}  // namespace fastmm
