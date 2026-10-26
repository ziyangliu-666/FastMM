#include "fastmm/backtest/population.hpp"

#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/queue_model.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/sim/venue_order.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/quoting.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace fastmm::bt {

namespace {

Timestamp event_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

// ---- one lane -----------------------------------------------------------------------------------

// The OMS states a quote can be in (core/oms.hpp); None: the slot holds no live order.
enum class St : std::uint8_t { None, PendingNew, Live, PartFilled, PendingReplace, PendingCancel };
constexpr bool working(St s) noexcept {
  return s == St::Live || s == St::PartFilled;
}
constexpr bool pending(St s) noexcept {
  return s == St::PendingNew || s == St::PendingReplace || s == St::PendingCancel;
}

// The engine's order in a slot (Oms::Order, the fields a quote uses).
struct EngOrder {
  St st = St::None;
  bool old_canceled = false;  // Order::kReplaceOldCanceled
  std::uint64_t id = 0;
  std::uint64_t pend_id = 0;
  Price px{};
  Price pend_px{};
  Qty qty{};
  Qty cum{};
  Qty pend_qty{};
  [[nodiscard]] Qty leaves() const noexcept { return qty - cum; }
};

// The quote manager's slot (QuoteManager::Slot).
struct SlotQ {
  Price want_px{};
  Qty want_qty{};
  bool apply_want = false;
  Timestamp last_requote{};
};

// The simulated venue's order (QueuedOrder under l3_queue).
struct VenOrder {
  bool live = false;
  std::uint64_t id = 0;
  Price px{};
  Qty qty{};
  Qty cum{};
  std::uint64_t prio = 0;
  Timestamp placed{};
  Qty crossed{};
  [[nodiscard]] Qty leaves() const noexcept { return qty - cum; }
};

struct LaneInst {
  EngOrder eng[2];
  SlotQ slot[2];
  VenOrder ven[2];
  Qty pos{};
  bool pulled = false;
  bool listed = false;  // in Pass::resting_[inst]
};

enum class K : std::uint8_t { New, Cancel, Replace, Ack, Reject, CancelAck, CancelReject, Fill };

struct Msg {
  Timestamp ts;   // arrival
  Timestamp vts;  // venue -> engine: the venue time it was sent at
  std::uint64_t id = 0;
  std::uint64_t id2 = 0;  // Replace: the original id
  Price px{};
  Qty qty{};
  Qty cum{};
  Notional fee{};
  std::uint32_t inst = 0;
  Side side = Side::Buy;
  K kind = K::Ack;
  Liquidity liq = Liquidity::Unknown;
  RejectReason reason = RejectReason::None;
};

// A FIFO of messages in arrival order (each path has one fixed latency, so arrivals never pass
// each other).
class MsgQueue {
 public:
  [[nodiscard]] bool empty() const noexcept { return head_ == q_.size(); }
  [[nodiscard]] const Msg& front() const noexcept { return q_[head_]; }
  void pop() noexcept {
    if (++head_ == q_.size()) {
      q_.clear();
      head_ = 0;
    }
  }
  void push(const Msg& m) { q_.push_back(m); }

 private:
  std::vector<Msg> q_;
  std::size_t head_ = 0;
};

struct Lane {
  LaneSpec spec;
  std::uint32_t wake_out = 0;  // Pass::wq_ class of the order path
  std::uint32_t wake_in = 0;   // and of the ack path
  std::vector<LaneInst> inst;
  MsgQueue out;  // engine -> venue
  MsgQueue in;   // venue -> engine
  Timestamp last_in{};
  std::uint64_t next_id = 1;
  LaneResult res;
  Notional cash{};
  Notional fees{};
};

struct Rec {
  std::uint64_t prio;
  Price px;
  Qty qty;
  Side side;
};

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
  return h;
}

// ---- one pass: the shared market and its lanes --------------------------------------------------

class Pass {
 public:
  Pass(const BacktestConfig& cfg, std::span<const LaneSpec> specs)
      : cfg_(cfg),
        n_(cfg.instruments.size()),
        world_(n_),
        books_(std::make_unique<L2Book<256>[]>(n_)),
        recs_(n_),
        resting_(n_),
        last_mid_(n_),
        bound_(n_),
        crossed_n_(n_),
        md_valid_(n_),
        md_mid_(n_) {
    world_.set_tape_trades(false);
    lanes_.resize(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
      lanes_[i].spec = specs[i];
      lanes_[i].inst.resize(n_);
      if (specs[i].requote_threshold_ticks <= 0) thr_zero_ = true;
      lanes_[i].wake_out = wake_class(specs[i].order_latency);
      lanes_[i].wake_in = wake_class(specs[i].ack_latency);
    }
    lm_.assign(n_ * lanes_.size(), 0);
    thr_.resize(n_ * lanes_.size());
    for (std::size_t i = 0; i < n_; ++i) {
      for (std::size_t k = 0; k < lanes_.size(); ++k)
        thr_[i * lanes_.size() + k] =
            instrument(static_cast<std::uint32_t>(i)).ticks(specs[k].requote_threshold_ticks).raw;
    }
    qp_ = cfg.engine.quotes;
    stp_ = cfg.engine.risk.stp;
  }

  PopulationStats run(sim::MdSource& src) {
    PopulationStats st;
    std::unique_ptr<sim::VenueOrderSource> ordered;
    sim::MdSource* events = &src;
    if (cfg_.reorder_window.ns > 0) {
      ordered = std::make_unique<sim::VenueOrderSource>(src, cfg_.reorder_window);
      events = ordered.get();
    }
    // The engine's clock (SimClock): from the data's start, never back. A message whose time is
    // behind it (a book update held back for the recorded order) is taken at the clock.
    clock_ = src.start_ts().valid() ? src.start_ts() : Timestamp{};
    const EventHeader* ev = events->next();
    for (;;) {
      const Timestamp ts_src = ev != nullptr ? event_time(*ev) : Timestamp::max();
      const Timestamp ts_md = mdq_.empty() ? Timestamp::max() : mdq_.front().arrival;
      const auto [w_ns, w_where] = next_wake();
      const Timestamp ts_w{w_ns};
      if (ts_src == Timestamp::max() && ts_md == Timestamp::max() && ts_w == Timestamp::max())
        break;
      const Timestamp t = std::min(ts_src, std::min(ts_w, ts_md));
      if (t > clock_) clock_ = t;
      if (ts_src <= ts_w && ts_src <= ts_md) {
        source_event(*ev, ts_src);
        ++st.source_events;
        ev = events->next();
      } else if (ts_w <= ts_md) {
        ++st.lane_wakes;
        run_lane(lanes_[pop_wake(w_where)], ts_w);
      } else {
        md_event(st);
        ++st.md_events;
      }
    }
    st.lane_requotes = requotes_;
    return st;
  }

