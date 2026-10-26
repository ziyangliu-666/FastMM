#pragma once
// Lane kernel: one population lane on one instrument, as a pure function of a compiled tape.
//
// A population (population.hpp) shares one market among its lanes. Everything a lane reads of that
// market can be written down once, ahead of the lanes: the venue's book after each recorded update
// (its best levels), the executions of recorded orders, the engine's view after each market-data
// message (validity, mid, touch, and the engine clock it is taken at) and the arrival place of the
// next recorded order at any time. With those on a tape, a lane on one instrument depends on
// nothing else: its own messages, its own orders, its own position. Instruments of one lane do not
// interact under the population's scope (fixed latencies keep each path in order, so a lane's
// wire never holds one instrument's message behind another's), so (lane, instrument) is the unit
// of work, and every unit runs the same code over the same tape.
//
// This header is that code, written for both the CPU and a GPU: plain integers, fixed-size state,
// no allocation, no library calls, 128-bit products only where the engine's fixed point uses them.
// It reproduces population.cpp's Pass (and so the engine) for basic_mm with one level per side:
// fills, cash, fees and positions are identical (tests/backtest/population_test.cpp).
#include <cstdint>

#if defined(__CUDACC__)
#define FASTMM_LK __host__ __device__ inline
#else
#define FASTMM_LK inline
#endif

