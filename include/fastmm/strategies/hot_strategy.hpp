#pragma once
// HotStrategy (ADR-0013, section 1): calls compiled Python hot hooks through the C ABI in
// hot_abi.h.
//
// An ordinary StrategyLike class, so the engine, risk, OMS, QuoteManager, journal and outbound hash
// are the ones C++ strategies use. Per call it fills the ctx inputs and the book view, copies the
// instrument's parameter block into the instrument's `self` record, calls the hook through a plain
// function pointer and turns the intents in ctx into one set_quotes or pull_quotes call. There is
// no Python object, GIL, allocation or virtual call on that path.
//
// Hooks run per instrument: on_book for the instrument that changed, on_fill for the fill's
// instrument, on_quoting for every instrument, on_connection for every instrument of the venue, and
// each timer hook for every instrument when its timer fires. A hook that returns a non-zero status,
// or a float level that cannot be converted, stops the strategy: every hook is disabled, the global
// kill switch trips with KillReason::StrategyError (which pulls the quotes and cancels the working
// orders) and, when the program asks for it (backtests), the engine is asked to stop.
//
// A ParamUpdate (live publishes, replay) is written into the parameter block of its instrument, or
// of every instrument, at the places HotProgram::params gives (strategies/hot_params.hpp); the next
// call copies it into `self`. on_params then calls the on_params hook for those instruments.
//
// The owner builds a HotProgram, calls attach() before the run and warm_up() before any venue
// connection, and reads error() afterwards.
//
// With a SlowChannel attached (attach_slow(), ADR-0013 slow methods), the strategy also
// records top-of-book changes, trades and fills for the slow tier and publishes a snapshot at most
// once per SlowChannelConfig::snapshot_interval of engine time; a change the interval holds back is
// published by a one-shot engine timer (tag kSlowSnapshotTimerTag) at the end of the interval.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_params.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/quoting.hpp"
#include "fastmm/strategies/slow_channel.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fastmm {

// The event hooks a hot strategy may define; timer hooks are numbered separately.
enum class HotHook : std::uint8_t {
  Book = 0,
  Fill = 1,
  Quoting = 2,
  Connection = 3,
  Params = 4,
  Count = 5
};

[[nodiscard]] constexpr std::string_view to_string(HotHook h) noexcept {
  switch (h) {
    case HotHook::Book:
      return "on_book";
    case HotHook::Fill:
      return "on_fill";
    case HotHook::Quoting:
      return "on_quoting";
    case HotHook::Connection:
      return "on_connection";
    case HotHook::Params:
      return "on_params";
    case HotHook::Count:
      break;
  }
  return "?";
}

inline constexpr std::size_t kMaxHotTimers = 16;
// Timer tags of the hot timers: this base plus the timer's index.
inline constexpr std::uint64_t kHotTimerTag = 0x484f'5400'0000'0000ULL;  // "HOT"

// What the compiler produced: hook addresses, timers and the initial `self` record.
struct HotProgram {
  struct Timer {
    fastmm_hot_fn fn = nullptr;
    Duration period{};
  };
  std::array<fastmm_hot_fn, static_cast<std::size_t>(HotHook::Count)> hooks{};
  std::array<Timer, kMaxHotTimers> timers{};
  std::size_t n_timers = 0;
  // One record: the parameter block (the first param_bytes) and the State defaults after it.
  std::vector<std::uint8_t> record;
  std::size_t param_bytes = 0;
  // The parameters a ParamUpdate may assign, by field index: their places in the parameter block.
  std::vector<HotParamSlot> params;
  bool stop_on_error = false;  // request_stop() after a failure (backtests)
  // Copy the level arrays into the book view. False when no hook can read them: the view then holds
  // the top of book and the level counts, and the arrays stay zero.
  bool book_depth = true;
};

// The first failure of a run.
struct HotError {
  std::int32_t status = FASTMM_HOT_OK;  // FASTMM_HOT_*
  std::int32_t fail_code = 0;           // FASTMM_HOT_FAILED: the code passed to ctx.fail()
  std::int32_t hook = -1;               // a HotHook, or -1 for a timer hook
  std::int32_t timer = -1;              // the timer hook's index
  Timestamp at{};
};