  void results(std::vector<LaneResult>& out, std::span<const std::size_t> idx) {
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
      Lane& l = lanes_[i];
      LaneResult r = std::move(l.res);
      r.cash = l.cash;
      r.fees = l.fees;
      Notional pnl = l.cash - l.fees;
      r.positions.resize(n_);
      for (std::size_t k = 0; k < n_; ++k) {
        const Qty pos = l.inst[k].pos;
        r.positions[k] = pos;
        r.position_sum += pos.raw < 0 ? -pos.raw : pos.raw;
        // Marked at the venue's final mid, as the run's PnL decomposition marks it; a book with one
        // side left marks nothing.
        const Price final_mid = world_.books()[k].mid();
        if (!pos.is_zero() && final_mid.is_positive()) pnl = pnl + mul(final_mid, pos);
      }
      r.net_pnl = pnl;
      out[idx[i]] = std::move(r);
    }
  }

  std::vector<std::vector<LaneFill>>* record = nullptr;  // per lane, when set

  // Writes the tape as it runs (compile_tape: a pass without lanes).
  void set_tape(Tape* t) {
    tape_ = t;
    t->recs.assign(n_, {});
    tape_last_.assign(n_, TapeLast{});
    tape_levels_.assign(n_, lk::Levels{});
    tape_lv_idx_.assign(n_, -1);
  }

 private:
  struct TapeEntry {
    Timestamp arrival;
    std::vector<std::uint64_t> bytes;
    [[nodiscard]] const EventHeader& hdr() const noexcept {
      return *reinterpret_cast<const EventHeader*>(bytes.data());
    }
  };
  struct Held {
    Timestamp recorded;
    Timestamp venue;
    bool has_md = false;
    std::vector<std::uint64_t> bytes;
  };

  // ---- the tape ---------------------------------------------------------------------------------

  struct TapeLast {
    std::int64_t mid = -1;
    std::int64_t bb = -1;
    std::int64_t ba = -1;
  };
  void tape_delta(std::uint32_t i, Timestamp now) {
    const L2Book<256>& book = world_.books()[i];
    const Instrument& inst = instrument(i);
    lk::Levels lv{};
    lv.exact = 1;
    for (const Side side : {Side::Buy, Side::Sell}) {
      const auto s = static_cast<std::size_t>(side);
      std::uint32_t k = 0;
      book.for_each_level(side, [&](const Level& l) {
        if (k == lk::kLevels) {
          lv.more = static_cast<std::uint8_t>(lv.more | (1U << s));
          return false;
        }
        if (k == 0) lv.best[s] = l.price.raw;
        const std::int64_t off =
            (side == Side::Buy ? lv.best[s] - l.price.raw : l.price.raw - lv.best[s]) /
            inst.tick.raw;
        const std::int64_t lots = l.qty.raw / inst.lot.raw;
        if (off < 0 || off > 65535 || lots * inst.lot.raw != l.qty.raw || lots > INT32_MAX ||
            (side == Side::Buy ? lv.best[s] - off * inst.tick.raw
                               : lv.best[s] + off * inst.tick.raw) != l.price.raw)
          lv.exact = 0;
        lv.off[s][k] = static_cast<std::uint16_t>(off);
        lv.qty[s][k] = static_cast<std::int32_t>(lots);
        ++k;
        return true;
      });
      lv.n[s] = static_cast<std::uint8_t>(k);
    }
    // An update that leaves the instrument's best levels as they were reuses their entry.
    if (tape_lv_idx_[i] < 0 || std::memcmp(&lv, &tape_levels_[i], sizeof lv) != 0) {
      tape_levels_[i] = lv;
      tape_lv_idx_[i] = static_cast<std::int64_t>(tape_->levels.size());
      tape_->levels.push_back(lv);
    }
    lk::Rec r{};
    r.t = now.ns;
    r.kind = lk::Delta;
    r.levels = static_cast<std::uint32_t>(tape_lv_idx_[i]);
    r.a = lv.n[0] > 0 ? lv.best[0] : 0;
    r.c = lv.n[1] > 0 ? lv.best[1] : 0;
    tape_->recs[i].push_back(r);
  }
  void tape_md(std::uint32_t i, Timestamp arrival, Price mid, const L2Book<256>& b) {
    TapeLast& last = tape_last_[i];
    const std::int64_t bb = b.best_bid().price.raw;
    const std::int64_t ba = b.best_ask().price.raw;
    if (mid.raw == last.mid && bb == last.bb && ba == last.ba) return;
    last = TapeLast{mid.raw, bb, ba};
    lk::Rec r{};
    r.t = arrival.ns;
    r.a = clock_.ns;
    r.b = mid.raw;
    r.c = bb;
    r.d = ba;
    r.kind = lk::Md;
    tape_->recs[i].push_back(r);
  }

  // ---- the shared market ------------------------------------------------------------------------

  void source_event(const EventHeader& md, Timestamp now) {
    world_.before(md, now);
    if (tape_ != nullptr && md.instrument.value < n_ &&
        (md.type == EventType::BookDelta || md.type == EventType::BookSnapshot))
      tape_delta(md.instrument.value, now);
    const std::uint32_t i = md.instrument.value;
    bool forward_md = true;
    if (i < n_) {
      switch (md.type) {
        case EventType::BookDelta:
        case EventType::BookSnapshot:
          venue_cross(i, now);
          break;
        case EventType::OrderAddL3:
        case EventType::OrderExecL3:
        case EventType::OrderCancelL3:
        case EventType::OrderReplaceL3:
          venue_l3(md, now);
          forward_md = false;
          break;
        default:
          break;
      }
    } else if (md.type == EventType::OrderAddL3 || md.type == EventType::OrderExecL3 ||
               md.type == EventType::OrderCancelL3 || md.type == EventType::OrderReplaceL3) {
      forward_md = true;  // as SimTransport: an instrument outside the table goes to the engine
    }
    if (md.seq != 0) {
      forward_in_order(forward_md ? &md : nullptr, md.seq, md.recv_ts, now);
    } else if (forward_md) {
      forward(md, now);
    }
    world_.after(md, now);
  }

  // SimTransport::forward_in_order: the engine gets the feed in recorded order.
  void forward_in_order(const EventHeader* md,
                        std::uint64_t seq,
                        Timestamp recorded,
                        Timestamp now) {
    if (seq > next_forward_) {
      Held h;
      h.recorded = recorded;
      h.venue = now;
      h.has_md = md != nullptr;
      if (md != nullptr) copy(*md, h.bytes);
      held_.emplace(seq, std::move(h));
      return;
    }
    if (md != nullptr) forward(*md, now);
    if (seq == next_forward_) ++next_forward_;
    for (;;) {
      const auto it = held_.begin();
      if (it == held_.end() || it->first > next_forward_) break;
      if (it->second.has_md)
        forward(*reinterpret_cast<const EventHeader*>(it->second.bytes.data()), it->second.venue);
      if (it->first == next_forward_) ++next_forward_;
      held_.erase(it);
    }
  }
  static void copy(const EventHeader& md, std::vector<std::uint64_t>& b) {
    b.resize((md.len + 7) / 8);
    std::memcpy(b.data(), &md, md.len);
  }
  // SimTransport::forward: every message moves the wire's arrival clock; the engine acts on the
  // book updates of its instruments only.
  void forward(const EventHeader& md, Timestamp now) {
    Timestamp arrival = now + cfg_.transport.md_in.fixed;
    if (arrival < last_md_arrival_) arrival = last_md_arrival_;
    last_md_arrival_ = arrival;
    if (md.instrument.value >= n_) return;
    if (md.type != EventType::BookDelta && md.type != EventType::BookSnapshot) return;
    TapeEntry e;
    e.arrival = arrival;
    if (!free_bytes_.empty()) {
      e.bytes = std::move(free_bytes_.back());
      free_bytes_.pop_back();
    }
    copy(md, e.bytes);
    auto* h = reinterpret_cast<EventHeader*>(e.bytes.data());
    h->exch_ts = now;
    h->recv_ts = arrival;
    mdq_.push_back(std::move(e));
  }

  // The engine takes the next market-data message: its book, then every lane's strategy.
  void md_event(PopulationStats&) {
    TapeEntry e = std::move(mdq_.front());
    mdq_.pop_front();
    const auto& d = msg_cast<BookDeltaMsg>(&e.hdr());
    const std::uint32_t i = d.hdr.instrument.value;
    L2Book<256>& b = books_[i];
    b.apply_delta(d);
    const Timestamp now = clock_;
    const bool valid = b.is_valid();
    const Price mid = valid ? b.mid() : Price{};
    free_bytes_.push_back(std::move(e.bytes));
    if (valid) last_mid_[i] = mid;
    if (tape_ != nullptr) tape_md(i, e.arrival, valid ? mid : Price{}, b);
    // A valid book whose mid did not move since the instrument's last update: every lane took that
    // update (requoted at this mid, or kept quotes within its threshold of it) or requoted since at
    // the same mid (a fill), so none moves now.
    if (valid && md_valid_[i] != 0 && mid == md_mid_[i] && !thr_zero_) return;
    md_valid_[i] = valid ? 1 : 0;
    md_mid_[i] = mid;
    on_book_all(i, now);
  }

  // ---- venue side -------------------------------------------------------------------------------

  void venue_l3(const EventHeader& md, Timestamp now) {
    auto& recs = recs_[md.instrument.value];
    const std::uint32_t i = md.instrument.value;
    const auto take = [&](std::uint64_t ref, Qty q) {
      const auto it = recs.find(ref);
      if (it == recs.end()) return;
      Rec& o = it->second;
      if (q.is_zero() || q > o.qty) q = o.qty;
      o.qty -= q;
      if (o.qty.is_zero()) recs.erase(it);
    };
    switch (md.type) {
      case EventType::OrderAddL3: {
        const auto& m = msg_cast<OrderAddL3Msg>(&md);
        if (!m.qty.is_positive()) return;
        recs[m.order_ref] = Rec{2 * ++l3_seq_, m.price, m.qty, m.side};
        if (tape_ != nullptr) tape_->adds.push_back(now.ns);
        return;
      }
      case EventType::OrderExecL3: {
        const auto& m = msg_cast<OrderExecL3Msg>(&md);
        const auto it = recs.find(m.order_ref);
        if (it == recs.end()) return;
        Rec& o = it->second;
        const Qty q = min(m.exec_qty, o.qty);
        const Price at = m.exec_price.is_positive() ? m.exec_price : o.px;
        venue_execute(i, o.side, o.px, at, q, o.prio, now);
        if (tape_ != nullptr) {
          lk::Rec r{};
          r.t = now.ns;
          r.a = o.px.raw;
          r.b = static_cast<std::int64_t>(o.prio);
          r.c = at.raw;
          r.d = q.raw;
          r.kind = lk::Exec;
          r.side = static_cast<std::uint8_t>(o.side);
          tape_->recs[i].push_back(r);
        }
        o.qty -= q;
        if (o.qty.is_zero()) recs.erase(it);
        return;
      }
      case EventType::OrderCancelL3: {
        const auto& m = msg_cast<OrderCancelL3Msg>(&md);
        take(m.order_ref, m.canceled_qty);
        return;
      }
      case EventType::OrderReplaceL3: {
        const auto& m = msg_cast<OrderReplaceL3Msg>(&md);
        const auto it = recs.find(m.old_order_ref);
        if (it == recs.end()) return;
        const Side side = it->second.side;
        take(m.old_order_ref, Qty{});
        if (!m.qty.is_positive()) return;
        recs[m.new_order_ref] = Rec{2 * ++l3_seq_, m.price, m.qty, side};
        if (tape_ != nullptr) tape_->adds.push_back(now.ns);
        return;
      }
      default:
        return;
    }
  }

  // QueuePositionModel::on_l3_execute for every lane: one order per side, so the execution's
  // quantity goes to it alone.
  void venue_execute(std::uint32_t i,
                     Side side,
                     Price r_px,
                     Price exec_px,
                     Qty q,
                     std::uint64_t r_prio,
                     Timestamp now) {
    // Only an order at or better than the executed one can be reached.
    Price& bound = bound_[i][static_cast<std::size_t>(side)];
    if (bound.is_zero() || better(side, r_px, bound)) return;
    Price exact{};
    auto& list = resting_[i];
    for (std::size_t k = 0; k < list.size();) {
      Lane& l = lanes_[list[k]];
      VenOrder& o = l.inst[i].ven[static_cast<std::size_t>(side)];
      if (o.live &&
          l3_execution_reaches(side, o.px, o.prio, r_px, r_prio, exec_px) == L3Reach::Fills &&
          now >= o.placed) {
        const Qty fill = min(q, o.leaves());
        if (fill.is_positive()) {
          o.cum += fill;
          venue_fill(l, i, side, o, fill, Liquidity::Maker, o.px, now);
        }
      }
      if (o.live && (exact.is_zero() || better(side, o.px, exact))) exact = o.px;
      if (unlist_if_idle(l, i, list, k)) continue;
      ++k;
    }
    bound = exact;
  }

  // queue_apply_cross for every lane resting on the instrument.
  void venue_cross(std::uint32_t i, Timestamp now) {
    auto& list = resting_[i];
    if (list.empty()) return;
    const L2Book<256>& book = world_.books()[i];
    const QueueTouch& t = world_.touches()[i];
    Price best[2];
    for (const Side s : {Side::Buy, Side::Sell}) {
      for_each_venue_level(book, t, s, now, [&](const Level& l) {
        best[static_cast<std::size_t>(s)] = l.price;
        return false;
      });
    }
    // Skip the lanes when no bid of theirs can reach the asks, no ask the bids, and no order has a
    // positive `crossed` to take back.
    std::array<Price, 2>& bd = bound_[i];
    const bool bids_reach = !bd[0].is_zero() && best[1].is_positive() && bd[0] >= best[1];
    const bool asks_reach = !bd[1].is_zero() && best[0].is_positive() && bd[1] <= best[0];
    if (!bids_reach && !asks_reach && crossed_n_[i] == 0) return;
    std::array<Price, 2> exact{};
    const auto crossing = [&](Side side, Price px) {
      const Side opp = opposite(side);
      const Price b = best[static_cast<std::size_t>(opp)];
      Qty q{};
      if (!b.is_positive() || !at_or_better(opp, b, px)) return q;
      for_each_venue_level(book, t, opp, now, [&](const Level& l) {
        if (!at_or_better(opp, l.price, px)) return false;
        q += l.qty;
        return true;
      });
      return q;
    };
    for (std::size_t k = 0; k < list.size();) {
      Lane& l = lanes_[list[k]];
      for (const Side s : {Side::Buy, Side::Sell}) {
        VenOrder& o = l.inst[i].ven[static_cast<std::size_t>(s)];
        if (!o.live) continue;
        const Qty c = crossing(s, o.px);
        if (now < o.placed) {
          if (c > o.crossed) set_crossed(i, o, c);
        } else if (c > o.crossed) {
          const Qty fresh = c - o.crossed;
          set_crossed(i, o, c);
          const Qty fill = min(fresh, o.leaves());
          if (fill.is_positive()) {
            o.cum += fill;
            venue_fill(l, i, s, o, fill, Liquidity::Maker, o.px, now);
          }
        } else {
          set_crossed(i, o, c);
        }
        Price& e = exact[static_cast<std::size_t>(s)];
        if (o.live && (e.is_zero() || better(s, o.px, e))) e = o.px;
      }
      if (unlist_if_idle(l, i, list, k)) continue;
      ++k;
    }
    bd = exact;
  }

  // `crossed` of a live venue order, keeping crossed_n_.
  void set_crossed(std::uint32_t i, VenOrder& o, Qty c) noexcept {
    if (o.crossed.is_positive() != c.is_positive()) {
      if (c.is_positive()) {
        ++crossed_n_[i];
      } else {
        --crossed_n_[i];
      }
    }
    o.crossed = c;
  }
  // A venue order leaves the book.
  void ven_drop(std::uint32_t i, VenOrder& o) noexcept {
    if (o.live && o.crossed.is_positive()) --crossed_n_[i];
    o.live = false;
  }

  bool unlist_if_idle(Lane& l,
                      std::uint32_t i,
                      std::vector<std::uint32_t>& list,
                      std::size_t k) noexcept {
    LaneInst& li = l.inst[i];
    if (li.ven[0].live || li.ven[1].live) return false;
    li.listed = false;
    list[k] = list.back();
    list.pop_back();
    return true;
  }
  void list(Lane& l, std::uint32_t i) {
    LaneInst& li = l.inst[i];
    if (li.listed) return;
    li.listed = true;
    resting_[i].push_back(static_cast<std::uint32_t>(&l - lanes_.data()));
  }

  // SimTransport::queue_fill + emit_fill.
  void venue_fill(Lane& l,
                  std::uint32_t i,
                  Side side,
                  VenOrder& o,
                  Qty fill,
                  Liquidity liq,
                  Price px,
                  Timestamp now) {
    Msg m;
    m.kind = K::Fill;
    m.id = o.id;
    m.inst = i;
    m.side = side;
    m.px = px;
    m.qty = fill;
    m.cum = o.cum;
    m.liq = liq;
    m.fee = cfg_.transport.fees.schedule(InstrumentId{i})
                .fee(cfg_.instruments.get(InstrumentId{i}).notional(px, fill), liq);
    if (o.leaves().is_zero()) ven_drop(i, o);
    to_engine(l, m, now);
  }

  void to_engine(Lane& l, Msg m, Timestamp venue_ts) {
    Timestamp arrival = venue_ts + l.spec.ack_latency;
    if (arrival < l.last_in) arrival = l.last_in;
    l.last_in = arrival;
    m.ts = arrival;
    m.vts = venue_ts;
    l.in.push(m);
    wake(l, arrival, l.wake_in);
  }
  void to_venue(Lane& l, Msg m, Timestamp now) {
    m.ts = now + l.spec.order_latency;
    l.out.push(m);
    wake(l, m.ts, l.wake_out);
  }
  // A lane's message arrives at `t`. Each latency path adds its latency to the time the message was
  // sent, which never goes back, so the wake-ups of one latency come in time order: one FIFO per
  // latency, and a heap for anything that would not.
  void wake(Lane& l, Timestamp t, std::uint32_t cls) {
    const auto id = static_cast<std::uint32_t>(&l - lanes_.data());
    WakeQ& q = wq_[cls];
    if (q.head == q.v.size() || q.v.back().first <= t.ns) {
      if (q.head != q.v.size() && q.v.back().first == t.ns && q.v.back().second == id) return;
      q.v.emplace_back(t.ns, id);
      return;
    }
    heap_.emplace_back(t.ns, id);
    std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  }
  std::uint32_t wake_class(Duration lat) {
    for (std::uint32_t c = 0; c < wq_lat_.size(); ++c) {
      if (wq_lat_[c] == lat) return c;
    }
    wq_lat_.push_back(lat);
    wq_.emplace_back();
    return static_cast<std::uint32_t>(wq_.size() - 1);
  }
  // The earliest wake-up: its time, and where it is (class index, or wq_.size() for the heap).
  [[nodiscard]] std::pair<std::int64_t, std::size_t> next_wake() const noexcept {
    std::int64_t t = Timestamp::max().ns;
    std::size_t where = wq_.size() + 1;
    for (std::size_t c = 0; c < wq_.size(); ++c) {
      const WakeQ& q = wq_[c];
      if (q.head != q.v.size() && q.v[q.head].first < t) {
        t = q.v[q.head].first;
        where = c;
      }
    }
    if (!heap_.empty() && heap_.front().first < t) {
      t = heap_.front().first;
      where = wq_.size();
    }
    return {t, where};
  }
  std::uint32_t pop_wake(std::size_t where) {
    if (where == wq_.size()) {
      std::pop_heap(heap_.begin(), heap_.end(), std::greater<>{});
      const std::uint32_t id = heap_.back().second;
      heap_.pop_back();
      return id;
    }
    WakeQ& q = wq_[where];
    const std::uint32_t id = q.v[q.head].second;
    if (++q.head == q.v.size()) {
      q.v.clear();
      q.head = 0;
    }
    return id;
  }

  void emit(Lane& l,
            K kind,
            std::uint32_t i,
            Side side,
            std::uint64_t id,
            Timestamp now,
            Qty cum = Qty{},
            RejectReason r = RejectReason::None) {
    Msg m;
    m.kind = kind;
    m.id = id;
    m.inst = i;
    m.side = side;
    m.cum = cum;
    m.reason = r;
    to_engine(l, m, now);
  }

  // A message reaches the venue: SimTransport::queue_new / queue_cancel / queue_replace.
  void venue_arrival(Lane& l, const Msg& m) {
    const Timestamp now = m.ts;
    const std::uint32_t i = m.inst;
    VenOrder& o = l.inst[i].ven[static_cast<std::size_t>(m.side)];
    switch (m.kind) {
      case K::New:
        venue_new(l, i, m.side, m.id, m.px, m.qty, true, now);
        return;
      case K::Cancel: {
        if (!o.live || o.id != m.id) {
          emit(l, K::CancelReject, i, m.side, m.id, now);
          return;
        }
        ven_drop(i, o);
        emit(l, K::CancelAck, i, m.side, o.id, now, o.cum);
        return;
      }
      case K::Replace: {
        if (!o.live || o.id != m.id2) {
          emit(l, K::CancelReject, i, m.side, m.id2, now);
          emit(l, K::Reject, i, m.side, m.id, now, Qty{}, RejectReason::VenueUnknownOrder);
          return;
        }
        if (m.px == o.px && m.qty <= o.leaves()) {
          const std::uint64_t old_id = o.id;
          const Qty old_cum = o.cum;
          o.id = m.id;
          o.qty = m.qty;
          o.cum = Qty{};
          if (now > o.placed) o.placed = now;
          emit(l, K::CancelAck, i, m.side, old_id, now, old_cum);
          emit(l, K::Ack, i, m.side, m.id, now);
          return;
        }
        ven_drop(i, o);
        emit(l, K::CancelAck, i, m.side, o.id, now, o.cum);
        venue_new(l, i, m.side, m.id, m.px, m.qty, false, now);
        return;
      }
      default:
        return;
    }
  }

  void venue_new(Lane& l,
                 std::uint32_t i,
                 Side side,
                 std::uint64_t id,
                 Price px,
                 Qty qty,
                 bool post_only,
                 Timestamp now) {
    if (!qty.is_positive() || !px.is_positive()) {
      emit(l, K::Reject, i, side, id, now, Qty{}, RejectReason::InvalidTick);
      return;
    }
    const L2Book<256>& book = world_.books()[i];
    const QueueTouch& t = world_.touches()[i];
    const Side opp = opposite(side);
    Level best{};
    for_each_venue_level(book, t, opp, now, [&](const Level& lv) {
      best = lv;
      return false;
    });
    const bool crosses =
        best.qty.is_positive() && (side == Side::Buy ? px >= best.price : px <= best.price);
    if (post_only && crosses) {
      emit(l, K::Reject, i, side, id, now, Qty{}, RejectReason::PostOnlyWouldCross);
      return;
    }
    emit(l, K::Ack, i, side, id, now);
    VenOrder& o = l.inst[i].ven[static_cast<std::size_t>(side)];
    ven_drop(i, o);  // a slot's venue order has ended before the engine sends a new one
    o = VenOrder{};
    o.id = id;
    o.px = px;
    o.qty = qty;
    Qty leaves = qty;
    Qty cum{};
    if (crosses) {
      for_each_venue_level(book, t, opp, now, [&](const Level& lv) {
        if (leaves.is_zero()) return false;
        if (!at_or_better(opp, lv.price, px)) return false;
        const Qty q = min(leaves, lv.qty);
        cum += q;
        leaves -= q;
        o.cum = cum;
        venue_fill(l, i, side, o, q, Liquidity::Taker, lv.price, now);
        return true;
      });
    }
    if (leaves.is_zero()) {
      o.live = false;
      return;
    }
    o.live = true;
    o.cum = cum;
    set_crossed(i, o, cum);
    o.placed = now;
    o.prio = 2 * l3_seq_ + 1;
    Price& b = bound_[i][static_cast<std::size_t>(side)];
    if (b.is_zero() || better(side, px, b)) b = px;
    list(l, i);
  }

  // ---- engine side ------------------------------------------------------------------------------

  void run_lane(Lane& l, Timestamp t) {
    for (;;) {
      const Timestamp ta = l.out.empty() ? Timestamp::max() : l.out.front().ts;
      const Timestamp ti = l.in.empty() ? Timestamp::max() : l.in.front().ts;
      if (ta <= ti && ta <= t) {
        const Msg m = l.out.front();
        l.out.pop();
        venue_arrival(l, m);
      } else if (ti <= t) {
        const Msg m = l.in.front();
        l.in.pop();
        engine_msg(l, m);
      } else {
        return;
      }
    }
  }

  [[nodiscard]] const Instrument& instrument(std::uint32_t i) const noexcept {
    return cfg_.instruments.get(InstrumentId{i});
  }

  // BasicMM::on_book for every lane: a pull on an invalid book, else a requote where the mid moved
  // its threshold away from the lane's last quoted mid. The lanes' last mids and thresholds of one
  // instrument are contiguous, so the test runs over arrays.
  void on_book_all(std::uint32_t i, Timestamp now) {
    const std::size_t n_lanes = lanes_.size();
    if (n_lanes == 0) return;  // compile_tape: a pass without lanes
    std::int64_t* lm = &lm_[i * n_lanes];
    const L2Book<256>& b = books_[i];
    if (!b.is_valid()) {
      for (std::size_t k = 0; k < n_lanes; ++k) {
        pull(lanes_[k], i, now);
        lm[k] = 0;
      }
      return;
    }
    const std::int64_t mid = b.mid().raw;
    const std::int64_t* th = &thr_[i * n_lanes];
    for (std::size_t k = 0; k < n_lanes; ++k) {
      const std::int64_t last = lm[k];
      const std::int64_t d = mid >= last ? mid - last : last - mid;
      if (last > 0 && d < th[k]) continue;
      requote(lanes_[k], i, now);
    }
  }

  // BasicMM::requote: compute_quotes, keep_passive, set_quotes.
  void requote(Lane& l, std::uint32_t i, Timestamp now) {
    ++requotes_;
    LaneInst& li = l.inst[i];
    const L2Book<256>& b = books_[i];
    const Instrument& inst = instrument(i);
    const Price mid = b.mid();
    const LaneSpec& p = l.spec;
    DesiredQuotes q;
    if (!p.quote_qty.is_zero()) {
      const Qty position = li.pos;
      const std::int64_t inventory_units = position / p.quote_qty;
      const Price half = mid * p.half_spread;
      const Price centre = mid - mid * (p.skew * inventory_units);
      const Qty qty = inst.round_qty(p.quote_qty);
      const bool can_buy = inventory_allows(Side::Buy, position, p.quote_qty, p.max_inventory);
      const bool can_sell = inventory_allows(Side::Sell, position, p.quote_qty, p.max_inventory);
      if (can_buy) q.bid(inst.round_price(centre - half, Side::Buy), qty);
      if (can_sell) q.ask(inst.round_price(centre + half, Side::Sell), qty);
      q.uncross(inst.tick);
    }
    keep_passive(q, b.best_bid().price, b.best_ask().price, inst.tick);
    // set_quotes -> QuoteManager::reconcile (level 0 of each side; no other level is ever used)
    li.pulled = false;
    for (const Side s : {Side::Buy, Side::Sell}) {
      const auto& want = q.side(s);
      const bool has = !want.empty() && want[0].qty.is_positive();
      reconcile_slot(l, i, s, has ? want[0].price : Price{}, has ? want[0].qty : Qty{}, now);
    }
    lm_[i * lanes_.size() + static_cast<std::size_t>(&l - lanes_.data())] = mid.raw;
  }

  // ctx.pull_quotes -> QuoteManager::pull_quotes.
  void pull(Lane& l, std::uint32_t i, Timestamp now) {
    LaneInst& li = l.inst[i];
    li.pulled = true;
    for (const Side s : {Side::Buy, Side::Sell}) {
      li.slot[static_cast<std::size_t>(s)].apply_want = false;
      const EngOrder& o = li.eng[static_cast<std::size_t>(s)];
      if (o.st == St::None || !working(o.st)) continue;
      cancel(l, i, s, now);
    }
  }

  // QuoteManager::reconcile_slot; qty zero: no quote on this side.
  void reconcile_slot(Lane& l, std::uint32_t i, Side s, Price px, Qty qty, Timestamp now) {
    LaneInst& li = l.inst[i];
    const auto k = static_cast<std::size_t>(s);
    SlotQ& slot = li.slot[k];
    EngOrder& o = li.eng[k];
    const bool has_want = qty.is_positive();
    if (o.st != St::None) {
      if (pending(o.st)) {
        slot.want_px = px;
        slot.want_qty = qty;
        slot.apply_want = true;
        return;
      }
      slot.apply_want = false;
      if (!has_want) {
        cancel(l, i, s, now);
        return;
      }
      const Instrument& inst = instrument(i);
      const std::int64_t dpx = (px - o.px).abs().raw / inst.tick.raw;
      const bool qty_ok = static_cast<Int128>(o.leaves().raw) * 10'000 >=
                          static_cast<Int128>(qty.raw) * qp_.min_qty_bps;
      if (dpx < qp_.min_requote_ticks && qty_ok) return;
      if (now - slot.last_requote < qp_.min_requote_interval) return;
      // Replace (supports_replace): the risk check, then OMS request_replace and send.
      if (stp_refuses(li, s, px)) {
        ++l.res.risk_rejects;
        return;
      }
      o.pend_id = l.next_id++;
      o.pend_px = px;
      o.pend_qty = qty;
      o.st = St::PendingReplace;
      Msg m;
      m.kind = K::Replace;
      m.id = o.pend_id;
      m.id2 = o.id;
      m.inst = i;
      m.side = s;
      m.px = px;
      m.qty = qty;
      to_venue(l, m, now);
      ++l.res.replaces;
      slot.last_requote = now;
      return;
    }
    slot.apply_want = false;
    if (!has_want) return;
    submit_new(l, i, s, px, qty, now);
  }

  bool stp_refuses(const LaneInst& li, Side s, Price px) const noexcept {
    if (!stp_) return false;
    const EngOrder& opp = li.eng[static_cast<std::size_t>(opposite(s))];
    if (opp.st == St::None) return false;
    const Price own = opp.px;
    return own.is_positive() && (s == Side::Buy ? px >= own : px <= own);
  }

  void submit_new(Lane& l, std::uint32_t i, Side s, Price px, Qty qty, Timestamp now) {
    LaneInst& li = l.inst[i];
    if (stp_refuses(li, s, px)) {
      ++l.res.risk_rejects;
      return;
    }
    EngOrder& o = li.eng[static_cast<std::size_t>(s)];
    o = EngOrder{};
    o.st = St::PendingNew;
    o.id = l.next_id++;
    o.px = px;
    o.qty = qty;
    Msg m;
    m.kind = K::New;
    m.id = o.id;
    m.inst = i;
    m.side = s;
    m.px = px;
    m.qty = qty;
    to_venue(l, m, now);
    ++l.res.orders;
    li.slot[static_cast<std::size_t>(s)].last_requote = now;
  }

  void cancel(Lane& l, std::uint32_t i, Side s, Timestamp now) {
    EngOrder& o = l.inst[i].eng[static_cast<std::size_t>(s)];
    if (!working(o.st)) return;
    o.st = St::PendingCancel;
    Msg m;
    m.kind = K::Cancel;
    m.id = o.id;
    m.inst = i;
    m.side = s;
    to_venue(l, m, now);
    ++l.res.cancels;
  }

  // What the OMS reported for a message (OmsUpdate, the fields the quote manager reads).
  struct Update {
    bool known = false;     // the slot's live order
    bool terminal = false;  // it ended
    bool rejected = false;  // terminal as Rejected
    St prev = St::None;
  };

  // QuoteManager::on_order_update.
  void on_order_update(Lane& l, std::uint32_t i, Side s, const Update& u, Timestamp now) {
    LaneInst& li = l.inst[i];
    const auto k = static_cast<std::size_t>(s);
    SlotQ& slot = li.slot[k];
    const EngOrder& o = li.eng[k];
    if (!u.terminal) {
      if (!u.known || !working(o.st)) return;
      if (li.pulled) {
        cancel(l, i, s, now);
        return;
      }
      if (slot.apply_want && (u.prev == St::PendingNew || u.prev == St::PendingReplace)) {
        slot.apply_want = false;
        reconcile_slot(l, i, s, slot.want_px, slot.want_qty, now);
      }
      return;
    }
    if (o.st != St::None) return;  // the slot holds a newer order
    if (li.pulled || !slot.apply_want) return;
    slot.apply_want = false;
    if (!slot.want_qty.is_positive()) return;
    submit_new(l, i, s, slot.want_px, slot.want_qty, now);
  }

  void terminate(EngOrder& o, Update& u) noexcept {
    u.terminal = true;
    o = EngOrder{};
  }

  void engine_msg(Lane& l, const Msg& m) {
    const Timestamp now = clock_;
    const std::uint32_t i = m.inst;
    LaneInst& li = l.inst[i];
    const auto k = static_cast<std::size_t>(m.side);
    EngOrder& o = li.eng[k];
    const bool match =
        o.st != St::None && (m.id == o.id || (o.st == St::PendingReplace && m.id == o.pend_id));
    Update u;
    u.prev = o.st;
    u.known = match;
    switch (m.kind) {
      case K::Ack: {
        if (!match) return;
        if (o.st == St::PendingNew) {
          o.st = St::Live;
        } else if (o.st == St::PendingReplace && m.id == o.pend_id) {
          // apply_replace: a new order on the venue (no in-place flag), so its cum starts at 0
          o.id = o.pend_id;
          o.cum = Qty{};
          o.px = o.pend_px;
          o.qty = o.pend_qty;
          o.pend_id = 0;
          o.pend_px = Price{};
          o.pend_qty = Qty{};
          o.old_canceled = false;
          o.st = St::Live;
        }
        on_order_update(l, i, m.side, u, now);
        return;
      }
      case K::Reject: {
        if (!match) return;
        ++l.res.venue_rejects;
        if (o.st == St::PendingNew) {
          u.rejected = true;
          terminate(o, u);
        } else if (o.st == St::PendingReplace && m.id == o.pend_id) {
          if (o.old_canceled) {
            terminate(o, u);
          } else {
            o.pend_id = 0;
            o.pend_px = Price{};
            o.pend_qty = Qty{};
            o.st = o.cum.is_zero() ? St::Live : St::PartFilled;
          }
        }
        if (u.terminal) u.known = false;
        on_order_update(l, i, m.side, u, now);
        return;
      }
      case K::CancelAck: {
        if (!match) return;
        if (m.cum > o.cum) ++l.res.venue_rejects;  // missed fill: never expected here
        if (o.st == St::PendingReplace && m.id == o.id) {
          o.old_canceled = true;
        } else {
          terminate(o, u);
          u.known = false;
        }
        on_order_update(l, i, m.side, u, now);
        return;
      }
      case K::CancelReject: {
        if (!match) return;
        if (o.st != St::PendingCancel) {
          on_order_update(l, i, m.side, u, now);
          return;
        }
        terminate(o, u);  // VenueUnknownOrder: it is gone
        u.known = false;
        on_order_update(l, i, m.side, u, now);
        return;
      }
      case K::Fill: {
        // Booked whatever the order's state (a late fill still moves the position).
        const Qty signed_qty = m.side == Side::Buy ? m.qty : -m.qty;
        li.pos += signed_qty;
        const Notional n = cfg_.instruments.get(InstrumentId{i}).notional(m.px, m.qty);
        l.cash = m.side == Side::Buy ? l.cash - n : l.cash + n;
        l.fees += m.fee;
        ++l.res.fills;
        if (m.liq == Liquidity::Taker) {
          ++l.res.taker_fills;
        } else {
          ++l.res.maker_fills;
        }
        l.res.volume += m.qty;
        l.res.fill_hash =
            mix(mix(mix(mix(mix(l.res.fill_hash, static_cast<std::uint64_t>(now.ns)), i),
                        static_cast<std::uint64_t>(m.side)),
                    static_cast<std::uint64_t>(m.px.raw)),
                static_cast<std::uint64_t>(m.qty.raw));
        if (record != nullptr) {
          (*record)[static_cast<std::size_t>(&l - lanes_.data())].push_back(
              LaneFill{m.vts, i, m.side, m.px, m.qty, m.liq});
        }
        if (match) {
          Qty cum = m.cum.is_positive() ? m.cum : o.cum + m.qty;
          if (cum < o.cum) cum = o.cum;
          if (cum > o.qty) cum = o.qty;
          o.cum = cum;
          if (o.cum >= o.qty) {
            terminate(o, u);
            u.known = false;
          } else if (o.st == St::Live || o.st == St::PendingNew) {
            o.st = St::PartFilled;
          }
        }
        // BasicMM::on_fill, then the quote manager.
        if (books_[i].is_valid()) requote(l, i, now);
        if (match) on_order_update(l, i, m.side, u, now);
        return;
      }
      default:
        return;
    }
  }

  const BacktestConfig& cfg_;
  std::size_t n_;
  sim::SimWorld world_;
  std::unique_ptr<L2Book<256>[]> books_;  // the engine's books
  std::vector<std::unordered_map<std::uint64_t, Rec>> recs_;
  std::vector<std::vector<std::uint32_t>> resting_;  // lanes with a venue order, per instrument
  std::vector<Price> last_mid_;                      // the engine's last valid mid
  // Per instrument and side, a price no live venue order of any lane is better than (exact after
  // each scan, possibly too good in between), and per instrument the live venue orders whose
  // `crossed` is positive: an execution or a book update that reaches neither skips the lanes.
  std::vector<std::array<Price, 2>> bound_;
  std::vector<std::uint32_t> crossed_n_;
  // Per instrument, the engine book's validity and mid at its last update: a lane acts on an
  // update only when the mid moved (or validity changed).
  std::vector<std::uint8_t> md_valid_;
  std::vector<Price> md_mid_;
  std::vector<std::int64_t> lm_;   // [instrument][lane]: the lane's last quoted mid, raw (0: none)
  std::vector<std::int64_t> thr_;  // [instrument][lane]: its requote threshold, raw price
  bool thr_zero_ = false;          // a lane requotes on every update
  std::vector<Lane> lanes_;
  QuoteParams qp_;
  bool stp_ = true;
  std::uint64_t l3_seq_ = 0;
  std::uint64_t next_forward_ = 1;
  std::map<std::uint64_t, Held> held_;
  Timestamp last_md_arrival_{};
  std::deque<TapeEntry> mdq_;
  std::vector<std::vector<std::uint64_t>> free_bytes_;
  struct WakeQ {
    std::vector<std::pair<std::int64_t, std::uint32_t>> v;
    std::size_t head = 0;
  };
  std::vector<WakeQ> wq_;
  std::vector<Duration> wq_lat_;
  std::vector<std::pair<std::int64_t, std::uint32_t>> heap_;
  std::uint64_t requotes_ = 0;
  Timestamp clock_{};
  Tape* tape_ = nullptr;
  std::vector<TapeLast> tape_last_;
  std::vector<lk::Levels> tape_levels_;
  std::vector<std::int64_t> tape_lv_idx_;
};

}  // namespace