namespace fastmm::lk {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;
__extension__ using i128 = __int128;

inline constexpr i64 kScale = 100'000'000;  // fixed point 1e-8, Ratio 1.0
inline constexpr int kLevels = 8;           // book levels per side on the tape
inline constexpr i64 kMaxTime = INT64_MAX;

FASTMM_LK i64 muldiv(i64 a, i64 b, i64 d) {
  return static_cast<i64>(static_cast<i128>(a) * b / d);
}
FASTMM_LK i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
FASTMM_LK i64 iabs(i64 v) {
  return v < 0 ? -v : v;
}
FASTMM_LK i64 imin(i64 a, i64 b) {
  return a < b ? a : b;
}

// ---- the tape ----------------------------------------------------------------------------------

enum RecKind : u8 { Delta = 0, Exec = 1, Md = 2 };

// One shared event of one instrument, in the order the population takes them.
struct Rec {
  i64 t;       // venue time (delta, exec) or market-data arrival (md)
  i64 a;       // exec: recorded order's price; md: the engine clock it is taken at; delta: best bid
  i64 b;       // exec: recorded order's place (2 * arrival count); md: mid (0: book invalid)
  i64 c;       // exec: execution price; md: best bid; delta: best ask (0: none on that side)
  i64 d;       // exec: quantity; md: best ask
  u32 levels;  // delta: index into the level table
  u8 kind;
  u8 side;  // exec: the recorded order's side (0 buy, 1 sell)
  u8 pad[2];
};
static_assert(sizeof(Rec) == 48);

// The venue's best levels of both sides after a book update, side 0 bids and 1 asks: the best
// price, then each level as ticks away from it and its quantity in lots. `more` bit s: side s goes
// deeper than kLevels; `exact` 0: a level did not fit the encoding (the kernel flags its use).
struct Levels {
  i64 best[2];
  std::int32_t qty[2][kLevels];
  std::uint16_t off[2][kLevels];
  u8 n[2];
  u8 more;
  u8 exact;
  u32 pad;
};
static_assert(sizeof(Levels) == 120);

struct Inst {
  i64 tick;
  i64 lot;
  i64 maker_cbps;
  i64 taker_cbps;
};

// Shared settings (QuoteParams, risk stp) and the engine clock's start.
struct Shared {
  i64 min_requote_ticks;
  i64 min_requote_interval;
  i64 min_qty_bps;
  i64 start;
  u8 stp;
};

struct Params {
  i64 half_spread;  // Ratio raw
  i64 skew;         // Ratio raw
  i64 quote_qty;    // raw
  i64 max_inventory;
  i64 threshold_ticks;
  i64 order_latency;  // ns
  i64 ack_latency;    // ns
};

struct Out {
  u64 fills;
  u64 maker_fills;
  u64 taker_fills;
  u64 orders;
  u64 replaces;
  u64 cancels;
  u64 venue_rejects;
  u64 risk_rejects;
  i64 volume;
  i64 cash;
  i64 fees;
  i64 position;
  u64 fill_hash;  // this instrument's fills in order
  u32 error;      // nonzero: the tape could not answer (book deeper than kLevels, queue full)
  u32 pad;
};

// ---- one lane on one instrument ----------------------------------------------------------------

enum St : u8 { None, PendingNew, Live, PartFilled, PendingReplace, PendingCancel };
FASTMM_LK bool working(u8 s) {
  return s == Live || s == PartFilled;
}
FASTMM_LK bool pending(u8 s) {
  return s == PendingNew || s == PendingReplace || s == PendingCancel;
}

enum K : u8 { New, Cancel, Replace, Ack, Reject, CancelAck, CancelReject, Fill };
enum Liq : u8 { Maker = 1, Taker = 2 };

struct Msg {
  i64 ts;
  i64 vts;
  u32 id;
  u32 id2;
  i64 px;
  i64 qty;
  i64 cum;
  i64 fee;
  u8 kind;
  u8 side;
  u8 liq;
  u8 reason;  // 1: post-only cross
  u8 pad[4];
};
static_assert(sizeof(Msg) == 64);

template <unsigned N>
struct Ring {
  Msg m[N];
  u32 head = 0;
  u32 n = 0;
  FASTMM_LK bool empty() const { return n == 0; }
  FASTMM_LK const Msg& front() const { return m[head]; }
  FASTMM_LK void pop() {
    head = (head + 1) % N;
    --n;
  }
  FASTMM_LK bool push(const Msg& x) {
    if (n == N) return false;
    m[(head + n) % N] = x;
    ++n;
    return true;
  }
};

struct EngOrder {
  u8 st = None;
  u8 old_canceled = 0;
  u32 id = 0;
  u32 pend_id = 0;
  i64 px = 0;
  i64 pend_px = 0;
  i64 qty = 0;
  i64 cum = 0;
  i64 pend_qty = 0;
};
struct Slot {
  i64 want_px = 0;
  i64 want_qty = 0;
  u8 apply_want = 0;
  i64 last_requote = INT64_MIN / 2;
};
struct VenOrder {
  u8 live = 0;
  u32 id = 0;
  i64 px = 0;
  i64 qty = 0;
  i64 cum = 0;
  i64 prio = 0;
  i64 placed = 0;
  i64 crossed = 0;
};

FASTMM_LK u64 mix(u64 h, u64 v) {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
  return h;
}

// Runs one lane on one instrument over the instrument's records. `adds`/`nadds`: the venue times
// of every recorded arrival (all instruments, in order), for the place of an order of ours.
class Lane {
 public:
  FASTMM_LK Lane(const Params& p,
                 const Inst& inst,
                 const Shared& sh,
                 const Levels* levels,
                 const i64* adds,
                 u64 nadds,
                 Out* out)
      : p_(p),
        in_(inst),
        sh_(sh),
        lv_(levels),
        adds_(adds),
        nadds_(nadds),
        o_(out),
        thr_(inst.tick * p.threshold_ticks) {
    *o_ = Out{};
  }

  FASTMM_LK void run(const Rec* recs, u64 n) {
    for (u64 k = 0; k < n; ++k) step(recs[k]);
    finish();
  }

  // One record, then finish() after the last: what run() does, for a caller that streams them.
  FASTMM_LK void step(const Rec& r) {
    if (r.kind == Md) {
      // A message held back for the recorded order reaches the engine after the venue event that
      // released it (its clock, r.a): own messages before that event come first.
      if (r.a > r.t) {
        drain(r.a, false);
      } else {
        drain(r.t, true);
      }
      md(r);
    } else {
      drain(r.t, false);
      if (r.kind == Delta) {
        book_ = &lv_[r.levels];
        best_[0] = r.a;
        best_[1] = r.c;
        cross(r.t);
      } else {
        exec(r);
      }
    }
  }
  FASTMM_LK void finish() {
    drain(kMaxTime, true);
    o_->position = pos_;
  }

