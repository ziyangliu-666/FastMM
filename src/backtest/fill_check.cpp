#include "fastmm/backtest/fill_check.hpp"

#include "fastmm/backtest/mid_series.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/queue_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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

// One step of the venue-time replay. A live fill's step reads the model's queue before the trade
// of the same time that caused it.
enum class Step : std::uint8_t { Depth = 0, Place = 1, LiveFill = 2, Trade = 3, Remove = 4 };
struct Item {
  std::int64_t ts;
  Step step;
  std::uint32_t seq;  // recorded order among equal (ts, step)
  const EventHeader* md = nullptr;
  std::uint32_t order = 0;  // index into OwnOrderLog::orders
  InstrumentId inst{};
  bool remap = false;  // a market journal's event: its instrument is `inst` in the session
};

double bps(double x, double px) {
  return px > 0 ? x / px * 1e4 : kNoValue;
}

// The orders' lives come from collect_own_orders (venue time). This replays the journal's book and
// trade messages together with those lives sorted by venue time, because a venue's execution
// report reaches us before the public trade that caused it, and a trade by receive time would
// fall after the order's end. In a live session the depth is first stripped of our own orders.
class Walker {
 public:
  Walker(std::span<const double> conservatism,
         const FillCheckOptions& opt,
         JournalReader& reader,
         const OwnOrderLog& log,
         bool market)
      : in_(opt.inputs),
        opt_(opt),
        market_(market),
        books_(reader.instruments().size()),
        log_(log),
        placed_(log.orders.size()) {
    const std::span<const Instrument> inst = reader.instruments();
    ticks_.resize(inst.size());
    for (std::size_t i = 0; i < inst.size(); ++i) ticks_[i] = inst[i].tick;
    res_.horizons_ns = opt.horizons_ns;
    res_.pre_window = opt.pre_window;
    res_.market = opt.market;
    // The first live fill of each order, by its numeric exec id (Binance: the public trade id).
    for (std::uint32_t i = 0; i < log.orders.size(); ++i) {
      const OwnOrder& o = log.orders[i];
      if (o.fills.empty()) continue;
      const OwnFill& f = first(o);
      if (f.exec != 0) exec_order_.emplace(f.exec, i);
    }
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
    if (!market_) add_md(h, h.instrument, false);
  }
  // An event of the market journal, its instrument `inst` in the session (invalid: none).
  void on_market_event(const EventHeader& h, InstrumentId inst) {
    if ((h.flags & EventHeader::kOutbound) != 0 || !is_md(h.type)) return;
    if (!inst.valid()) {
      ++res_.market_unmapped;
      return;
    }
    add_md(h, inst, inst != h.instrument);
  }