std::string check_population(const BacktestConfig& cfg) {
  if (cfg.transport.fill_model != sim::FillModel::L3Queue) return "fill_model must be l3_queue";
  if (cfg.transport.own_orders_in_feed) return "own_orders_in_feed must be false";
  if (!cfg.transport.accounts.empty()) return "[backtest.balances] is not supported";
  if (!cfg.transport.venues.empty()) return "[backtest.venues.*] is not supported";
  if (cfg.transport.order_out.jitter.ns != 0 || cfg.transport.ack_in.jitter.ns != 0 ||
      cfg.transport.md_in.jitter.ns != 0)
    return "latency jitter must be 0";
  if (cfg.transport.order_out.p_drop != 0.0) return "p_drop must be 0";
  if (cfg.transport.cancel_latency || cfg.transport.fill_latency)
    return "cancel and fill latencies of their own are not supported";
  if (cfg.transport.order_service.ns != 0) return "order_service_us must be 0";
  if (cfg.transport.md_recorded_arrival) return "md_arrival must be venue";
  if (!cfg.engine.quotes.supports_replace || !cfg.transport.supports_replace)
    return "supports_replace must be true";
  if (!cfg.engine.quotes.post_only) return "post_only must be true";
  if (cfg.strategy != "basic_mm" && !cfg.strategy.empty()) return "the strategy must be basic_mm";
  BasicMMParams p;
  if (const auto err = p.apply(cfg.params)) return *err;
  if (p.levels != 1) return "levels must be 1";
  if (p.pull_on_stale_ms.ns != 0) return "pull_on_stale_ms must be 0";
  const RiskLimits& r = cfg.engine.risk;
  if (r.max_order_qty.is_positive() || r.max_order_notional.is_positive() ||
      r.max_position.is_positive() || r.max_open_orders > 0 || r.price_collar_bps > 0 ||
      r.fat_finger_bps > 0 || r.stale_md.ns > 0 || r.max_loss.is_positive() || r.orders_per_sec > 0)
    return "[risk] limits other than stp are not supported";
  return {};
}

