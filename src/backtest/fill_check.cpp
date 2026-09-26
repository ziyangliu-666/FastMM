#include "fastmm/backtest/fill_check.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/queue_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fastmm::bt {

namespace {

using Book = L2Book<256>;

constexpr std::int64_t kMs = 1'000'000;

// Pass-2 state of an order of the log.
struct Placed {
  std::int64_t row = -1;  // index in FillCheckResult::orders, -1 when not in the models
  bool active = false;    // in the models
};

// One step of the venue-time replay.
enum class Step : std::uint8_t { Depth = 0, Place = 1, Trade = 2, Remove = 3 };
struct Item {
  std::int64_t ts;
  Step step;
  std::uint32_t seq;  // recorded order among equal (ts, step)
  const EventHeader* md = nullptr;
  std::uint32_t order = 0;  // index into OwnOrderLog::orders
};

// The orders' lives come from collect_own_orders (venue time). This replays the journal's book and
// trade messages together with those lives sorted by venue time, because a venue's execution
// report reaches us before the public trade that caused it, and a trade by receive time would
// fall after the order's end. In a live session the depth is first stripped of our own orders.
class Walker {
 public:
  Walker(std::span<const double> conservatism, JournalReader& reader, const OwnOrderLog& log)
      : books_(reader.instruments().size()), log_(log), placed_(log.orders.size()) {
    for (const double c : conservatism) {
      const auto bps = static_cast<std::int64_t>(std::llround(std::clamp(c, 0.0, 1.0) * 10'000));
      models_.push_back(std::make_unique<QueuePositionModel>(bps));
      model_ptrs_.push_back(models_.back().get());
      res_.conservatism.push_back(static_cast<double>(bps) / 10'000);
    }
    res_.own_in_depth = log.live;
    res_.ms_order_times = log.ms_order_times;
    res_.orders_sent = log.orders_sent;
    res_.rejected = log.rejected;
    res_.unknown_acks = log.unknown_acks;
    if (log.live) stripper_ = std::make_unique<OwnOrderStripper>(reader);
  }

  void on_event(const EventHeader& h) {
    if ((h.flags & EventHeader::kOutbound) != 0) return;
    if (h.type == EventType::BookTicker) {
      items_.push_back(Item{venue_ts(h).ns, Step::Depth, next_seq_++, &h});
      return;
    }
    if (h.type != EventType::BookDelta && h.type != EventType::BookSnapshot &&
        h.type != EventType::Trade)
      return;
    if (!h.exch_ts.valid()) ++res_.md_recv_times;
    items_.push_back(Item{
        venue_ts(h).ns, h.type == EventType::Trade ? Step::Trade : Step::Depth, next_seq_++, &h});
  }

  FillCheckResult finish() {
    schedule();
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
      if (a.ts != b.ts) return a.ts < b.ts;
      if (a.step != b.step) return a.step < b.step;
      return a.seq < b.seq;
    });
    for (const Item& it : items_) replay(it);
    for (FillCheckOrder& o : res_.orders) {
      if (o.end == FillCheckEnd::Open) o.end_ts = log_.last_ts;
    }
    return std::move(res_);
  }

 private:
  [[nodiscard]] Timestamp time_of(const VenueTime& t) {
    if (t.from_recv) ++res_.order_recv_times;
    return t.ts;
  }
  [[nodiscard]] std::uint32_t index_of(const OwnOrder& o) const noexcept {
    return static_cast<std::uint32_t>(&o - log_.orders.data());
  }

  void schedule() {
    for (std::uint32_t i = 0; i < log_.orders.size(); ++i) {
      const OwnOrder& s = log_.orders[i];
      if (!s.acked) {
        if (!s.rejected) ++res_.not_acked;
        continue;
      }
      const Timestamp ack = time_of(s.ack);
      if (!s.resting_type()) {
        ++res_.not_resting;
        continue;
      }
      if (s.ended && s.end.ts < ack) {
        static_cast<void>(time_of(s.end));
        ++res_.ended_before_ack;
        continue;
      }
      items_.push_back(Item{ack.ns, Step::Place, next_seq_++, nullptr, i});
      for (const OwnFill& f : s.fills) static_cast<void>(time_of(f.at));
      if (!s.ended) continue;
      static_cast<void>(time_of(s.end));
      // Millisecond venue times: the end may lie anywhere in its millisecond, so trades up to its
      // last nanosecond still count (and are counted as ties).
      items_.push_back(Item{log_.gone(s).ns, Step::Remove, next_seq_++, nullptr, i});
    }
  }

  Book& book(InstrumentId id) {
    if (id.value >= books_.size()) books_.resize(id.value + 1);
    if (!books_[id.value]) books_[id.value] = std::make_unique<Book>();
    return *books_[id.value];
  }
  QueueTouch& touch(InstrumentId id) {
    if (id.value >= touches_.size()) touches_.resize(id.value + 1);
    return touches_[id.value];
  }

  // The freshest top of book: without our own quantity at its venue time, it moves the queues
  // when it is newer than the mirrored depth.
  void on_ticker(const BookTickerMsg& m) {
    ++res_.tickers;
    const InstrumentId id = m.hdr.instrument;
    const Timestamp t = venue_ts(m.hdr);
    QueueTouch& tt = touch(id);
    tt = queue_touch(
        m, [&](Side s, Price p) { return stripper_ ? stripper_->own_at(id, s, p, t) : Qty{}; });
    if (queue_apply_touch(book(id), id, tt, model_ptrs_)) ++res_.tickers_used;
  }
  FillCheckOrder* row(const OwnOrder& s) {
    const Placed& p = placed_[index_of(s)];
    return p.row < 0 ? nullptr : &res_.orders[static_cast<std::size_t>(p.row)];
  }

  void replay(const Item& it) {
    switch (it.step) {
      case Step::Depth: {
        if (it.md->type == EventType::BookTicker) {
          on_ticker(msg_cast<BookTickerMsg>(it.md));
          break;
        }
        ++res_.md_events;
        const EventHeader* h = stripper_ ? stripper_->strip(*it.md, buf_) : it.md;
        if (h != nullptr)
          queue_apply_book(
              book(h->instrument), msg_cast<BookDeltaMsg>(h), Timestamp{it.ts}, model_ptrs_);
        break;
      }
      case Step::Trade:
        ++res_.md_events;
        on_trade(msg_cast<TradeMsg>(it.md), Timestamp{it.ts});
        break;
      case Step::Place:
        place(log_.orders[it.order]);
        break;
      case Step::Remove:
        remove(log_.orders[it.order]);
        break;
    }
  }

  void place(const OwnOrder& s) {
    const OwnOrder* orig = s.replaces.valid() ? log_.find(s.replaces) : nullptr;
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
    // The book as of the ack's venue time (stripped of our own orders in a live session): depth
    // at that time or later is applied after the order entered.
    o.queue_ahead =
        queue_at_placement(level_qty(b, s.side, s.price), s.side, s.price, b, touch(s.instrument));
    o.ack_ts = s.ack.ts;
    o.end = s.ended ? s.why : FillCheckEnd::Open;
    o.end_ts = s.ended ? s.end.ts : Timestamp{};
    for (const OwnFill& f : s.fills) {
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
          const QueuedOrder& old = q->get(h);
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
    Placed& p = placed_[index_of(s)];
    p.row = static_cast<std::int64_t>(res_.orders.size());
    p.active = true;
    active_.push_back(index_of(s));
    res_.orders.push_back(std::move(o));
  }

  void deactivate(const OwnOrder& s) {
    Placed& p = placed_[index_of(s)];
    if (!p.active) return;
    p.active = false;
    active_.erase(std::remove(active_.begin(), active_.end(), index_of(s)), active_.end());
  }
  void remove(const OwnOrder& s) {
    for (QueuePositionModel* q : model_ptrs_) {
      if (const auto h = q->find(s.id); h.valid()) q->remove(h);
    }
    deactivate(s);
  }

  void on_trade(const TradeMsg& t, Timestamp ts) {
    const bool ms = log_.ms_order_times;
    // Millisecond end times: a trade after the last fill's trade id did not fill the order, even
    // inside the end's millisecond (Binance's exec id is the public trade id).
    if (ms) {
      for (std::size_t n = active_.size(); n-- > 0;) {
        const OwnOrder& s = log_.orders[active_[n]];
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
                 [&](QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty) {
                   if (const OwnOrder* s = log_.find(o.cl_ord_id)) {
                     if (FillCheckOrder* r = row(*s)) {
                       r->model_filled[k] += fill;
                       if (!r->model_first_fill_ts[k].valid()) r->model_first_fill_ts[k] = ts;
                       if (k == 0 && ms && !s->ack.from_recv && ts.ns < s->ack.ts.ns + kMs)
                         ++res_.ack_ties;
                       // After the end's millisecond start: a tie unless the trade id shows it is
                       // the fill's own trade or an earlier one.
                       if (k == 0 && ms && s->ended && !s->end.from_recv && ts > s->end.ts &&
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
  std::vector<QueueTouch> touches_;
  std::vector<std::unique_ptr<QueuePositionModel>> models_;
  std::vector<QueuePositionModel*> model_ptrs_;
  const OwnOrderLog& log_;
  std::vector<Placed> placed_;
  std::unique_ptr<OwnOrderStripper> stripper_;
  sim::EventBuf buf_;
  std::vector<std::uint32_t> active_;
  std::vector<Item> items_;
  std::uint32_t next_seq_ = 0;
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
  const OwnOrderLog log = collect_own_orders(reader);
  Walker w(conservatism, reader, log);
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
  fmt::format_to(it,
                 "market   {} book and trade messages, {} tickers ({} newer than the depth)\n",
                 r.md_events,
                 r.tickers,
                 r.tickers_used);
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