class HotStrategy {
 public:
  static constexpr std::string_view name() noexcept { return "python_hot"; }
  // The parameters live in the `self` records; the engine sees none. The fields of a ParamUpdate
  // index HotProgram::params, whose schema the owner records in the journal.
  static const ParamSchema& schema() {
    static const ParamSchema s{};
    return s;
  }
  std::optional<std::string> configure(const ParamMap&) { return std::nullopt; }

  // Copies the program and allocates one parameter block and one record per instrument. Startup
  // only (allocates). Returns false when the record is smaller than the parameter block, there
  // are more timers than kMaxHotTimers or more parameters than kMaxParams, or a parameter lies
  // outside the parameter block.
  bool attach(const HotProgram& p, const InstrumentTable& instruments) {
    if (p.param_bytes > p.record.size() || p.n_timers > kMaxHotTimers) return false;
    if (p.params.size() > kMaxParams) return false;
    for (const HotParamSlot& s : p.params) {
      const std::size_t width = s.type == ParamType::Bool ? 1 : 8;
      if (std::size_t{s.offset} + width > p.param_bytes) return false;
      if (s.raw_offset >= 0 && static_cast<std::size_t>(s.raw_offset) + 8 > p.param_bytes)
        return false;
    }
    program_ = p;
    record_size_ = p.record.size();
    param_bytes_ = p.param_bytes;
    const std::size_t n = instruments.size();
    records_.resize(n * record_size_);
    params_.resize(n * param_bytes_);
    for (std::size_t i = 0; i < n; ++i) {
      if (record_size_ > 0)
        std::memcpy(records_.data() + i * record_size_, p.record.data(), record_size_);
      if (param_bytes_ > 0)
        std::memcpy(params_.data() + i * param_bytes_, p.record.data(), param_bytes_);
    }
    inst_f_.resize(n);
    for (const Instrument& inst : instruments) {
      if (inst.id.value >= n) continue;
      inst_f_[inst.id.value] = {to_f(inst.tick.raw), to_f(inst.lot.raw), to_f(inst.min_qty.raw)};
    }
    hooks_ = p.hooks;
    book_depth_ = p.book_depth;
    error_ = HotError{};
    calls_ = 0;
    param_ts_.assign(n, 0);
    param_seq_.assign(n, 0);
    last_top_.assign(n, TopOfBook{});
    update_first_ = update_last_ = 0;
    update_seq_ = 0;
    next_snapshot_ = Timestamp{};
    snapshot_dirty_ = false;
    snapshot_timer_armed_ = false;
    return true;
  }

  // Connects the slow tier (sessions of a strategy with slow methods; none in replay): the strategy
  // records rows and fills into `channel` and publishes snapshots to it. `channel` must outlive the
  // run and hold at least as many instruments as the table. Startup only, after attach().
  bool attach_slow(SlowChannel* channel) noexcept {
    if (channel != nullptr && channel->instruments() < inst_f_.size()) return false;
    slow_ = channel;
    return true;
  }

  // Calls every hook once on scratch copies of ctx, book and instrument 0's record, so code pages
  // and lazily bound symbols are loaded before the first event. Nothing reaches the engine and the
  // statuses are ignored; the records are not touched. Startup only (allocates).
  void warm_up(const InstrumentTable& instruments) {
    if (records_.empty() || instruments.size() == 0) return;
    const Instrument& inst = instruments.get(InstrumentId{0});
    std::vector<std::uint8_t> scratch(record_size_);
    fastmm_hot_book b{};
    const auto call = [&](fastmm_hot_fn fn) {
      if (fn == nullptr) return;
      fastmm_hot_ctx c{};
      c.tick_raw = inst.tick.raw;
      c.lot_raw = inst.lot.raw;
      c.min_qty_raw = inst.min_qty.raw;
      c.tick = to_f(inst.tick.raw);
      c.lot = to_f(inst.lot.raw);
      c.min_qty = to_f(inst.min_qty.raw);
      std::memcpy(scratch.data(), records_.data(), record_size_);
      static_cast<void>(fn(scratch.data(), &c, &b));
    };
    for (fastmm_hot_fn fn : hooks_) call(fn);
    for (std::size_t i = 0; i < program_.n_timers; ++i) call(program_.timers[i].fn);
  }