Tape compile_tape(const BacktestConfig& cfg, sim::MdSource& source) {
  if (const std::string why = check_population(cfg); !why.empty())
    throw std::invalid_argument("population: " + why);
  Tape t;
  Pass pass(cfg, {});
  pass.set_tape(&t);
  static_cast<void>(pass.run(source));
  t.insts.resize(cfg.instruments.size());
  for (std::size_t i = 0; i < cfg.instruments.size(); ++i) {
    const Instrument& inst = cfg.instruments.get(InstrumentId{static_cast<std::uint32_t>(i)});
    const FeeRates& f = cfg.transport.fees.schedule(inst.id);
    t.insts[i] = lk::Inst{inst.tick.raw, inst.lot.raw, f.maker_cbps, f.taker_cbps};
  }
  t.shared.min_requote_ticks = cfg.engine.quotes.min_requote_ticks;
  t.shared.min_requote_interval = cfg.engine.quotes.min_requote_interval.ns;
  t.shared.min_qty_bps = cfg.engine.quotes.min_qty_bps;
  t.shared.start = source.start_ts().valid() ? source.start_ts().ns : 0;
  t.shared.stp = cfg.engine.risk.stp ? 1 : 0;
  return t;
}

namespace {
template <class T>
void put_vec(std::FILE* f, const std::vector<T>& v) {
  const std::uint64_t n = v.size();
  std::fwrite(&n, sizeof n, 1, f);
  if (n != 0) std::fwrite(v.data(), sizeof(T), n, f);
}
template <class T>
std::vector<T> get_vec(std::FILE* f) {
  std::uint64_t n = 0;
  if (std::fread(&n, sizeof n, 1, f) != 1) throw std::runtime_error("tape: truncated");
  std::vector<T> v(n);
  if (n != 0 && std::fread(v.data(), sizeof(T), n, f) != n)
    throw std::runtime_error("tape: truncated");
  return v;
}
constexpr std::uint64_t kTapeMagic = 0x31455041544d46ULL;  // "FMTAPE1"
}  // namespace