 private:
  // Own messages due before a shared event at `t`: before a venue event strictly earlier, before a
  // market-data message at the same time too.
  FASTMM_LK void drain(i64 t, bool inclusive) {
    if (due_ == kMaxTime || (inclusive ? due_ > t : due_ >= t)) return;  // the common case
    for (;;) {
      const i64 ta = out_.empty() ? kMaxTime : out_.front().ts;
      const i64 ti = in_q_.empty() ? kMaxTime : in_q_.front().ts;
      const i64 nx = ta <= ti ? ta : ti;
      due_ = nx;
      if (nx == kMaxTime || (inclusive ? nx > t : nx >= t)) return;
      if (ta <= ti) {
        const Msg m = out_.front();
        out_.pop();
        arrival(m);
      } else {
        const Msg m = in_q_.front();
        in_q_.pop();
        clock_ = m.ts > sh_.start ? m.ts : sh_.start;
        engine(m);
      }
    }
  }

  // ---- venue -----------------------------------------------------------------------------------

  // The venue's best price of `side`, 0: none. From the record, so that a lane whose orders the
  // book does not reach never reads the level table.
  FASTMM_LK i64 best(u8 side) const { return best_[side]; }
  FASTMM_LK i64 level_px(u8 side, u32 k) const {
    const i64 d = static_cast<i64>(book_->off[side][k]) * in_.tick;
    return side == 0 ? book_->best[0] - d : book_->best[1] + d;
  }
  FASTMM_LK i64 level_qty(u8 side, u32 k) const {
    return static_cast<i64>(book_->qty[side][k]) * in_.lot;
  }
  // The levels of `side` run out at k == n: past them the book may hold more.
  FASTMM_LK void check_depth(u8 side) {
    if ((book_->more & (1U << side)) != 0) o_->error |= 1U;
  }
  FASTMM_LK static bool at_or_better(u8 side, i64 p, i64 q) { return side == 0 ? p >= q : p <= q; }
  FASTMM_LK static bool better(u8 side, i64 p, i64 q) { return side == 0 ? p > q : p < q; }
  // Opposite quantity at or through `px` for an order of `side`.
  FASTMM_LK i64 crossing(u8 side, i64 px) {
    const u8 opp = side ^ 1U;
    const i64 b = best(opp);
    if (b <= 0 || !at_or_better(opp, b, px)) return 0;
    if (book_->exact == 0) o_->error |= 4U;
    const u32 n = book_->n[opp];
    i64 q = 0;
    for (u32 k = 0; k < n; ++k) {
      if (!at_or_better(opp, level_px(opp, k), px)) return q;
      q += level_qty(opp, k);
    }
    check_depth(opp);
    return q;
  }

  FASTMM_LK void cross(i64 now) {
    for (u8 s = 0; s < 2; ++s) {
      VenOrder& o = ven_[s];
      if (o.live == 0) continue;
      const u8 opp = s ^ 1U;
      const i64 b = best(opp);
      if (o.crossed == 0 && (b <= 0 || !at_or_better(opp, b, o.px))) continue;
      const i64 c = crossing(s, o.px);
      if (now < o.placed) {
        if (c > o.crossed) o.crossed = c;
      } else if (c > o.crossed) {
        const i64 fresh = c - o.crossed;
        o.crossed = c;
        const i64 fill = imin(fresh, o.qty - o.cum);
        if (fill > 0) {
          o.cum += fill;
          venue_fill(s, o, fill, Maker, o.px, now);
        }
      } else {
        o.crossed = c;
      }
    }
  }

