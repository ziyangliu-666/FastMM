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
// The owner builds a HotProgram, calls attach() before the run and warm_up() before any venue
// connection, and reads error() afterwards.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/quoting.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

// The event hooks a hot strategy may define; timer hooks are numbered separately.
enum class HotHook : std::uint8_t { Book = 0, Fill = 1, Quoting = 2, Connection = 3, Count = 4 };

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
  // The parameters live in the `self` records; the engine sees none.
  static const ParamSchema& schema() {
    static const ParamSchema s{};
    return s;
  }
  std::optional<std::string> configure(const ParamMap&) { return std::nullopt; }

  // Copies the program and allocates one parameter block and one record per instrument. Startup
  // only (allocates). Returns false when the record is smaller than the parameter block or there
  // are more timers than kMaxHotTimers.
  bool attach(const HotProgram& p, const InstrumentTable& instruments) {
    if (p.param_bytes > p.record.size() || p.n_timers > kMaxHotTimers) return false;
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
  }

  template <class Ctx, class Book>
  FASTMM_FORCE_INLINE void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Book)];
    if (fn != nullptr) call(ctx, id, book, fn, static_cast<std::int32_t>(HotHook::Book), -1);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Fill)];
    if (fn == nullptr || !ctx.contains(fill.instrument)) return;
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
    const fastmm_hot_fn fn = hooks_[static_cast<std::size_t>(HotHook::Quoting)];
    if (fn == nullptr) return;
    for (const Instrument& inst : ctx.instruments()) {
      call(ctx, inst.id, ctx.book(inst.id), fn, static_cast<std::int32_t>(HotHook::Quoting), -1);
      if (failed()) return;
    }
  }

  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
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
    const std::uint64_t i = tag - kHotTimerTag;
    if (tag < kHotTimerTag || i >= program_.n_timers) return;
    const fastmm_hot_fn fn = program_.timers[i].fn;
    if (fn == nullptr) return;
    for (const Instrument& inst : ctx.instruments()) {
      call(ctx, inst.id, ctx.book(inst.id), fn, -1, static_cast<std::int32_t>(i));
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
};

static_assert(StrategyLike<HotStrategy>);
static_assert(verify_strategy<HotStrategy>());

}  // namespace fastmm