void Tape::save(const std::string& path) const {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) throw std::runtime_error("tape: cannot write " + path);
  std::fwrite(&kTapeMagic, sizeof kTapeMagic, 1, f);
  std::fwrite(&shared, sizeof shared, 1, f);
  put_vec(f, insts);
  put_vec(f, levels);
  put_vec(f, adds);
  for (const auto& r : recs) put_vec(f, r);
  std::fclose(f);
}

Tape Tape::load(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) throw std::runtime_error("tape: cannot read " + path);
  Tape t;
  std::uint64_t magic = 0;
  if (std::fread(&magic, sizeof magic, 1, f) != 1 || magic != kTapeMagic ||
      std::fread(&t.shared, sizeof t.shared, 1, f) != 1) {
    std::fclose(f);
    throw std::runtime_error("tape: not a tape: " + path);
  }
  t.insts = get_vec<lk::Inst>(f);
  t.levels = get_vec<lk::Levels>(f);
  t.adds = get_vec<std::int64_t>(f);
  t.recs.resize(t.insts.size());
  for (auto& r : t.recs) r = get_vec<lk::Rec>(f);
  std::fclose(f);
  return t;
}

lk::Params kernel_params(const LaneSpec& s) {
  if (s.requote_threshold_ticks < 1)
    throw std::invalid_argument("tape lanes: requote_threshold_ticks must be at least 1");
  return lk::Params{s.half_spread.raw,
                    s.skew.raw,
                    s.quote_qty.raw,
                    s.max_inventory.raw,
                    s.requote_threshold_ticks,
                    s.order_latency.ns,
                    s.ack_latency.ns};
}