  // ---- engine hooks ---------------------------------------------------------------------------

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    for (std::size_t i = 0; i < program_.n_timers; ++i) {
      if (program_.timers[i].fn != nullptr)
        static_cast<void>(ctx.every(program_.timers[i].period, kHotTimerTag + i));
    }
    if (slow_ != nullptr) publish_snapshot(ctx);
  }

  template <class Ctx, class Book>
  FASTMM_FORCE_INLINE void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Book)];
    if (fn != nullptr) call(ctx, id, book, fn, static_cast<std::int32_t>(HotHook::Book), -1);
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_book(ctx, id, book);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    if (!ctx.contains(fill.instrument)) return;
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_fill(ctx, fill);
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Fill)];
    if (fn == nullptr) return;
    fastmm_hot_ctx& c = cx_;
    c.fill_side = fill.side == Side::Buy ? 0 : 1;
    c.fill_maker = fill.liquidity == Liquidity::Maker ? 1 : 0;
    c.fill_price_raw = fill.price.raw;
    c.fill_qty_raw = fill.qty.raw;
    c.fill_price = to_f(fill.price.raw);
    c.fill_qty = to_f(fill.qty.raw);
    call(ctx,
         fill.instrument,
         ctx.book(fill.instrument),
         fn,
         static_cast<std::int32_t>(HotHook::Fill),
         -1);
    c.fill_side = 0;
    c.fill_maker = 0;
    c.fill_price_raw = 0;
    c.fill_qty_raw = 0;
    c.fill_price = 0.0;
    c.fill_qty = 0.0;
  }

  template <class Ctx>
  void on_quoting(Ctx& ctx, bool) noexcept {
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_touch(ctx);
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Quoting)];
    if (fn == nullptr) return;
    for (const Instrument& inst : ctx.instruments()) {
      call(ctx, inst.id, ctx.book(inst.id), fn, static_cast<std::int32_t>(HotHook::Quoting), -1);
      if (failed()) return;
    }
  }

  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_touch(ctx);
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Connection)];
    if (fn == nullptr) return;
    cx_.connected = m.state == ConnState::Live ? 1 : 0;
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.venue != m.hdr.venue) continue;
      call(ctx, inst.id, ctx.book(inst.id), fn, static_cast<std::int32_t>(HotHook::Connection), -1);
      if (failed()) break;
    }
    cx_.connected = 0;
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t tag) noexcept {
    if (tag == kSlowSnapshotTimerTag) {
      snapshot_timer_armed_ = false;
      if (slow_ != nullptr && snapshot_dirty_) publish_snapshot(ctx);
      return;
    }
    const std::uint64_t i = tag - kHotTimerTag;
    if (tag < kHotTimerTag || i >= program_.n_timers) return;
    const fastmm_hot_fn fn = program_.timers[i].fn;
    if (fn == nullptr) return;
    for (const Instrument& inst : ctx.instruments()) {
      call(ctx, inst.id, ctx.book(inst.id), fn, -1, static_cast<std::int32_t>(i));
      if (failed()) return;
    }
  }

  template <class Ctx>
  void on_trade(Ctx& ctx, InstrumentId id, const TradeMsg& t) noexcept {
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_trade(ctx, id, t);
  }

  // Engine thread: writes the message's (field, raw value) pairs into the parameter block of its
  // instrument, or of every instrument. A field without a HotProgram::params entry is skipped.
  // on_params follows at the same event.
  void apply_param_update(const ParamUpdateMsg& m) noexcept {
    const std::size_t n_inst = inst_f_.size();
    std::size_t first = 0;
    std::size_t last = n_inst;
    if (!m.all_instruments()) {
      if (m.hdr.instrument.value >= n_inst) return;
      first = m.hdr.instrument.value;
      last = first + 1;
    }
    const std::size_t count = std::min<std::size_t>(m.count, ParamUpdateMsg::kMaxFields);
    const std::vector<HotParamSlot>& slots = program_.params;
    if (param_bytes_ > 0) {
      for (std::size_t k = first; k < last; ++k) {
        std::uint8_t* const block = params_.data() + (k * param_bytes_);
        for (std::size_t i = 0; i < count; ++i) {
          if (m.field[i] < slots.size()) hot_write_param(slots[m.field[i]], block, m.value[i]);
        }
      }
    }
    update_first_ = first;
    update_last_ = last;
    update_seq_ = m.publish_seq;
  }

  // After apply_param_update(): stamps the updated instruments with the engine time and calls the
  // on_params hook for each of them.
  template <class Ctx>
  void on_params(Ctx& ctx) noexcept {
    const std::size_t first = update_first_;
    const std::size_t last = update_last_;
    update_first_ = update_last_ = 0;
    for (std::size_t k = first; k < last; ++k) {
      param_ts_[k] = ctx.now().ns;
      param_seq_[k] = update_seq_;
    }
    if (FASTMM_UNLIKELY(slow_ != nullptr)) slow_touch(ctx);
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Params)];
    if (fn == nullptr) return;
    for (std::size_t k = first; k < last; ++k) {
      InstrumentId id{};
      id.value = static_cast<decltype(id.value)>(k);
      if (!ctx.contains(id)) continue;
      call(ctx, id, ctx.book(id), fn, static_cast<std::int32_t>(HotHook::Params), -1);
      if (failed()) return;
    }
  }

  // ---- state ----------------------------------------------------------------------------------

  [[nodiscard]] bool failed() const noexcept { return error_.status != FASTMM_HOT_OK; }
  [[nodiscard]] const HotError& error() const noexcept { return error_; }
  [[nodiscard]] std::uint64_t calls() const noexcept { return calls_; }
  [[nodiscard]] std::size_t record_size() const noexcept { return record_size_; }
  // One instrument's `self` record (record_size() bytes).
  [[nodiscard]] const std::uint8_t* record(InstrumentId id) const noexcept {
    return records_.data() + static_cast<std::size_t>(id.value) * record_size_;
  }

 private:
  struct InstFloats {
    double tick = 0.0;
    double lot = 0.0;
    double min_qty = 0.0;
  };

  // Largest magnitude a 1e-8 fixed-point value holds, with margin.
  static constexpr double kMaxFixed = 9.2e10;

  [[nodiscard]] static double to_f(std::int64_t raw) noexcept {
    return static_cast<double>(raw) / static_cast<double>(kFixedScale);
  }

  template <class Book>
  FASTMM_FORCE_INLINE void fill_book(const Book& book) noexcept {
    fastmm_hot_book& b = bk_;
    const Level bb = book.best_bid();
    const Level ba = book.best_ask();
    const Price mid = book.mid();
    b.ts_ns = book.last_update().ns;
    b.valid = book.is_valid() ? 1 : 0;
    b.mid_raw = mid.raw;
    b.best_bid_raw = bb.price.raw;
    b.best_ask_raw = ba.price.raw;
    b.best_bid_qty_raw = bb.qty.raw;
    b.best_ask_qty_raw = ba.qty.raw;
    b.mid = to_f(mid.raw);
    b.best_bid = to_f(bb.price.raw);
    b.best_ask = to_f(ba.price.raw);
    b.best_bid_qty = to_f(bb.qty.raw);
    b.best_ask_qty = to_f(ba.qty.raw);
    if (!book_depth_) {
      b.n_bids = static_cast<std::int32_t>(
          std::min<std::size_t>(book.depth(Side::Buy), FASTMM_HOT_BOOK_DEPTH));
      b.n_asks = static_cast<std::int32_t>(
          std::min<std::size_t>(book.depth(Side::Sell), FASTMM_HOT_BOOK_DEPTH));
      return;
    }
    const auto nb = static_cast<std::int32_t>(
        std::min<std::size_t>(book.depth(Side::Buy), FASTMM_HOT_BOOK_DEPTH));
    const auto na = static_cast<std::int32_t>(
        std::min<std::size_t>(book.depth(Side::Sell), FASTMM_HOT_BOOK_DEPTH));
    for (std::int32_t i = 0; i < nb; ++i) {
      const Level l = book.level(Side::Buy, static_cast<std::size_t>(i));
      b.bid_px_raw[i] = l.price.raw;
      b.bid_qty_raw[i] = l.qty.raw;
      b.bid_px[i] = to_f(l.price.raw);
      b.bid_qty[i] = to_f(l.qty.raw);
    }
    for (std::int32_t i = 0; i < na; ++i) {
      const Level l = book.level(Side::Sell, static_cast<std::size_t>(i));
      b.ask_px_raw[i] = l.price.raw;
      b.ask_qty_raw[i] = l.qty.raw;
      b.ask_px[i] = to_f(l.price.raw);
      b.ask_qty[i] = to_f(l.qty.raw);
    }
    // Levels past the ones present read as zero (only the slots the previous call used).
    for (std::int32_t i = nb; i < b.n_bids; ++i) {
      b.bid_px_raw[i] = 0;
      b.bid_qty_raw[i] = 0;
      b.bid_px[i] = 0.0;
      b.bid_qty[i] = 0.0;
    }
    for (std::int32_t i = na; i < b.n_asks; ++i) {
      b.ask_px_raw[i] = 0;
      b.ask_qty_raw[i] = 0;
      b.ask_px[i] = 0.0;
      b.ask_qty[i] = 0.0;
    }
    b.n_bids = nb;
    b.n_asks = na;
  }

  template <class Ctx, class Book>
  FASTMM_FORCE_INLINE void call(Ctx& ctx,
                                InstrumentId id,
                                const Book& book,
                                fastmm_hot_fn fn,
                                std::int32_t hook,
                                std::int32_t timer) noexcept {
    const Instrument& inst = ctx.instrument(id);
    const InstFloats& f = inst_f_[id.value];
    fastmm_hot_ctx& c = cx_;
    c.now_ns = ctx.now().ns;
    c.instrument = static_cast<std::int32_t>(id.value);
    c.quoting_enabled = (ctx.quoting_enabled() && !ctx.venue_killed(inst.venue)) ? 1 : 0;
    c.tick_raw = inst.tick.raw;
    c.lot_raw = inst.lot.raw;
    c.min_qty_raw = inst.min_qty.raw;
    c.tick = f.tick;
    c.lot = f.lot;
    c.min_qty = f.min_qty;
    const Qty pos = ctx.position(id).qty;
    c.position_raw = pos.raw;
    c.position = to_f(pos.raw);
    c.action = FASTMM_HOT_ACTION_NONE;
    c.flags = 0;
    c.n_bids = 0;
    c.n_asks = 0;
    c.status = FASTMM_HOT_OK;
    c.fail_code = 0;
    fill_book(book);
    std::uint8_t* const self = records_.data() + static_cast<std::size_t>(id.value) * record_size_;
    std::memcpy(
        self, params_.data() + static_cast<std::size_t>(id.value) * param_bytes_, param_bytes_);
    ++calls_;
    const std::int32_t rc = fn(self, &c, &bk_);
    if (FASTMM_UNLIKELY(rc != FASTMM_HOT_OK)) {
      fail(ctx, rc, c.fail_code, hook, timer);
      return;
    }
    if (c.action == FASTMM_HOT_ACTION_NONE) return;
    if (c.action == FASTMM_HOT_ACTION_PULL) {
      ctx.pull_quotes(id);
      return;
    }
    if (FASTMM_LIKELY(build_quotes(inst))) {
      if ((c.flags & FASTMM_HOT_FLAG_UNCROSS) != 0) q_.uncross(inst.tick);
      if ((c.flags & FASTMM_HOT_FLAG_KEEP_PASSIVE) != 0)
        keep_passive(q_, book.best_bid().price, book.best_ask().price, inst.tick);
      static_cast<void>(ctx.set_quotes(id, q_));
    } else {
      fail(ctx, FASTMM_HOT_BAD_VALUE, 0, hook, timer);
    }
  }

  // A float level: nearest 1e-8, then the instrument's tick (bids down, asks up) and lot (down).
  [[nodiscard]] static bool float_level(const Instrument& inst,
                                        Side side,
                                        double px,
                                        double qty,
                                        Price& out_px,
                                        Qty& out_qty) noexcept {
    if (!(std::fabs(px) <= kMaxFixed) || !(qty >= 0.0 && qty <= kMaxFixed)) return false;
    out_px = inst.round_price(Price::from_double(px), side);
    out_qty = inst.round_qty(Qty::from_double(qty));
    return true;
  }

  // ctx levels -> q_. False when a float level is not finite, out of range or has a negative
  // quantity. A non-positive price or a zero quantity drops the level (DesiredQuotes::bid/ask).
  [[nodiscard]] bool build_quotes(const Instrument& inst) noexcept {
    const fastmm_hot_ctx& c = cx_;
    q_.clear();
    const std::int32_t nb = std::clamp<std::int32_t>(c.n_bids, 0, FASTMM_HOT_QUOTE_LEVELS);
    const std::int32_t na = std::clamp<std::int32_t>(c.n_asks, 0, FASTMM_HOT_QUOTE_LEVELS);
    Price px;
    Qty qty;
    for (std::int32_t i = 0; i < nb; ++i) {
      if (c.bid_is_raw[i] != 0) {
        static_cast<void>(
            q_.bid(Price::from_raw(c.bid_px_raw[i]), Qty::from_raw(c.bid_qty_raw[i])));
      } else {
        if (!float_level(inst, Side::Buy, c.bid_px[i], c.bid_qty[i], px, qty)) return false;
        static_cast<void>(q_.bid(px, qty));
      }
    }
    for (std::int32_t i = 0; i < na; ++i) {
      if (c.ask_is_raw[i] != 0) {
        static_cast<void>(
            q_.ask(Price::from_raw(c.ask_px_raw[i]), Qty::from_raw(c.ask_qty_raw[i])));
      } else {
        if (!float_level(inst, Side::Sell, c.ask_px[i], c.ask_qty[i], px, qty)) return false;
        static_cast<void>(q_.ask(px, qty));
      }
    }
    return true;
  }

  template <class Ctx>
  FASTMM_NOINLINE void fail(Ctx& ctx,
                            std::int32_t status,
                            std::int32_t code,
                            std::int32_t hook,
                            std::int32_t timer) noexcept {
    if (!failed()) {
      error_.status = status;
      error_.fail_code = code;
      error_.hook = hook;
      error_.timer = timer;
      error_.at = ctx.now();
    }
    hooks_.fill(nullptr);
    for (HotProgram::Timer& t : program_.timers) t.fn = nullptr;
    ctx.trip_kill(KillReason::StrategyError);  // pulls the quotes and cancels the working orders
    if (program_.stop_on_error) ctx.request_stop();
  }

  struct TopOfBook {
    std::int64_t bid = 0;
    std::int64_t bid_qty = 0;
    std::int64_t ask = 0;
    std::int64_t ask_qty = 0;
  };

  // ---- slow tier --------------------------------------------------------------------------------

  template <class Ctx, class Book>
  FASTMM_NOINLINE void slow_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (id.value < last_top_.size()) {
      const Level bb = book.best_bid();
      const Level ba = book.best_ask();
      TopOfBook& top = last_top_[id.value];
      if (bb.price.raw != top.bid || bb.qty.raw != top.bid_qty || ba.price.raw != top.ask ||
          ba.qty.raw != top.ask_qty) {
        top = TopOfBook{bb.price.raw, bb.qty.raw, ba.price.raw, ba.qty.raw};
        SlowRecentRow row{};
        row.ts_ns = ctx.now().ns;
        row.bid_raw = top.bid;
        row.bid_qty_raw = top.bid_qty;
        row.ask_raw = top.ask;
        row.ask_qty_raw = top.ask_qty;
        row.kind = SlowRecentRow::kTop;
        slow_->record(id, row);
      }
    }
    slow_touch(ctx);
  }

  template <class Ctx>
  FASTMM_NOINLINE void slow_trade(Ctx& ctx, InstrumentId id, const TradeMsg& t) noexcept {
    if (!ctx.contains(id)) return;
    const auto& book = ctx.book(id);
    const Level bb = book.best_bid();
    const Level ba = book.best_ask();
    SlowRecentRow row{};
    row.ts_ns = ctx.now().ns;
    row.bid_raw = bb.price.raw;
    row.bid_qty_raw = bb.qty.raw;
    row.ask_raw = ba.price.raw;
    row.ask_qty_raw = ba.qty.raw;
    row.trade_price_raw = t.price.raw;
    row.trade_qty_raw = t.qty.raw;
    row.kind = SlowRecentRow::kTrade;
    row.side = t.aggressor == Side::Buy ? 0 : 1;
    slow_->record(id, row);
    slow_touch(ctx);
  }

  template <class Ctx>
  FASTMM_NOINLINE void slow_fill(Ctx& ctx, const Fill& fill) noexcept {
    SlowFill f{};
    f.ts_ns = ctx.now().ns;
    f.price_raw = fill.price.raw;
    f.qty_raw = fill.qty.raw;
    f.fee_raw = fill.fee.raw;
    f.position_raw = ctx.position(fill.instrument).qty.raw;
    f.instrument = static_cast<std::uint32_t>(fill.instrument.value);
    f.side = fill.side == Side::Buy ? 0 : 1;
    f.maker = fill.liquidity == Liquidity::Maker ? 1 : 0;
    static_cast<void>(slow_->push_fill(f));  // a full ring records SlowFailure::FillsOverflow
    slow_touch(ctx);
  }

  // Publishes a snapshot when the interval since the last one has passed; otherwise marks it
  // pending and arms a one-shot timer for the end of the interval.
  template <class Ctx>
  FASTMM_NOINLINE void slow_touch(Ctx& ctx) noexcept {
    const Timestamp now = ctx.now();
    if (now >= next_snapshot_) {
      publish_snapshot(ctx);
      return;
    }
    snapshot_dirty_ = true;
    if (!snapshot_timer_armed_)
      snapshot_timer_armed_ = ctx.once(next_snapshot_ - now, kSlowSnapshotTimerTag).valid();
  }

  template <class Ctx>
  void publish_snapshot(Ctx& ctx) noexcept {
    const Timestamp now = ctx.now();
    const bool enabled = ctx.quoting_enabled();
    slow_->write_snapshot(
        now, enabled, ctx.killed(), [&](SlowInstrumentState* states, std::size_t cap) noexcept {
          for (const Instrument& inst : ctx.instruments()) {
            const std::size_t k = inst.id.value;
            if (k >= cap || k >= param_ts_.size()) continue;
            const auto& book = ctx.book(inst.id);
            const Level bb = book.best_bid();
            const Level ba = book.best_ask();
            const Position& pos = ctx.position(inst.id);
            SlowInstrumentState& s = states[k];
            s.book_ts_ns = book.last_update().ns;
            s.best_bid_raw = bb.price.raw;
            s.best_bid_qty_raw = bb.qty.raw;
            s.best_ask_raw = ba.price.raw;
            s.best_ask_qty_raw = ba.qty.raw;
            s.mid_raw = book.mid().raw;
            s.position_raw = pos.qty.raw;
            s.avg_price_raw = pos.avg_px.raw;
            s.realized_pnl_raw = pos.realized.raw;
            s.unrealized_pnl_raw = pos.unrealized.raw;
            s.fees_raw = pos.fees.raw;
            s.bid_open_qty_raw = ctx.open_qty(inst.id, Side::Buy).raw;
            s.ask_open_qty_raw = ctx.open_qty(inst.id, Side::Sell).raw;
            s.param_ts_ns = param_ts_[k];
            s.param_seq = param_seq_[k];
            s.fills = pos.fills;
            s.book_valid = book.is_valid() ? 1 : 0;
            s.quoting = enabled && !ctx.venue_killed(inst.venue) ? 1 : 0;
          }
        });
    next_snapshot_ = now + slow_->config().snapshot_interval;
    snapshot_dirty_ = false;
  }

  HotProgram program_{};
  std::array<fastmm_hot_fn, static_cast<std::size_t>(HotHook::Count)> hooks_{};
  std::size_t record_size_ = 0;
  std::size_t param_bytes_ = 0;
  bool book_depth_ = true;
  std::vector<std::uint8_t> records_;
  std::vector<std::uint8_t> params_;
  std::vector<InstFloats> inst_f_;
  fastmm_hot_ctx cx_{};
  fastmm_hot_book bk_{};
  DesiredQuotes q_{};
  HotError error_{};
  std::uint64_t calls_ = 0;
  // parameter updates and the slow tier
  SlowChannel* slow_ = nullptr;
  std::vector<std::int64_t> param_ts_;
  std::vector<std::uint64_t> param_seq_;
  std::vector<TopOfBook> last_top_;
  std::size_t update_first_ = 0;
  std::size_t update_last_ = 0;
  std::uint64_t update_seq_ = 0;
  Timestamp next_snapshot_{};
  bool snapshot_dirty_ = false;
  bool snapshot_timer_armed_ = false;
};

static_assert(StrategyLike<HotStrategy>);
static_assert(verify_strategy<HotStrategy>());

}  // namespace fastmm