  FASTMM_LK void exec(const Rec& r) {
    VenOrder& o = ven_[r.side];
    if (o.live == 0 || r.t < o.placed) return;
    // l3_execution_reaches == Fills
    const bool reached = at_or_better(r.side, o.px, r.c);
    if (!(reached && (better(r.side, o.px, r.a) || (o.px == r.a && o.prio < r.b)))) return;
    const i64 fill = imin(r.d, o.qty - o.cum);
    if (fill <= 0) return;
    o.cum += fill;
    venue_fill(r.side, o, fill, Maker, o.px, r.t);
  }

  FASTMM_LK void venue_fill(u8 side, VenOrder& o, i64 fill, u8 liq, i64 px, i64 now) {
    Msg m{};
    m.kind = Fill;
    m.id = o.id;
    m.side = side;
    m.px = px;
    m.qty = fill;
    m.cum = o.cum;
    m.liq = liq;
    const i64 notional = muldiv(px, fill, kScale);
    m.fee = muldiv(notional, liq == Taker ? in_.taker_cbps : in_.maker_cbps, 1'000'000);
    if (o.qty - o.cum == 0) o.live = 0;
    to_engine(m, now);
  }

  FASTMM_LK void to_engine(Msg m, i64 vts) {
    i64 arrival = vts + p_.ack_latency;
    if (arrival < last_in_) arrival = last_in_;
    last_in_ = arrival;
    m.ts = arrival;
    m.vts = vts;
    if (!in_q_.push(m)) o_->error |= 2U;
    if (m.ts < due_) due_ = m.ts;
  }
  FASTMM_LK void emit(u8 kind, u8 side, u32 id, i64 now, i64 cum = 0, u8 reason = 0) {
    Msg m{};
    m.kind = kind;
    m.id = id;
    m.side = side;
    m.cum = cum;
    m.reason = reason;
    to_engine(m, now);
  }

  FASTMM_LK void arrival(const Msg& m) {
    const i64 now = m.ts;
    VenOrder& o = ven_[m.side];
    switch (m.kind) {
      case New:
        venue_new(m.side, m.id, m.px, m.qty, true, now);
        return;
      case Cancel:
        if (o.live == 0 || o.id != m.id) {
          emit(CancelReject, m.side, m.id, now);
          return;
        }
        o.live = 0;
        emit(CancelAck, m.side, o.id, now, o.cum);
        return;
      case Replace: {
        if (o.live == 0 || o.id != m.id2) {
          emit(CancelReject, m.side, m.id2, now);
          emit(Reject, m.side, m.id, now, 0, 2);
          return;
        }
        if (m.px == o.px && m.qty <= o.qty - o.cum) {
          const u32 old_id = o.id;
          const i64 old_cum = o.cum;
          o.id = m.id;
          o.qty = m.qty;
          o.cum = 0;
          if (now > o.placed) o.placed = now;
          emit(CancelAck, m.side, old_id, now, old_cum);
          emit(Ack, m.side, m.id, now);
          return;
        }
        o.live = 0;
        emit(CancelAck, m.side, o.id, now, o.cum);
        venue_new(m.side, m.id, m.px, m.qty, false, now);
        return;
      }
      default:
        return;
    }
  }