std::vector<lk::Out> run_tape(const Tape& tape, std::span<const LaneSpec> lanes, int threads) {
  const std::size_t n_inst = tape.insts.size();
  std::vector<lk::Params> params;
  params.reserve(lanes.size());
  for (const LaneSpec& s : lanes) params.push_back(kernel_params(s));
  std::vector<lk::Out> out(lanes.size() * n_inst);
  const std::size_t units = out.size();
  unsigned w = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
  if (w == 0) w = 1;
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (;;) {
      const std::size_t u = next.fetch_add(1, std::memory_order_relaxed);
      if (u >= units) return;
      const std::size_t lane = u / n_inst;
      const std::size_t i = u % n_inst;
      auto k = std::make_unique<lk::Lane>(params[lane],
                                          tape.insts[i],
                                          tape.shared,
                                          tape.levels.data(),
                                          tape.adds.data(),
                                          tape.adds.size(),
                                          &out[u]);
      k->run(tape.recs[i].data(), tape.recs[i].size());
    }
  };
  if (w == 1) {
    work();
  } else {
    std::vector<std::jthread> pool;
    for (unsigned k = 0; k < w; ++k) pool.emplace_back(work);
  }
  return out;
}

LaneSpec lane_from_config(const BacktestConfig& cfg) {
  BasicMMParams p;
  if (const auto err = p.apply(cfg.params)) throw std::invalid_argument(*err);
  LaneSpec s;
  s.half_spread = p.half_spread_bps;
  s.skew = p.skew_bps_per_unit;
  s.quote_qty = p.quote_qty;
  s.max_inventory = p.max_inventory;
  s.requote_threshold_ticks = p.requote_threshold_ticks;
  s.order_latency = cfg.transport.order_out.fixed;
  s.ack_latency = cfg.transport.ack_in.fixed;
  return s;
}

