#include "fastmm/backtest/fill_check.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/sim/queue_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fastmm::bt {

std::string_view to_string(FillCheckEnd e) noexcept {
  switch (e) {
    case FillCheckEnd::Open:
      return "open";
    case FillCheckEnd::Canceled:
      return "canceled";
    case FillCheckEnd::Filled:
      return "filled";
    case FillCheckEnd::Expired:
      return "expired";
    case FillCheckEnd::Replaced:
      return "replaced";
    case FillCheckEnd::Reconciled:
      return "reconciled";
  }
  return "?";
}

namespace {

using sim::QueuePositionModel;
using Book = L2Book<256>;

constexpr std::int64_t kMs = 1'000'000;

// Venue time of a market-data message: its exch_ts, else (a source without one) its receive time.
Timestamp venue_ts(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

std::uint64_t numeric_exec_id(const ExecId& id) noexcept {
  const std::string_view v = id.view();
  std::uint64_t n = 0;
  const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  return ec == std::errc{} && ptr == v.data() + v.size() ? n : 0;
}

// A time of an order event: the venue's (exch_ts), or the receive time when the venue gave none.
struct VenueTime {
  Timestamp ts;
  bool from_recv = true;
  [[nodiscard]] bool set() const noexcept { return ts.valid(); }
  // Take `h`'s time if there is none yet, or if the one held is only a receive time.
  void offer(const EventHeader& h) noexcept {
    if (h.exch_ts.valid() && (from_recv || !ts.valid())) {
      ts = h.exch_ts;
      from_recv = false;
    } else if (!ts.valid()) {
      ts = h.recv_ts;
      from_recv = true;
    }
  }
};

struct LiveFill {
  VenueTime at;
  Qty qty;
  std::uint64_t exec = 0;  // numeric exec id, 0 when it is not a number
};

// An order the session sent, from its outbound copy, and what the venue said about it.
struct Sent {
  ClientOrderId id;
  InstrumentId instrument;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price;
  Qty qty;
  ClientOrderId replaces;     // OutReplace: the order it replaces
  ClientOrderId replaced_by;  // the OutReplace that replaces it
  bool acked = false;
  bool rejected = false;
  bool ended = false;  // live order gone (before or after the ack)
  VenueTime ack;
  VenueTime end;
  FillCheckEnd why = FillCheckEnd::Open;
  std::vector<LiveFill> fills;
  std::uint64_t last_exec = 0;  // numeric exec id of the last fill (Binance: the trade id)
  // Pass 2.
  std::int64_t row = -1;  // index in FillCheckResult::orders, -1 when not in the models
  bool active = false;    // in the models
  Qty live_leaves;        // own quantity the venue's book shows (own_in_depth)
};

// One step of the venue-time replay.
enum class Step : std::uint8_t { Depth = 0, Place = 1, Trade = 2, LiveFill = 3, Remove = 4 };
struct Item {
  std::int64_t ts;
  Step step;
  std::uint32_t seq;  // recorded order among equal (ts, step)
  const EventHeader* md = nullptr;
  std::uint32_t order = 0;  // index into Walker::orders_
  std::uint32_t fill = 0;   // Step::LiveFill: index into Sent::fills
};

// Two passes. The first reads the journal in recorded order and collects each order's life in
// venue time: the ack's exch_ts (the matching engine's time), the end's (cancel ack, last fill,
// expiry), its fills. The second replays market data and those order steps sorted by venue time,
// because a venue's execution report reaches us before the public trade that caused it, and a
// trade by receive time would fall after the order's end.
class Walker {
 public:
  Walker(std::span<const double> conservatism, std::size_t instruments, bool own_in_depth)
      : books_(instruments) {
    for (const double c : conservatism) {
      const auto bps = static_cast<std::int64_t>(std::llround(std::clamp(c, 0.0, 1.0) * 10'000));
      models_.push_back(std::make_unique<QueuePositionModel>(bps));
      model_ptrs_.push_back(models_.back().get());
      res_.conservatism.push_back(static_cast<double>(bps) / 10'000);
    }
    res_.own_in_depth = own_in_depth;
  }

  void on_event(const EventHeader& h) {
    if (h.type == EventType::EngineTime || h.type == EventType::Padding ||
        h.type == EventType::LatencySample)
      return;
    const Timestamp vts = venue_ts(h);
    if (vts.valid() && vts > last_ts_) last_ts_ = vts;
    if ((h.flags & EventHeader::kOutbound) != 0) {
      if ((h.flags & EventHeader::kDropped) == 0) on_outbound(h);
      return;
    }
    const auto order_time = [&] {
      if (h.exch_ts.valid()) {
        ++order_venue_times_;
        if (h.exch_ts.ns % kMs != 0) ms_order_times_ = false;
      }
    };
    switch (h.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
      case EventType::Trade:
        if (!h.exch_ts.valid()) ++res_.md_recv_times;
        items_.push_back(
            Item{vts.ns, h.type == EventType::Trade ? Step::Trade : Step::Depth, next_seq_++, &h});
        break;
      case EventType::OrderAck:
        order_time();
        on_ack(h, msg_cast<OrderAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderFill:
        order_time();
        on_fill(h, msg_cast<OrderFillMsg>(&h));
        break;
      case EventType::OrderCancelAck:
        order_time();
        on_cancel_ack(h, msg_cast<OrderCancelAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderExpired:
        order_time();
        if (Sent* s = find(msg_cast<OrderExpiredMsg>(&h).cl_ord_id))
          end(*s, h, FillCheckEnd::Expired);
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
  }

  FillCheckResult finish() {
    ms_ = ms_order_times_ && order_venue_times_ > 0;
    res_.ms_order_times = ms_;
    schedule();
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
      if (a.ts != b.ts) return a.ts < b.ts;
      if (a.step != b.step) return a.step < b.step;
      return a.seq < b.seq;
    });
    for (const Item& it : items_) replay(it);
    for (FillCheckOrder& o : res_.orders) {
      if (o.end == FillCheckEnd::Open) o.end_ts = last_ts_;
    }
    return std::move(res_);
  }

 private:
  // ---- pass 1: recorded order ----------------------------------------------------------------

  Sent* find(ClientOrderId id) {
    const auto it = index_.find(id.value);
    return it == index_.end() ? nullptr : &orders_[it->second];
  }
  Sent& add(const Sent& s) {
    if (const auto it = index_.find(s.id.value); it != index_.end()) {
      orders_[it->second] = s;
      return orders_[it->second];
    }
    index_.emplace(s.id.value, orders_.size());
    orders_.push_back(s);
    return orders_.back();
  }

  void on_outbound(const EventHeader& h) {
    if (h.type == EventType::OutNewOrder) {
      const auto& m = msg_cast<OutNewOrderMsg>(&h);
      Sent s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.side = m.side;
      s.type = m.type;
      s.tif = m.tif;
      s.price = m.price;
      s.qty = m.qty;
      add(s);
      ++res_.orders_sent;
    } else if (h.type == EventType::OutReplace) {
      const auto& m = msg_cast<OutReplaceMsg>(&h);
      Sent s;
      s.id = m.cl_ord_id;
      s.instrument = h.instrument;
      s.price = m.price;
      s.qty = m.qty;
      s.replaces = m.orig_cl_ord_id;
      if (Sent* orig = find(m.orig_cl_ord_id)) {
        s.side = orig->side;
        s.type = orig->type == OrderType::PostOnly ? OrderType::PostOnly : OrderType::Limit;
        orig->replaced_by = m.cl_ord_id;
      }
      add(s);
      ++res_.orders_sent;
    }
  }

  void on_ack(const EventHeader& h, ClientOrderId id) {
    Sent* s = find(id);
    if (s == nullptr) {
      ++res_.unknown_acks;
      return;
    }
    // A venue may acknowledge twice (API response and user stream); either may carry the time.
    s->ack.offer(h);
    if (s->acked) return;
    s->acked = true;
    if (Sent* orig = s->replaces.valid() ? find(s->replaces) : nullptr)
      end(*orig, h, FillCheckEnd::Replaced);
  }

  void on_fill(const EventHeader& h, const OrderFillMsg& m) {
    Sent* s = find(m.cl_ord_id);
    if (s == nullptr) return;
    Qty filled;
    for (const LiveFill& f : s->fills) filled += f.qty;
    const std::uint64_t exec = numeric_exec_id(m.exec_id);
    const bool seen = exec != 0 && std::any_of(s->fills.begin(),
                                               s->fills.end(),
                                               [&](const LiveFill& f) { return f.exec == exec; });
    if (!s->ended && !seen) {
      LiveFill f;
      f.exec = exec;
      f.at.offer(h);
      f.qty = m.qty;
      s->fills.push_back(f);
      filled += m.qty;
      if (exec != 0) s->last_exec = exec;
    }
    const bool last =
        (m.flags & OrderFillMsg::kReplayed) == 0 ? m.leaves_qty.is_zero() : m.cum_qty >= s->qty;
    if (last || filled >= s->qty) end(*s, h, FillCheckEnd::Filled);
  }

  void on_cancel_ack(const EventHeader& h, ClientOrderId id) {
    Sent* s = find(id);
    if (s == nullptr) return;
    // A second cancel ack (API response and user stream) may carry the venue time the first
    // did not.
    if (s->ended && (s->why == FillCheckEnd::Canceled || s->why == FillCheckEnd::Replaced)) {
      s->end.offer(h);
      return;
    }
    end(*s, h, s->replaced_by.valid() ? FillCheckEnd::Replaced : FillCheckEnd::Canceled);
  }

  void on_reject(ClientOrderId id) {
    Sent* s = find(id);
    if (s == nullptr || s->acked || s->rejected) return;
    s->rejected = true;
    ++res_.rejected;
    s->ended = true;
  }

  // Begin, OpenOrder..., End: an acked order the snapshot does not list is gone. With a sent
  // watermark, orders above it were not yet sent when the snapshot was taken.
  void on_reconcile(const EventHeader& h, const ReconcileMsg& m) {
    switch (m.kind) {
      case ReconcileMsg::Kind::Begin:
        listed_.clear();
        watermark_ =
            (m.flags & ReconcileMsg::kSentWatermark) != 0 ? m.sent_watermark : ClientOrderId{};
        break;
      case ReconcileMsg::Kind::OpenOrder:
        listed_.insert(m.cl_ord_id.value);
        break;
      case ReconcileMsg::Kind::End:
        for (Sent& s : orders_) {
          if (!s.acked || s.ended || listed_.contains(s.id.value)) continue;
          if (watermark_.valid() && s.id.value > watermark_.value) continue;
          end(s, h, FillCheckEnd::Reconciled);
        }
        break;
      default:
        break;
    }
  }

  void end(Sent& s, const EventHeader& h, FillCheckEnd why) {
    if (s.ended) return;
    s.ended = true;
    s.why = why;
    s.end.offer(h);
  }

  // ---- between the passes: order steps in venue time -------------------------------------------

  [[nodiscard]] Timestamp time_of(const VenueTime& t) {
    if (t.from_recv) ++res_.order_recv_times;
    return t.ts;
  }

  void schedule() {
    for (std::uint32_t i = 0; i < orders_.size(); ++i) {
      Sent& s = orders_[i];
      if (!s.acked) {
        if (!s.rejected) ++res_.not_acked;
        continue;
      }
      const Timestamp ack = time_of(s.ack);
      if (s.type == OrderType::Market || s.tif == TimeInForce::Ioc || s.tif == TimeInForce::Fok) {
        ++res_.not_resting;
        continue;
      }
      if (s.ended && s.end.ts < ack) {
        static_cast<void>(time_of(s.end));
        ++res_.ended_before_ack;
        continue;
      }
      items_.push_back(Item{ack.ns, Step::Place, next_seq_++, nullptr, i});
      for (std::uint32_t f = 0; f < s.fills.size(); ++f)
        items_.push_back(
            Item{time_of(s.fills[f].at).ns, Step::LiveFill, next_seq_++, nullptr, i, f});
      if (!s.ended) continue;
      // The replaced order leaves the models when its successor is acknowledged (which may keep
      // the queue position), not at its own cancel ack.
      const Sent* next = s.replaced_by.valid() ? find(s.replaced_by) : nullptr;
      Timestamp gone = time_of(s.end);
      if (next != nullptr && next->acked && next->ack.ts > gone) gone = next->ack.ts;
      // Millisecond venue times (Binance transactTime): the end may lie anywhere in its
      // millisecond, so trades up to its last nanosecond still count (and are counted as ties).
      if (ms_ && !s.end.from_recv) gone = Timestamp{gone.ns + kMs - 1};
      items_.push_back(Item{gone.ns, Step::Remove, next_seq_++, nullptr, i});
    }
  }

  // ---- pass 2: venue time
  // ------------------------------------------------------------------------

  Book& book(InstrumentId id) {
    if (id.value >= books_.size()) books_.resize(id.value + 1);
    if (!books_[id.value]) books_[id.value] = std::make_unique<Book>();
    return *books_[id.value];
  }
  FillCheckOrder* row(const Sent& s) {
    return s.row < 0 ? nullptr : &res_.orders[static_cast<std::size_t>(s.row)];
  }

  void replay(const Item& it) {
    switch (it.step) {
      case Step::Depth:
        ++res_.md_events;
        apply_book(msg_cast<BookDeltaMsg>(it.md), Timestamp{it.ts});
        break;
      case Step::Trade:
        ++res_.md_events;
        on_trade(msg_cast<TradeMsg>(it.md), Timestamp{it.ts});
        break;
      case Step::Place:
        place(orders_[it.order]);
        break;
      case Step::LiveFill: {
        Sent& s = orders_[it.order];
        const Qty q = s.fills[it.fill].qty;
        s.live_leaves = q >= s.live_leaves ? Qty{} : s.live_leaves - q;
        break;
      }
      case Step::Remove:
        remove(orders_[it.order]);
        break;
    }
  }

  void place(Sent& s) {
    Sent* orig = s.replaces.valid() ? find(s.replaces) : nullptr;
    Book& b = book(s.instrument);
    const Level best = s.side == Side::Buy ? b.best_ask() : b.best_bid();
    if (best.qty.is_positive() &&
        (s.side == Side::Buy ? s.price >= best.price : s.price <= best.price)) {
      ++res_.crossed_at_ack;
      if (orig != nullptr) remove(*orig);
      return;
    }
    FillCheckOrder o;
    o.cl_ord_id = s.id;
    o.instrument = s.instrument;
    o.side = s.side;
    o.price = s.price;
    o.qty = s.qty;
    // The book as of the ack's venue time: depth at that time or later is applied after the
    // order entered, so the venue's book does not hold the order yet. It may still hold one of
    // ours that ended before (a cancel-then-new at the same price), until the next depth update
    // at that level: that one is not ahead of us.
    o.queue_ahead = less(sim::level_qty(b, s.side, s.price), own_at(s.instrument, s.side, s.price));
    o.ack_ts = s.ack.ts;
    o.end = s.ended ? s.why : FillCheckEnd::Open;
    o.end_ts = s.ended ? s.end.ts : Timestamp{};
    for (const LiveFill& f : s.fills) {
      o.live_filled += f.qty;
      if (!o.live_first_fill_ts.valid()) o.live_first_fill_ts = f.at.ts;
    }
    o.model_filled.assign(models_.size(), Qty{});
    o.model_first_fill_ts.assign(models_.size(), Timestamp{});
    for (QueuePositionModel* q : model_ptrs_) {
      // SimTransport::queue_replace: the same price at no more than the leaves keeps the place.
      if (orig != nullptr) {
        const auto h = q->find(orig->id);
        if (h.valid()) {
          const sim::QueuedOrder& old = q->get(h);
          if (old.price == s.price && s.qty <= old.leaves() &&
              q->amend_keep_priority(h, s.id, 0, s.qty))
            continue;
          q->remove(h);
        }
      }
      if (!q->place(s.id, 0, s.instrument, s.side, s.price, s.qty, o.queue_ahead).valid())
        ++res_.model_full;
    }
    if (orig != nullptr) deactivate(*orig);
    s.row = static_cast<std::int64_t>(res_.orders.size());
    s.active = true;
    s.live_leaves = s.qty;
    active_.push_back(static_cast<std::uint32_t>(&s - orders_.data()));
    res_.orders.push_back(std::move(o));
  }

  void deactivate(Sent& s) {
    if (!s.active) return;
    s.active = false;
    if (res_.own_in_depth && s.live_leaves.is_positive())
      ghosts_.push_back(Ghost{s.instrument, s.side, s.price, s.live_leaves});
    const auto idx = static_cast<std::uint32_t>(&s - orders_.data());
    active_.erase(std::remove(active_.begin(), active_.end(), idx), active_.end());
  }
  void remove(Sent& s) {
    for (QueuePositionModel* q : model_ptrs_) {
      if (const auto h = q->find(s.id); h.valid()) q->remove(h);
    }
    deactivate(s);
  }

  // Our own quantity in the depth feed's level, when the feed includes our orders: the live
  // leaves of our resting orders there and, with `ghosts`, of orders that ended after the level's
  // last depth update.
  [[nodiscard]] Qty own_at(InstrumentId inst, Side side, Price px, bool ghosts = true) const {
    Qty q;
    if (!res_.own_in_depth) return q;
    for (const std::uint32_t i : active_) {
      const Sent& s = orders_[i];
      if (s.instrument == inst && s.side == side && s.price == px) q += s.live_leaves;
    }
    if (ghosts) {
      for (const Ghost& g : ghosts_) {
        if (g.instrument == inst && g.side == side && g.price == px) q += g.qty;
      }
    }
    return q;
  }
  void drop_ghosts(InstrumentId inst, Side side, Price px, bool whole_instrument) {
    std::erase_if(ghosts_, [&](const Ghost& g) {
      return g.instrument == inst && (whole_instrument || (g.side == side && g.price == px));
    });
  }
  static Qty less(Qty a, Qty b) noexcept { return b >= a ? Qty{} : a - b; }

  // sim::queue_apply_book, with our own quantity taken out of each level when the depth shows it.
  void apply_book(const BookDeltaMsg& d, Timestamp now) {
    if (!res_.own_in_depth) {
      sim::queue_apply_book(book(d.hdr.instrument), d, now, model_ptrs_);
      return;
    }
    Book& b = book(d.hdr.instrument);
    const InstrumentId id = d.hdr.instrument;
    if (d.is_snapshot()) {
      b.apply_delta(d);
      drop_ghosts(id, Side::Buy, Price{}, true);
      for (QueuePositionModel* q : model_ptrs_) {
        q->for_each([&](QueuePositionModel::Handle32 h, const sim::QueuedOrder& o) {
          if (o.instrument != id) return;
          const Qty shown = less(sim::level_qty(b, o.side, o.price), own_at(id, o.side, o.price));
          if (shown < o.ahead) q->get(h).ahead = shown;
        });
      }
      return;
    }
    for (Side s : {Side::Buy, Side::Sell}) {
      for (const Level& l : (s == Side::Buy ? d.bids() : d.asks())) {
        const Qty old = sim::level_qty(b, s, l.price);
        b.apply_level(s, l.price, l.qty);
        // The update is later than any ghost at this level, so it no longer shows them.
        const Qty own_before = own_at(id, s, l.price);
        drop_ghosts(id, s, l.price, false);
        const Qty own_after = own_at(id, s, l.price);
        for (QueuePositionModel* q : model_ptrs_)
          q->on_level_change(id, s, l.price, less(old, own_before), less(l.qty, own_after));
      }
    }
    b.set_seq(d.last_update_id);
    b.set_last_update(now);
  }

  void on_trade(const TradeMsg& t, Timestamp ts) {
    // Millisecond end times: a trade after the last fill's trade id did not fill the order, even
    // inside the end's millisecond (Binance's exec id is the public trade id).
    if (ms_) {
      for (std::size_t n = active_.size(); n-- > 0;) {
        Sent& s = orders_[active_[n]];
        if (s.ended && s.why == FillCheckEnd::Filled && s.instrument == t.hdr.instrument &&
            s.last_exec != 0 && t.trade_id > s.last_exec && ts > s.end.ts)
          remove(s);
      }
    }
    for (std::size_t k = 0; k < models_.size(); ++k) {
      QueuePositionModel& q = *models_[k];
      q.on_trade(t.hdr.instrument,
                 t.price,
                 t.qty,
                 t.aggressor,
                 [&](QueuePositionModel::Handle32 h, sim::QueuedOrder& o, Qty fill, Qty) {
                   if (Sent* s = find(o.cl_ord_id)) {
                     if (FillCheckOrder* r = row(*s)) {
                       r->model_filled[k] += fill;
                       if (!r->model_first_fill_ts[k].valid()) r->model_first_fill_ts[k] = ts;
                       if (k == 0 && ms_ && !s->ack.from_recv && ts.ns < s->ack.ts.ns + kMs)
                         ++res_.ack_ties;
                       // After the end's millisecond start: a tie unless the trade id shows it
                       // is the fill's own trade or an earlier one.
                       if (k == 0 && ms_ && s->ended && !s->end.from_recv && ts > s->end.ts &&
                           !(s->why == FillCheckEnd::Filled && s->last_exec != 0 &&
                             t.trade_id <= s->last_exec))
                         ++res_.end_ties;
                     }
                   }
                   if (o.leaves().is_zero()) q.remove(h);
                 });
    }
  }

  std::vector<std::unique_ptr<Book>> books_;
  std::vector<std::unique_ptr<QueuePositionModel>> models_;
  std::vector<QueuePositionModel*> model_ptrs_;
  std::vector<Sent> orders_;
  std::unordered_map<std::uint64_t, std::size_t> index_;
  std::vector<std::uint32_t> active_;
  struct Ghost {
    InstrumentId instrument;
    Side side;
    Price price;
    Qty qty;
  };
  std::vector<Ghost> ghosts_;  // own orders gone at the venue, still in the depth feed
  std::vector<Item> items_;
  std::uint32_t next_seq_ = 0;
  std::unordered_set<std::uint64_t> listed_;
  ClientOrderId watermark_;
  Timestamp last_ts_;
  std::uint64_t order_venue_times_ = 0;
  bool ms_order_times_ = true;
  bool ms_ = false;
  FillCheckResult res_;
};

std::string qty_text(Qty q) {
  char buf[kMaxDecimalChars];
  return {buf, q.to_decimal(buf)};
}
std::string price_text(Price p) {
  char buf[kMaxDecimalChars];
  return {buf, p.to_decimal(buf)};
}

}  // namespace

FillCheckSummary FillCheckResult::summary(std::size_t k) const {
  FillCheckSummary s;
  s.conservatism = conservatism.at(k);
  std::vector<std::int64_t> dt;
  for (const FillCheckOrder& o : orders) {
    const bool live = o.live_filled.is_positive();
    const bool model = o.model_filled[k].is_positive();
    s.live_qty += o.live_filled;
    s.model_qty += o.model_filled[k];
    if (live && model) {
      ++s.both;
      if (o.live_first_fill_ts.valid() && o.model_first_fill_ts[k].valid())
        dt.push_back(std::abs((o.model_first_fill_ts[k] - o.live_first_fill_ts).ns));
    } else if (live) {
      ++s.live_only;
    } else if (model) {
      ++s.model_only;
    } else {
      ++s.neither;
    }
  }
  if (!dt.empty()) {
    const auto mid = dt.begin() + static_cast<std::ptrdiff_t>(dt.size() / 2);
    std::nth_element(dt.begin(), mid, dt.end());
    if (dt.size() % 2 == 1) {
      s.median_abs_dt_ns = *mid;
    } else {
      const std::int64_t hi = *mid;
      const std::int64_t lo = *std::max_element(dt.begin(), mid);
      s.median_abs_dt_ns = lo + (hi - lo) / 2;
    }
  }
  return s;
}

FillCheckResult fill_check(JournalReader& reader, std::span<const double> conservatism) {
  // A live session (TscClock) records the venue's depth feed, which includes our own orders; a
  // backtest's simulated feed does not (sim::SimTransport replays the data without them).
  Walker w(conservatism, reader.instruments().size(), reader.header().tsc0 != 0);
  reader.for_each([&](const EventHeader* h) { w.on_event(*h); });
  reader.reset();
  return w.finish();
}

FillCheckResult fill_check(const std::string& path, std::span<const double> conservatism) {
  JournalReader reader;
  if (auto r = reader.open(path); !r) {
    throw std::runtime_error("cannot open " + path + ": " + std::string(to_string(r.error())));
  }
  return fill_check(reader, conservatism);
}

std::string format_fill_check(const FillCheckResult& r) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "orders   {} sent, {} resting after the ack; left out: {} rejected, {} not acked, "
                 "{} ended before the ack, {} market/IOC/FOK, {} crossing the book at the ack\n",
                 r.orders_sent,
                 r.orders.size(),
                 r.rejected,
                 r.not_acked,
                 r.ended_before_ack,
                 r.not_resting,
                 r.crossed_at_ack);
  if (r.unknown_acks > 0 || r.model_full > 0) {
    fmt::format_to(it,
                   "warning  {} acks without an outbound copy, {} orders the queue model had no "
                   "room for\n",
                   r.unknown_acks,
                   r.model_full);
  }
  fmt::format_to(it, "market   {} book and trade messages\n", r.md_events);
  fmt::format_to(it,
                 "times    venue ({}); from receive time: {} order events, {} market messages; "
                 "ties in the ack / end millisecond: {} / {}; own orders in depth: {}\n",
                 r.ms_order_times ? "order times in ms" : "as recorded",
                 r.order_recv_times,
                 r.md_recv_times,
                 r.ack_ties,
                 r.end_ties,
                 r.own_in_depth ? "yes" : "no");
  if (r.conservatism.empty()) return out;
  const FillCheckSummary live = r.summary(0);
  fmt::format_to(
      it, "live     {} filled, qty {}\n", live.both + live.live_only, qty_text(live.live_qty));
  fmt::format_to(it,
                 "\n{:<12} {:>7} {:>6} {:>9} {:>10} {:>7} {:>10} {:>12} {:>9}\n",
                 "conservatism",
                 "filled",
                 "both",
                 "live only",
                 "model only",
                 "neither",
                 "model/live",
                 "model qty",
                 "|dt| p50");
  for (std::size_t k = 0; k < r.conservatism.size(); ++k) {
    const FillCheckSummary s = r.summary(k);
    const std::string ratio = s.live_qty.is_zero() ? "-" : fmt::format("{:.3f}", s.qty_ratio());
    const std::string dt =
        s.median_abs_dt_ns < 0
            ? "-"
            : fmt::format("{:.1f}ms", static_cast<double>(s.median_abs_dt_ns) / 1e6);
    fmt::format_to(it,
                   "{:<12.2f} {:>7} {:>6} {:>9} {:>10} {:>7} {:>10} {:>12} {:>9}\n",
                   s.conservatism,
                   s.both + s.model_only,
                   s.both,
                   s.live_only,
                   s.model_only,
                   s.neither,
                   ratio,
                   qty_text(s.model_qty),
                   dt);
  }
  return out;
}

std::string fill_check_csv(const FillCheckResult& r) {
  std::string out;
  auto it = std::back_inserter(out);
  out +=
      "cl_ord_id,instrument,side,price,qty,queue_ahead,ack_ts_ns,end_ts_ns,resting_ns,end,"
      "live_filled,live_first_fill_ts_ns";
  for (const double c : r.conservatism)
    fmt::format_to(it, ",model_filled_c{0:.2f},model_first_fill_ts_ns_c{0:.2f}", c);
  out += '\n';
  for (const FillCheckOrder& o : r.orders) {
    fmt::format_to(it,
                   "{},{},{},{},{},{},{},{},{},{},{},{}",
                   o.cl_ord_id.value,
                   o.instrument.value,
                   o.side == Side::Buy ? "buy" : "sell",
                   price_text(o.price),
                   qty_text(o.qty),
                   qty_text(o.queue_ahead),
                   o.ack_ts.ns,
                   o.end_ts.ns,
                   o.resting().ns,
                   to_string(o.end),
                   qty_text(o.live_filled),
                   o.live_first_fill_ts.ns);
    for (std::size_t k = 0; k < o.model_filled.size(); ++k)
      fmt::format_to(it, ",{},{}", qty_text(o.model_filled[k]), o.model_first_fill_ts[k].ns);
    out += '\n';
  }
  return out;
}

}  // namespace fastmm::bt