  // Recorded arrivals at or before `t`.
  FASTMM_LK i64 seq_at(i64 t) const {
    u64 lo = 0;
    u64 hi = nadds_;
    while (lo < hi) {
      const u64 mid = lo + (hi - lo) / 2;
      if (adds_[mid] <= t) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return static_cast<i64>(lo);
  }

  FASTMM_LK void venue_new(u8 side, u32 id, i64 px, i64 qty, bool post_only, i64 now) {
    if (qty <= 0 || px <= 0) {
      emit(Reject, side, id, now, 0, 3);
      return;
    }
    const u8 opp = side ^ 1U;
    const i64 b = best(opp);
    const bool crosses = b > 0 && (side == 0 ? px >= b : px <= b);
    if (post_only && crosses) {
      emit(Reject, side, id, now, 0, 1);
      return;
    }
    emit(Ack, side, id, now);
    VenOrder& o = ven_[side];
    o = VenOrder{};
    o.id = id;
    o.px = px;
    o.qty = qty;
    i64 leaves = qty;
    i64 cum = 0;
    if (crosses) {
      if (book_->exact == 0) o_->error |= 4U;
      const u32 n = book_->n[opp];
      u32 k = 0;
      for (; k < n && leaves > 0; ++k) {
        const i64 lp = level_px(opp, k);
        if (!at_or_better(opp, lp, px)) break;
        const i64 q = imin(leaves, level_qty(opp, k));
        cum += q;
        leaves -= q;
        o.cum = cum;
        venue_fill(side, o, q, Taker, lp, now);
      }
      if (k == n && leaves > 0) check_depth(opp);
    }
    if (leaves == 0) {
      o.live = 0;
      return;
    }
    o.live = 1;
    o.cum = cum;
    o.crossed = cum;
    o.placed = now;
    o.prio = 2 * seq_at(now) + 1;
  }

  // ---- engine ----------------------------------------------------------------------------------

  FASTMM_LK void md(const Rec& r) {
    clock_ = r.a;
    bb_ = r.c;
    ba_ = r.d;
    const bool was_valid = valid_;
    const i64 prev = mid_;
    valid_ = r.b > 0;
    mid_ = r.b;
    if (!valid_) {
      pull();
      last_mid_ = 0;
      return;
    }
    // The tape writes a record when the touch moves; a lane acts when the mid moves (or validity
    // came back), as BasicMM's threshold reads it.
    if (was_valid && prev == mid_ && thr_ > 0) return;
    if (last_mid_ > 0 && iabs(mid_ - last_mid_) < thr_) return;
    requote();
  }

  FASTMM_LK i64 round_px(i64 p, u8 side) const {
    const i64 fl = floor_div(p, in_.tick) * in_.tick;
    return side == 0 || fl == p ? fl : fl + in_.tick;
  }

  FASTMM_LK void requote() {
    i64 bid_px = 0;
    i64 bid_q = 0;
    i64 ask_px = 0;
    i64 ask_q = 0;
    const i64 mid = mid_;
    if (p_.quote_qty != 0) {
      const i64 units = pos_ / p_.quote_qty;
      const i64 half = muldiv(mid, p_.half_spread, kScale);
      const i64 skew = p_.skew * units;  // Ratio * int64
      const i64 centre = mid - muldiv(mid, skew, kScale);
      const i64 qty = floor_div(p_.quote_qty, in_.lot) * in_.lot;
      const bool can_buy = p_.max_inventory == 0 || pos_ + p_.quote_qty <= p_.max_inventory;
      const bool can_sell = p_.max_inventory == 0 || pos_ - p_.quote_qty >= -p_.max_inventory;
      if (can_buy) {
        const i64 px = round_px(centre - half, 0);
        if (px > 0 && qty > 0) {
          bid_px = px;
          bid_q = qty;
        }
      }
      if (can_sell) {
        const i64 px = round_px(centre + half, 1);
        if (px > 0 && qty > 0) {
          ask_px = px;
          ask_q = qty;
        }
      }
      if (bid_q > 0 && ask_q > 0 && bid_px >= ask_px) ask_px = bid_px + in_.tick;  // uncross
    }
    // keep_passive
    if (bid_q > 0 && ba_ > 0) {
      const i64 limit = ba_ - in_.tick;
      if (bid_px > limit) bid_px = limit;
      if (bid_px <= 0) bid_q = 0;
    }
    if (ask_q > 0 && bb_ > 0) {
      const i64 limit = bb_ + in_.tick;
      if (ask_px < limit) ask_px = limit;
    }
    pulled_ = false;
    slot_reconcile(0, bid_q > 0 ? bid_px : 0, bid_q);
    slot_reconcile(1, ask_q > 0 ? ask_px : 0, ask_q);
    last_mid_ = mid;
  }

  FASTMM_LK void pull() {
    pulled_ = true;
    for (u8 s = 0; s < 2; ++s) {
      slot_[s].apply_want = 0;
      if (working(eng_[s].st)) cancel(s);
    }
  }

  FASTMM_LK bool stp_refuses(u8 s, i64 px) const {
    if (sh_.stp == 0) return false;
    const EngOrder& opp = eng_[s ^ 1U];
    if (opp.st == None || opp.px <= 0) return false;
    return s == 0 ? px >= opp.px : px <= opp.px;
  }

  FASTMM_LK void slot_reconcile(u8 s, i64 px, i64 qty) {
    Slot& slot = slot_[s];
    EngOrder& o = eng_[s];
    const i64 now = clock_;
    if (o.st != None) {
      if (pending(o.st)) {
        slot.want_px = px;
        slot.want_qty = qty;
        slot.apply_want = 1;
        return;
      }
      slot.apply_want = 0;
      if (qty <= 0) {
        cancel(s);
        return;
      }
      const i64 dpx = iabs(px - o.px) / in_.tick;
      const bool qty_ok =
          static_cast<i128>(o.qty - o.cum) * 10'000 >= static_cast<i128>(qty) * sh_.min_qty_bps;
      if (dpx < sh_.min_requote_ticks && qty_ok) return;
      if (now - slot.last_requote < sh_.min_requote_interval) return;
      if (stp_refuses(s, px)) {
        ++o_->risk_rejects;
        return;
      }
      o.pend_id = next_id_++;
      o.pend_px = px;
      o.pend_qty = qty;
      o.st = PendingReplace;
      Msg m{};
      m.kind = Replace;
      m.id = o.pend_id;
      m.id2 = o.id;
      m.side = s;
      m.px = px;
      m.qty = qty;
      to_venue(m);
      ++o_->replaces;
      slot.last_requote = now;
      return;
    }
    slot.apply_want = 0;
    if (qty <= 0) return;
    submit_new(s, px, qty);
  }

  FASTMM_LK void submit_new(u8 s, i64 px, i64 qty) {
    if (stp_refuses(s, px)) {
      ++o_->risk_rejects;
      return;
    }
    EngOrder& o = eng_[s];
    o = EngOrder{};
    o.st = PendingNew;
    o.id = next_id_++;
    o.px = px;
    o.qty = qty;
    Msg m{};
    m.kind = New;
    m.id = o.id;
    m.side = s;
    m.px = px;
    m.qty = qty;
    to_venue(m);
    ++o_->orders;
    slot_[s].last_requote = clock_;
  }

  FASTMM_LK void cancel(u8 s) {
    EngOrder& o = eng_[s];
    if (!working(o.st)) return;
    o.st = PendingCancel;
    Msg m{};
    m.kind = Cancel;
    m.id = o.id;
    m.side = s;
    to_venue(m);
    ++o_->cancels;
  }

  FASTMM_LK void to_venue(Msg m) {
    m.ts = clock_ + p_.order_latency;
    if (!out_.push(m)) o_->error |= 2U;
    if (m.ts < due_) due_ = m.ts;
  }

  FASTMM_LK void order_update(u8 s, bool known, bool terminal, u8 prev) {
    Slot& slot = slot_[s];
    const EngOrder& o = eng_[s];
    if (!terminal) {
      if (!known || !working(o.st)) return;
      if (pulled_) {
        cancel(s);
        return;
      }
      if (slot.apply_want != 0 && (prev == PendingNew || prev == PendingReplace)) {
        slot.apply_want = 0;
        slot_reconcile(s, slot.want_px, slot.want_qty);
      }
      return;
    }
    if (o.st != None) return;
    if (pulled_ || slot.apply_want == 0) return;
    slot.apply_want = 0;
    if (slot.want_qty <= 0) return;
    submit_new(s, slot.want_px, slot.want_qty);
  }

  FASTMM_LK void engine(const Msg& m) {
    EngOrder& o = eng_[m.side];
    const bool match =
        o.st != None && (m.id == o.id || (o.st == PendingReplace && m.id == o.pend_id));
    const u8 prev = o.st;
    bool terminal = false;
    bool known = match;
    switch (m.kind) {
      case Ack:
        if (!match) return;
        if (o.st == PendingNew) {
          o.st = Live;
        } else if (o.st == PendingReplace && m.id == o.pend_id) {
          o.id = o.pend_id;
          o.cum = 0;
          o.px = o.pend_px;
          o.qty = o.pend_qty;
          o.pend_id = 0;
          o.pend_px = 0;
          o.pend_qty = 0;
          o.old_canceled = 0;
          o.st = Live;
        }
        order_update(m.side, known, false, prev);
        return;
      case Reject:
        if (!match) return;
        ++o_->venue_rejects;
        if (o.st == PendingNew) {
          o = EngOrder{};
          terminal = true;
        } else if (o.st == PendingReplace && m.id == o.pend_id) {
          if (o.old_canceled) {
            o = EngOrder{};
            terminal = true;
          } else {
            o.pend_id = 0;
            o.pend_px = 0;
            o.pend_qty = 0;
            o.st = o.cum == 0 ? Live : PartFilled;
          }
        }
        if (terminal) known = false;
        order_update(m.side, known, terminal, prev);
        return;
      case CancelAck:
        if (!match) return;
        if (m.cum > o.cum) ++o_->venue_rejects;
        if (o.st == PendingReplace && m.id == o.id) {
          o.old_canceled = 1;
        } else {
          o = EngOrder{};
          terminal = true;
          known = false;
        }
        order_update(m.side, known, terminal, prev);
        return;
      case CancelReject:
        if (!match) return;
        if (o.st != PendingCancel) {
          order_update(m.side, known, false, prev);
          return;
        }
        o = EngOrder{};
        order_update(m.side, false, true, prev);
        return;
      case Fill: {
        pos_ += m.side == 0 ? m.qty : -m.qty;
        const i64 n = muldiv(m.px, m.qty, kScale);
        o_->cash += m.side == 0 ? -n : n;
        o_->fees += m.fee;
        ++o_->fills;
        if (m.liq == Taker) {
          ++o_->taker_fills;
        } else {
          ++o_->maker_fills;
        }
        o_->volume += m.qty;
        o_->fill_hash = mix(
            mix(mix(mix(o_->fill_hash, static_cast<u64>(m.vts)), m.side), static_cast<u64>(m.px)),
            static_cast<u64>(m.qty));
        if (match) {
          i64 cum = m.cum > 0 ? m.cum : o.cum + m.qty;
          if (cum < o.cum) cum = o.cum;
          if (cum > o.qty) cum = o.qty;
          o.cum = cum;
          if (o.cum >= o.qty) {
            o = EngOrder{};
            terminal = true;
            known = false;
          } else if (o.st == Live || o.st == PendingNew) {
            o.st = PartFilled;
          }
        }
        if (valid_) requote();
        if (match) order_update(m.side, known, terminal, prev);
        return;
      }
      default:
        return;
    }
  }

  Params p_;
  Inst in_;
  Shared sh_;
  const Levels* lv_;
  const i64* adds_;
  u64 nadds_;
  Out* o_;
  const Levels* book_ = nullptr;
  i64 best_[2] = {0, 0};
  i64 thr_ = 0;
  // engine
  EngOrder eng_[2];
  Slot slot_[2];
  i64 last_mid_ = 0;
  i64 pos_ = 0;
  bool pulled_ = false;
  bool valid_ = false;
  i64 mid_ = 0;
  i64 bb_ = 0;
  i64 ba_ = 0;
  i64 clock_ = 0;
  u32 next_id_ = 1;
  // venue
  VenOrder ven_[2];
  // wires
  Ring<16> out_;
  Ring<64> in_q_;
  i64 last_in_ = 0;
  i64 due_ = kMaxTime;  // the earliest message in the wires
};

}  // namespace fastmm::lk