std::vector<LaneResult> run_population(
    const BacktestConfig& cfg,
    std::span<const LaneSpec> lanes,
    const std::function<std::unique_ptr<sim::MdSource>()>& source_factory,
    int threads,
    PopulationStats* stats,
    std::vector<std::vector<LaneFill>>* fills) {
  if (const std::string why = check_population(cfg); !why.empty())
    throw std::invalid_argument("population: " + why);
  for (const LaneSpec& s : lanes) {
    if (s.order_latency.ns <= 0 || s.ack_latency.ns <= 0)
      throw std::invalid_argument("population: lane latencies must be positive");
  }
  std::vector<LaneResult> out(lanes.size());
  if (lanes.empty()) return out;
  if (fills != nullptr) fills->assign(lanes.size(), {});
  unsigned w = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
  if (w == 0) w = 1;
  if (w > lanes.size()) w = static_cast<unsigned>(lanes.size());
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<PopulationStats> per(w);
  std::exception_ptr error;
  std::atomic<bool> failed{false};
  const auto work = [&](unsigned k) {
    try {
      std::vector<std::size_t> idx;
      std::vector<LaneSpec> mine;
      for (std::size_t i = k; i < lanes.size(); i += w) {
        idx.push_back(i);
        mine.push_back(lanes[i]);
      }
      std::unique_ptr<sim::MdSource> src = source_factory();
      if (src == nullptr) throw std::invalid_argument("population: needs recorded data");
      Pass pass(cfg, mine);
      std::vector<std::vector<LaneFill>> rec;
      if (fills != nullptr) {
        rec.resize(mine.size());
        pass.record = &rec;
      }
      per[k] = pass.run(*src);
      pass.results(out, idx);
      if (fills != nullptr) {
        for (std::size_t j = 0; j < idx.size(); ++j) (*fills)[idx[j]] = std::move(rec[j]);
      }
    } catch (...) {
      if (!failed.exchange(true)) error = std::current_exception();
    }
  };
  if (w == 1) {
    work(0);
  } else {
    std::vector<std::jthread> pool;
    pool.reserve(w);
    for (unsigned k = 0; k < w; ++k) pool.emplace_back(work, k);
  }
  if (error) std::rethrow_exception(error);
  if (stats != nullptr) {
    *stats = PopulationStats{};
    for (const PopulationStats& p : per) {
      stats->source_events = std::max(stats->source_events, p.source_events);
      stats->md_events = std::max(stats->md_events, p.md_events);
      stats->lane_wakes += p.lane_wakes;
      stats->lane_requotes += p.lane_requotes;
    }
    stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  return out;
}

}  // namespace fastmm::bt