  FillCheckResult finish() {
    schedule();
    if (active_by_inst_.size() < books_.size()) active_by_inst_.resize(books_.size());
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
      if (a.ts != b.ts) return a.ts < b.ts;
      if (a.step != b.step) return a.step < b.step;
      return a.seq < b.seq;
    });
    for (const Item& it : items_) replay(it);
    for (FillCheckOrder& o : res_.orders) {
      if (o.end == FillCheckEnd::Open) o.end_ts = log_.last_ts;
    }
    mids_.sort();
    for (FillCheckOrder& o : res_.orders) mark(o);
    return std::move(res_);
  }

 private:
  static bool is_md(EventType t) noexcept {
    return t == EventType::BookTicker || t == EventType::BookDelta ||
           t == EventType::BookSnapshot || t == EventType::Trade;
  }
  void add_md(const EventHeader& h, InstrumentId inst, bool remap) {
    if ((h.flags & EventHeader::kOutbound) != 0 || !is_md(h.type)) return;
    if (inst.value >= books_.size()) books_.resize(inst.value + 1);
    if (h.type != EventType::BookTicker && !h.exch_ts.valid()) ++res_.md_recv_times;
    items_.push_back(Item{venue_ts(h).ns,
                          h.type == EventType::Trade ? Step::Trade : Step::Depth,
                          next_seq_++,
                          &h,
                          0,
                          inst,
                          remap});
  }
  static const OwnFill& first(const OwnOrder& s) {
    const OwnFill* f = &s.fills.front();
    for (const OwnFill& x : s.fills) {
      if (x.at.ts < f->at.ts) f = &x;
    }
    return *f;
  }
  // Same millisecond (venue order times in whole ms), else within a millisecond.
  [[nodiscard]] bool near(Timestamp a, Timestamp b) const noexcept {
    if (log_.ms_order_times) return a.ns / kMs == b.ns / kMs;
    return std::abs((a - b).ns) <= kMs;
  }

  // Markouts and pre-fill moves of the order's first fills, once every mid is in.
  void mark(FillCheckOrder& o) const {
    const double s = o.side == Side::Buy ? 1.0 : -1.0;
    const double px = o.price.to_double();
    const std::size_t nh = opt_.horizons_ns.size();
    const auto pre = [&](Timestamp t) {
      const double m0 = mids_.at(o.instrument.value, t.ns - opt_.pre_window.ns);
      const double m1 = mids_.at(o.instrument.value, t.ns);
      return m0 > 0 && m1 > 0 ? bps(s * (m1 - m0), px) : kNoValue;
    };
    const auto markouts = [&](Timestamp t, double* out) {
      for (std::size_t h = 0; h < nh; ++h) {
        const double m = mids_.at(o.instrument.value, t.ns + opt_.horizons_ns[h]);
        out[h] = m > 0 ? bps(s * (m - px), px) : kNoValue;
      }
    };
    if (o.live_first_fill_ts.valid()) {
      o.live_pre_move_bps = pre(o.live_first_fill_ts);
      markouts(o.live_first_fill_ts, o.live_markout_bps.data());
    }
    for (std::size_t k = 0; k < o.model_first_fill_ts.size(); ++k) {
      if (!o.model_first_fill_ts[k].valid()) continue;
      o.model_pre_move_bps[k] = pre(o.model_first_fill_ts[k]);
      markouts(o.model_first_fill_ts[k], o.model_markout_bps.data() + k * nh);
    }
  }

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
      if (!s.fills.empty())
        items_.push_back(Item{first_fill(s).ns, Step::LiveFill, next_seq_++, nullptr, i});
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
  TradeTape* tape(InstrumentId id) {
    if (!in_.tape) return nullptr;
    if (id.value >= tapes_.size()) tapes_.resize(id.value + 1);
    return &tapes_[id.value];
  }
  static Timestamp first_fill(const OwnOrder& s) { return first(s).at.ts; }

  // The freshest top of book: without our own quantity at its venue time, it moves the queues
  // when it is newer than the mirrored depth.
  void on_ticker(const BookTickerMsg& m) {
    ++res_.tickers;
    const InstrumentId id = m.hdr.instrument;
    const Timestamp t = venue_ts(m.hdr);
    if (m.bid_px.is_positive() && m.ask_px.is_positive()) {
      mids_.add(id, t, (m.bid_px.to_double() + m.ask_px.to_double()) / 2);
      crossed(id, m.bid_px, m.ask_px, t);
    }
    if (!in_.touch) return;
    QueueTouch& tt = touch(id);
    tt = queue_touch(
        m, [&](Side s, Price p) { return stripper_ ? stripper_->own_at(id, s, p, t) : Qty{}; });
    if (queue_apply_touch(book(id), id, tt, tape(id), model_ptrs_)) ++res_.tickers_used;
  }
  FillCheckOrder* row(const OwnOrder& s) {
    const Placed& p = placed_[index_of(s)];
    return p.row < 0 ? nullptr : &res_.orders[static_cast<std::size_t>(p.row)];
  }

  // The item's event with the session's instrument id (a copy in remap_buf_ when it differs).
  const EventHeader* md_of(const Item& it) {
    if (!it.remap) return it.md;
    if (it.md->len > sizeof(remap_buf_.bytes)) return nullptr;
    std::memcpy(remap_buf_.bytes, it.md, it.md->len);
    remap_buf_.hdr().instrument = it.inst;
    return &remap_buf_.hdr();
  }

  void replay(const Item& it) {
    switch (it.step) {
      case Step::Depth: {
        const EventHeader* md = md_of(it);
        if (md == nullptr) break;
        if (md->type == EventType::BookTicker) {
          on_ticker(msg_cast<BookTickerMsg>(md));
          break;
        }
        ++res_.md_events;
        const EventHeader* h = stripper_ ? stripper_->strip(*md, buf_) : md;
        if (h != nullptr) {
          Book& b = book(h->instrument);
          queue_apply_book(
              b, msg_cast<BookDeltaMsg>(h), Timestamp{it.ts}, tape(h->instrument), model_ptrs_);
          crossed(h->instrument, b.best_bid().price, b.best_ask().price, Timestamp{it.ts});
        }
        break;
      }
      case Step::LiveFill:
        at_live_fill(log_.orders[it.order]);
        break;
      case Step::Trade: {
        const EventHeader* md = md_of(it);
        if (md == nullptr) break;
        ++res_.md_events;
        on_trade(msg_cast<TradeMsg>(md), Timestamp{it.ts});
        break;
      }
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
    o.queue_ahead = queue_at_placement(
        level_qty(b, s.side, s.price), s.side, s.price, b, touch(s.instrument), tape(s.instrument));
    const Timestamp view = queue_view_ts(s.side, s.price, b, touch(s.instrument));
    o.ack_ts = s.ack.ts;
    o.end = s.ended ? s.why : FillCheckEnd::Open;
    o.end_ts = s.ended ? s.end.ts : Timestamp{};
    for (const OwnFill& f : s.fills) {
      o.live_filled += f.qty;
      if (!o.live_first_fill_ts.valid()) o.live_first_fill_ts = f.at.ts;
    }
    o.model_filled.assign(models_.size(), Qty{});
    o.model_first_fill_ts.assign(models_.size(), Timestamp{});
    o.model_ahead_at_fill.assign(models_.size(), Qty::from_raw(-1));
    o.model_ahead_at_live_fill.assign(models_.size(), Qty::from_raw(-1));
    o.cancel_sent = s.cancel_sent;
    const std::size_t nh = opt_.horizons_ns.size();
    o.live_markout_bps.assign(nh, kNoValue);
    o.model_through.assign(models_.size(), 0);
    o.model_pre_move_bps.assign(models_.size(), kNoValue);
    o.model_markout_bps.assign(models_.size() * nh, kNoValue);
    if (!s.fills.empty()) o.live_print = FillPrint::Missing;
    {
      // The best price of others on the order's side: the touch when newer than the depth.
      const QueueTouch& tt = touch(s.instrument);
      Price top = (s.side == Side::Buy ? b.best_bid() : b.best_ask()).price;
      if (tt.valid() && tt.newer_than(b.seq(), b.last_update()) && tt.qty(s.side).is_positive())
        top = tt.px(s.side);
      const Price tick = s.instrument.value < ticks_.size() ? ticks_[s.instrument.value] : Price{};
      if (top.is_positive() && tick.is_positive()) {
        o.touch_known = true;
        const std::int64_t d = s.side == Side::Buy ? top.raw - s.price.raw : s.price.raw - top.raw;
        o.ticks_behind = static_cast<std::int32_t>(d / tick.raw);
      }
    }
    for (QueuePositionModel* q : model_ptrs_) {
      // SimTransport::queue_replace: the same price at no more than the leaves keeps the place.
      if (orig != nullptr) {
        const auto h = q->find(orig->id);
        if (h.valid()) {
          const QueuedOrder& old = q->get(h);
          if (old.price == s.price && s.qty <= old.leaves() &&
              q->amend_keep_priority(h, s.id, 0, s.qty, s.ack.ts))
            continue;
          q->remove(h);
        }
      }
      if (!q->place(s.id, 0, s.instrument, s.side, s.price, s.qty, o.queue_ahead, s.ack.ts, view)
               .valid())
        ++res_.model_full;
    }
    if (orig != nullptr) deactivate(*orig);
    Placed& p = placed_[index_of(s)];
    p.row = static_cast<std::int64_t>(res_.orders.size());
    p.active = true;
    active_.push_back(index_of(s));
    if (s.instrument.value >= active_by_inst_.size())
      active_by_inst_.resize(s.instrument.value + 1);
    active_by_inst_[s.instrument.value].push_back(index_of(s));
    res_.orders.push_back(std::move(o));
  }

  void deactivate(const OwnOrder& s) {
    Placed& p = placed_[index_of(s)];
    if (!p.active) return;
    p.active = false;
    active_.erase(std::remove(active_.begin(), active_.end(), index_of(s)), active_.end());
    if (s.instrument.value < active_by_inst_.size()) {
      auto& v = active_by_inst_[s.instrument.value];
      v.erase(std::remove(v.begin(), v.end(), index_of(s)), v.end());
    }
  }

  // The touch of `id` at (bid, ask), venue time t: marks the resting orders the opposite side
  // reached.
  void crossed(InstrumentId id, Price bid, Price ask, Timestamp t) {
    if (id.value >= active_by_inst_.size()) return;
    for (const std::uint32_t i : active_by_inst_[id.value]) {
      const OwnOrder& s = log_.orders[i];
      const Price opp = s.side == Side::Buy ? ask : bid;
      if (!opp.is_positive() || better(s.side, opp, s.price) || t < s.ack.ts) continue;
      FillCheckOrder* r = row(s);
      if (r != nullptr && !r->touch_crossed.valid()) r->touch_crossed = t;
    }
  }

  // What a public trade says about the resting orders it printed against: through their price,
  // at it, and the trade of a first live fill.
  void diagnose_trade(const TradeMsg& t, Timestamp ts) {
    const InstrumentId id = t.hdr.instrument;
    if (const auto e = exec_order_.find(t.trade_id); t.trade_id != 0 && e != exec_order_.end()) {
      const OwnOrder& s = log_.orders[e->second];
      FillCheckOrder* r = s.instrument == id ? row(s) : nullptr;
      if (r != nullptr) {
        ++res_.exec_matched;
        if (r->live_print == FillPrint::Missing) r->live_print = FillPrint::AtPrice;
      }
    }
    if (id.value >= active_by_inst_.size()) return;
    const Side maker = opposite(t.aggressor);
    for (const std::uint32_t i : active_by_inst_[id.value]) {
      const OwnOrder& s = log_.orders[i];
      if (s.side != maker || ts < s.ack.ts) continue;
      FillCheckOrder* r = row(s);
      if (r == nullptr) continue;
      const bool fill_ms = r->live_first_fill_ts.valid() && near(ts, r->live_first_fill_ts);
      if (better(t.aggressor, t.price, s.price)) {
        if (!r->traded_through.valid()) r->traded_through = ts;
        if (fill_ms) r->live_print = FillPrint::Through;
      } else if (t.price == s.price) {
        r->printed_at_px += t.qty;
        if (fill_ms && r->live_print == FillPrint::Missing) r->live_print = FillPrint::AtPrice;
      }
    }
  }
  void remove(const OwnOrder& s) {
    for (QueuePositionModel* q : model_ptrs_) {
      if (const auto h = q->find(s.id); h.valid()) q->remove(h);
    }
    deactivate(s);
  }

  // The queue each model has for the order as the venue fills it.
  void at_live_fill(const OwnOrder& s) {
    FillCheckOrder* r = row(s);
    if (r == nullptr) return;
    for (std::size_t k = 0; k < models_.size(); ++k) {
      if (const auto h = models_[k]->find(s.id); h.valid())
        r->model_ahead_at_live_fill[k] = models_[k]->get(h).ahead;
    }
  }

  void on_trade(const TradeMsg& t, Timestamp ts) {
    diagnose_trade(t, ts);
    const bool ms = log_.ms_order_times;
    // Millisecond end times: a trade after the last fill's trade id did not fill the order, even
    // inside the end's millisecond (Binance's exec id is the public trade id).
    if (ms) {
      for (std::size_t n = active_.size(); n-- > 0;) {
        const OwnOrder& s = log_.orders[active_[n]];
        if (s.ended && s.instrument == t.hdr.instrument && s.last_exec != 0 &&
            t.trade_id > s.last_exec && ts > s.end.ts)
          remove(s);
      }
    }
    for (std::size_t k = 0; k < models_.size(); ++k) {
      QueuePositionModel& q = *models_[k];
      q.on_trade(t.hdr.instrument,
                 t.price,
                 t.qty,
                 t.aggressor,
                 ts,
                 [&](QueuePositionModel::Handle32 h, QueuedOrder& o, Qty fill, Qty ahead) {
                   if (const OwnOrder* s = log_.find(o.cl_ord_id)) {
                     if (FillCheckOrder* r = row(*s)) {
                       r->model_filled[k] += fill;
                       if (!r->model_first_fill_ts[k].valid()) {
                         r->model_first_fill_ts[k] = ts;
                         r->model_ahead_at_fill[k] = ahead;
                         r->model_through[k] = better(t.aggressor, t.price, o.price) ? 1 : 0;
                       }
                       if (k == 0 && ms && !s->ack.from_recv && ts.ns < s->ack.ts.ns + kMs)
                         ++res_.ack_ties;
                       // After the end's millisecond start: a tie unless the trade id shows it is
                       // the fill's own trade or an earlier one.
                       if (k == 0 && ms && s->ended && !s->end.from_recv && ts > s->end.ts &&
                           (s->last_exec == 0 || t.trade_id > s->last_exec))
                         ++res_.end_ties;
                     }
                   }
                   if (o.leaves().is_zero()) q.remove(h);
                 });
    }
    if (TradeTape* tp = tape(t.hdr.instrument)) tp->add(t, ts);
  }

  FillCheckInputs in_;
  const FillCheckOptions& opt_;
  bool market_;
  std::vector<Price> ticks_;  // per instrument
  MidSeries mids_;
  std::unordered_map<std::uint64_t, std::uint32_t> exec_order_;  // first live fill's exec id
  std::vector<std::vector<std::uint32_t>> active_by_inst_;
  sim::EventBuf remap_buf_;
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<QueueTouch> touches_;
  std::vector<TradeTape> tapes_;
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

namespace {

void open_journal(JournalReader& reader, const std::string& path) {
  if (auto r = reader.open(path); !r) {
    throw std::runtime_error("cannot open " + path + ": " + reader.describe(r.error()));
  }
}

}  // namespace

FillCheckResult fill_check(JournalReader& reader,
                           std::span<const double> conservatism,
                           const FillCheckInputs& in) {
  FillCheckOptions opt;
  opt.inputs = in;
  return fill_check(reader, conservatism, opt);
}

FillCheckResult fill_check(JournalReader& reader,
                           std::span<const double> conservatism,
                           const FillCheckOptions& opt) {
  const OwnOrderLog log = collect_own_orders(reader);
  JournalReader market;
  if (!opt.market.empty()) open_journal(market, opt.market);
  Walker w(conservatism, opt, reader, log, !opt.market.empty());
  reader.for_each([&](const EventHeader* h) { w.on_event(*h); });
  reader.reset();
  if (!opt.market.empty()) {
    // The market journal's instruments in the session's table, by venue id and symbol.
    const std::span<const Instrument> mine = reader.instruments();
    std::vector<InstrumentId> map;
    for (const Instrument& m : market.instruments()) {
      InstrumentId id{};
      for (std::size_t i = 0; i < mine.size(); ++i) {
        if (mine[i].venue == m.venue && mine[i].symbol == m.symbol)
          id = InstrumentId{static_cast<std::uint32_t>(i)};
      }
      map.push_back(id);
    }
    market.for_each([&](const EventHeader* h) {
      w.on_market_event(
          *h, h->instrument.value < map.size() ? map[h->instrument.value] : InstrumentId{});
    });
  }
  return w.finish();  // the items point into both journals
}

FillCheckResult fill_check(const std::string& path,
                           std::span<const double> conservatism,
                           const FillCheckInputs& in) {
  FillCheckOptions opt;
  opt.inputs = in;
  return fill_check(path, conservatism, opt);
}

FillCheckResult fill_check(const std::string& path,
                           std::span<const double> conservatism,
                           const FillCheckOptions& opt) {
  JournalReader reader;
  open_journal(reader, path);
  return fill_check(reader, conservatism, opt);
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

std::string_view to_string(FillPrint p) noexcept {
  switch (p) {
    case FillPrint::None:
      return "none";
    case FillPrint::Missing:
      return "missing";
    case FillPrint::AtPrice:
      return "at_price";
    case FillPrint::Through:
      return "through";
  }
  return "?";
}

namespace {

std::string horizon_label(std::int64_t ns) {
  if (ns % 60'000'000'000 == 0) return fmt::format("{}m", ns / 60'000'000'000);
  if (ns % 1'000'000'000 == 0) return fmt::format("{}s", ns / 1'000'000'000);
  if (ns % 1'000'000 == 0) return fmt::format("{}ms", ns / 1'000'000);
  return fmt::format("{}us", ns / 1'000);
}

std::string value_text(double v) {
  return std::isnan(v) ? std::string() : fmt::format("{:.3f}", v);
}

std::string ts_text(Timestamp t) {
  return t.valid() ? std::to_string(t.ns) : std::string();
}

}  // namespace

std::string fill_check_csv(const FillCheckResult& r) {
  std::string out;
  auto it = std::back_inserter(out);
  out +=
      "cl_ord_id,instrument,side,price,qty,queue_ahead,ack_ts_ns,end_ts_ns,resting_ns,end,"
      "live_filled,live_first_fill_ts_ns,cancel_sent_ns,ticks_behind,touch_crossed_ns,"
      "traded_through_ns,printed_at_px,live_print,live_pre_move_bps";
  for (const std::int64_t h : r.horizons_ns)
    fmt::format_to(it, ",live_markout_{}_bps", horizon_label(h));
  for (const double c : r.conservatism) {
    fmt::format_to(it,
                   ",model_filled_c{0:.2f},model_first_fill_ts_ns_c{0:.2f},model_through_c{0:.2f},"
                   "model_pre_move_bps_c{0:.2f}",
                   c);
    for (const std::int64_t h : r.horizons_ns)
      fmt::format_to(it, ",model_markout_{}_bps_c{:.2f}", horizon_label(h), c);
  }
  out += '\n';
  const std::size_t nh = r.horizons_ns.size();
  for (const FillCheckOrder& o : r.orders) {
    fmt::format_to(it,
                   "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
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
                   o.live_first_fill_ts.ns,
                   ts_text(o.cancel_sent),
                   o.touch_known ? std::to_string(o.ticks_behind) : std::string(),
                   ts_text(o.touch_crossed),
                   ts_text(o.traded_through),
                   qty_text(o.printed_at_px),
                   to_string(o.live_print),
                   value_text(o.live_pre_move_bps));
    for (std::size_t h = 0; h < nh && h < o.live_markout_bps.size(); ++h)
      fmt::format_to(it, ",{}", value_text(o.live_markout_bps[h]));
    for (std::size_t k = 0; k < o.model_filled.size(); ++k) {
      fmt::format_to(it,
                     ",{},{},{},{}",
                     qty_text(o.model_filled[k]),
                     o.model_first_fill_ts[k].ns,
                     static_cast<int>(o.model_through[k]),
                     value_text(o.model_pre_move_bps[k]));
      for (std::size_t h = 0; h < nh; ++h)
        fmt::format_to(it, ",{}", value_text(o.model_markout_bps[k * nh + h]));
    }
    out += '\n';
  }
  return out;
}

// ---- diagnosis --------------------------------------------------------------------------------

namespace {

// Notional-weighted markouts and the pre-fill moves of a set of first fills.
struct FillSet {
  std::uint64_t n = 0;
  double notional = 0;
  std::vector<double> mo;     // sum of markout bps x notional, per horizon
  std::vector<double> mo_w;   // notional marked, per horizon
  std::vector<double> pre;    // pre-fill moves, bps
  std::uint64_t through = 0;  // came from a print through the price (model) / a sweep (live)
  std::uint64_t missing = 0;  // live: its trade is not in the recorded tape
  std::uint64_t after_cancel = 0;
  std::uint64_t crossed_before = 0;  // the opposite touch reached the price at or before the fill

  explicit FillSet(std::size_t nh) : mo(nh, 0.0), mo_w(nh, 0.0) {}
  void add(double qty, double px, const double* markout, double pre_move) {
    ++n;
    const double w = qty * px;
    notional += w;
    for (std::size_t h = 0; h < mo.size(); ++h) {
      if (std::isnan(markout[h])) continue;
      mo[h] += markout[h] * w;
      mo_w[h] += w;
    }
    if (!std::isnan(pre_move)) pre.push_back(pre_move);
  }
  [[nodiscard]] double markout(std::size_t h) const {
    return mo_w[h] > 0 ? mo[h] / mo_w[h] : kNoValue;
  }
};

double quantile(std::vector<double> v, double p) {
  if (v.empty()) return kNoValue;
  std::sort(v.begin(), v.end());
  const double x = p * static_cast<double>(v.size() - 1);
  const auto i = static_cast<std::size_t>(x);
  if (i + 1 >= v.size()) return v.back();
  return v[i] + (x - static_cast<double>(i)) * (v[i + 1] - v[i]);
}

std::string bps_text(double v) {
  return std::isnan(v) ? std::string("-") : fmt::format("{:+.2f}", v);
}

std::string share(std::uint64_t a, std::uint64_t b) {
  return b == 0 ? std::string("-")
                : fmt::format("{:.0f}%", 100.0 * static_cast<double>(a) / static_cast<double>(b));
}

// Orders of one bucket of a feature.
struct Bucket {
  const char* name;
  std::uint64_t orders = 0;
  std::uint64_t live = 0;
  std::uint64_t model = 0;
  std::uint64_t live_only = 0;
};

}  // namespace

std::string format_fill_diagnosis(const FillCheckResult& r, std::size_t k) {
  std::string out;
  auto it = std::back_inserter(out);
  if (k >= r.conservatism.size()) return out;
  const std::size_t nh = r.horizons_ns.size();
  FillSet both_live(nh);
  FillSet both_model(nh);
  FillSet live_only(nh);
  FillSet model_only(nh);
  const auto live_fill = [&](FillSet& f, const FillCheckOrder& o) {
    f.add(o.live_filled.to_double(),
          o.price.to_double(),
          o.live_markout_bps.data(),
          o.live_pre_move_bps);
    if (o.live_print == FillPrint::Through) ++f.through;
    if (o.live_print == FillPrint::Missing) ++f.missing;
    if (o.live_fill_after_cancel()) ++f.after_cancel;
    if (o.touch_crossed.valid() && o.touch_crossed <= o.live_first_fill_ts) ++f.crossed_before;
  };
  const auto model_fill = [&](FillSet& f, const FillCheckOrder& o) {
    f.add(o.model_filled[k].to_double(),
          o.price.to_double(),
          o.model_markout_bps.data() + k * nh,
          o.model_pre_move_bps[k]);
    if (o.model_through[k] != 0) ++f.through;
    if (o.touch_crossed.valid() && o.touch_crossed <= o.model_first_fill_ts[k]) ++f.crossed_before;
  };
  for (const FillCheckOrder& o : r.orders) {
    const bool live = o.live_filled.is_positive();
    const bool model = o.model_filled[k].is_positive();
    if (live && model) {
      live_fill(both_live, o);
      model_fill(both_model, o);
    } else if (live) {
      live_fill(live_only, o);
    } else if (model) {
      model_fill(model_only, o);
    }
  }

  fmt::format_to(it,
                 "diagnosis at conservatism {:.2f}: first fills, markouts notional-weighted in bps "
                 "(s * (mid(t + h) - price) / price), pre-fill move s * (mid(t) - mid(t - {})) "
                 "/ price\n",
                 r.conservatism[k],
                 horizon_label(r.pre_window.ns));
  if (!r.market.empty())
    fmt::format_to(it,
                   "market data from {} ({} messages on instruments the session lacks)\n",
                   r.market,
                   r.market_unmapped);
  fmt::format_to(it, "  {:<20} {:>6}", "fills", "n");
  for (const std::int64_t h : r.horizons_ns) fmt::format_to(it, " {:>8}", horizon_label(h));
  fmt::format_to(it,
                 " {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8}\n",
                 "pre p10",
                 "pre p50",
                 "pre p90",
                 "through",
                 "missing",
                 "crossed",
                 "cancel");
  const auto row = [&](const char* name, const FillSet& f, bool live) {
    fmt::format_to(it, "  {:<20} {:>6}", name, f.n);
    for (std::size_t h = 0; h < nh; ++h) fmt::format_to(it, " {:>8}", bps_text(f.markout(h)));
    fmt::format_to(it,
                   " {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8}\n",
                   bps_text(quantile(f.pre, 0.1)),
                   bps_text(quantile(f.pre, 0.5)),
                   bps_text(quantile(f.pre, 0.9)),
                   share(f.through, f.n),
                   live ? share(f.missing, f.n) : std::string("-"),
                   share(f.crossed_before, f.n),
                   live ? share(f.after_cancel, f.n) : std::string("-"));
  };
  row("both, live fill", both_live, true);
  row("both, model fill", both_model, false);
  row("live only", live_only, true);
  row("model only", model_only, false);
  fmt::format_to(it,
                 "  through: a sweep printed through the price in the live fill's millisecond (the "
                 "model's fill came from a print through it); missing: no print at or through "
                 "the price in the live fill's millisecond; crossed: the opposite touch had "
                 "reached the price by the fill; cancel: a cancel or replace had gone out before "
                 "the live fill. {} first live fills found by exec id in the trades.\n",
                 r.exec_matched);

  // Fill rates by feature: orders, filled live, filled by the model, live / model.
  const auto table = [&](const char* title, std::vector<Bucket> b, auto&& index) {
    for (const FillCheckOrder& o : r.orders) {
      const int i = index(o);
      if (i < 0) continue;
      Bucket& x = b[static_cast<std::size_t>(i)];
      ++x.orders;
      const bool live = o.live_filled.is_positive();
      const bool model = o.model_filled[k].is_positive();
      if (live) ++x.live;
      if (model) ++x.model;
      if (live && !model) ++x.live_only;
    }
    fmt::format_to(it,
                   "\n  {:<34} {:>8} {:>7} {:>7} {:>10} {:>9}\n",
                   title,
                   "orders",
                   "live",
                   "model",
                   "live/model",
                   "live only");
    for (const Bucket& x : b) {
      if (x.orders == 0) continue;
      fmt::format_to(
          it,
          "  {:<34} {:>8} {:>7} {:>7} {:>10} {:>9}\n",
          x.name,
          x.orders,
          x.live,
          x.model,
          x.model == 0
              ? std::string("-")
              : fmt::format("{:.2f}", static_cast<double>(x.live) / static_cast<double>(x.model)),
          x.live_only);
    }
  };
  table("queue ahead at the ack / order qty",
        {{"0"}, {"(0, 1]"}, {"(1, 5]"}, {"> 5"}},
        [](const FillCheckOrder& o) {
          if (o.queue_ahead.is_zero()) return 0;
          const double q = o.queue_ahead.to_double() / std::max(o.qty.to_double(), 1e-18);
          if (q <= 1) return 1;
          return q <= 5 ? 2 : 3;
        });
  table("ticks behind the touch at the ack",
        {{"inside (< 0)"}, {"at the touch"}, {"1"}, {"2-4"}, {">= 5"}, {"unknown"}},
        [](const FillCheckOrder& o) {
          if (!o.touch_known) return 5;
          const std::int32_t t = o.ticks_behind;
          if (t < 0) return 0;
          if (t <= 1) return t + 1;
          return t <= 4 ? 3 : 4;
        });
  table("opposite touch reached the price", {{"no"}, {"yes"}}, [](const FillCheckOrder& o) {
    return o.touch_crossed.valid() ? 1 : 0;
  });
  table("a trade printed through the price", {{"no"}, {"yes"}}, [](const FillCheckOrder& o) {
    return o.traded_through.valid() ? 1 : 0;
  });
  table("printed at the price / queue at ack",
        {{"nothing printed"}, {"< queue"}, {">= queue"}},
        [](const FillCheckOrder& o) {
          if (o.printed_at_px.is_zero()) return 0;
          return o.printed_at_px < o.queue_ahead ? 1 : 2;
        });
  table("resting time",
        {{"< 100ms"}, {"100ms - 1s"}, {"1s - 10s"}, {">= 10s"}},
        [](const FillCheckOrder& o) {
          const std::int64_t ns = o.resting().ns;
          if (ns < 100'000'000) return 0;
          if (ns < 1'000'000'000) return 1;
          return ns < 10'000'000'000 ? 2 : 3;
        });
  table("cancel or replace sent", {{"no"}, {"yes"}}, [](const FillCheckOrder& o) {
    return o.cancel_sent.valid() ? 1 : 0;
  });
  return out;
}

}  // namespace fastmm::bt
