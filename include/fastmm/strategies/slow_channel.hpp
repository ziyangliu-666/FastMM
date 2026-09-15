#pragma once
// SlowChannel: what the engine thread shares with the slow methods of a Python strategy (ADR-0013,
// sections 1 and 4). A session and its slow tier own one jointly (std::shared_ptr), so it outlives
// either.
//
//   engine thread (HotStrategy)              slow tier (one thread)          other threads
//   write_snapshot()  at a bounded rate      try_snapshot()                  publish()
//   record()          top of book, trades    recent()
//   push_fill()       overflow: failure      drain_fills()
//                                            begin_call() / end_call()       check() (watchdog)
//
// Parameter updates go through publish() to a ParamSink: by default param_ring(), which a live
// engine's RingFeed polls; a backtest sets a sink that schedules them at simulated times. The
// engine side never waits and allocates nothing; memory is allocated in the constructor. Nothing
// here calls Python.
//
// The snapshot is a seqlock over a header and one SlowInstrumentState per instrument. The recent
// rows of an instrument are a window that keeps the newest `recent_rows` rows in a ring of twice
// that size; a reader copies the window and discards rows the writer overwrote during the copy. The
// fills ring has one producer (the engine) and one consumer (the slow tier); a fill that does not
// fit records SlowFailure::FillsOverflow.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace fastmm {

// Tag of the engine timer that publishes a snapshot the rate limit held back.
inline constexpr std::uint64_t kSlowSnapshotTimerTag = 0x534c'4f57'0000'0000ULL;  // "SLOW"

// One instrument in a snapshot. Prices, quantities and money are raw fixed point (1e-8).
struct SlowInstrumentState {
  std::int64_t book_ts_ns;  // last book update, engine time (0: none)
  std::int64_t best_bid_raw;
  std::int64_t best_bid_qty_raw;
  std::int64_t best_ask_raw;
  std::int64_t best_ask_qty_raw;
  std::int64_t mid_raw;
  std::int64_t position_raw;
  std::int64_t avg_price_raw;
  std::int64_t realized_pnl_raw;
  std::int64_t unrealized_pnl_raw;  // Position::unrealized at its last mark
  std::int64_t fees_raw;
  std::int64_t bid_open_qty_raw;  // unfilled quantity of open orders, quotes included
  std::int64_t ask_open_qty_raw;
  std::int64_t param_ts_ns;  // engine time of the instrument's last ParamUpdate (0: none)
  std::uint64_t param_seq;   // that update's publish_seq
  std::uint32_t fills;
  std::uint8_t book_valid;
  std::uint8_t quoting;  // quoting is enabled and the instrument's venue is not killed
  std::uint8_t pad_[2];
};
static_assert(sizeof(SlowInstrumentState) == 128 &&
              std::is_trivially_copyable_v<SlowInstrumentState>);

struct SlowSnapshotHeader {
  std::int64_t ts_ns;     // engine time of the snapshot (0: none published yet)
  std::uint64_t version;  // snapshots published
  std::uint32_t instruments;
  std::uint8_t quoting_enabled;  // global quoting flag (false while parameters are stale)
  std::uint8_t killed;           // the global kill switch
  std::uint8_t pad_[10];
};
static_assert(sizeof(SlowSnapshotHeader) == 32 && std::is_trivially_copyable_v<SlowSnapshotHeader>);

// A row of the recent window: a change of the top of book, or a trade with the top of book at it.
struct SlowRecentRow {
  static constexpr std::uint8_t kTop = 0;
  static constexpr std::uint8_t kTrade = 1;
  std::int64_t ts_ns;  // engine time
  std::int64_t bid_raw;
  std::int64_t bid_qty_raw;
  std::int64_t ask_raw;
  std::int64_t ask_qty_raw;
  std::int64_t trade_price_raw;  // kTrade rows
  std::int64_t trade_qty_raw;
  std::uint8_t kind;
  std::uint8_t side;  // kTrade: the aggressor, 0 buy, 1 sell
  std::uint8_t pad_[6];
};
static_assert(sizeof(SlowRecentRow) == 64 && std::is_trivially_copyable_v<SlowRecentRow>);

struct SlowFill {
  std::uint64_t seq;   // from 1, one per fill
  std::int64_t ts_ns;  // engine time
  std::int64_t price_raw;
  std::int64_t qty_raw;
  std::int64_t fee_raw;       // quote currency, as booked
  std::int64_t position_raw;  // after the fill
  std::uint32_t instrument;
  std::uint8_t side;   // 0 buy, 1 sell
  std::uint8_t maker;  // 1 when the fill added liquidity
  std::uint8_t pad_[10];
};
static_assert(sizeof(SlowFill) == 64 && std::is_trivially_copyable_v<SlowFill>);

// Why the slow tier failed; the first failure is kept.
enum class SlowFailure : std::uint8_t {
  None = 0,
  Exception = 1,      // a slow method raised
  Timeout = 2,        // a call ran past its timeout
  FillsOverflow = 3,  // the engine found the fills ring full
  ThreadExited = 4,   // the slow thread ended while the session ran
};
[[nodiscard]] constexpr std::string_view to_string(SlowFailure f) noexcept {
  switch (f) {
    case SlowFailure::None:
      return "None";
    case SlowFailure::Exception:
      return "Exception";
    case SlowFailure::Timeout:
      return "Timeout";
    case SlowFailure::FillsOverflow:
      return "FillsOverflow";
    case SlowFailure::ThreadExited:
      return "ThreadExited";
  }
  return "?";
}

struct SlowChannelConfig {
  std::size_t instruments = 1;
  std::size_t fills_capacity = 1U << 16;  // rounded up to a power of two
  std::size_t recent_rows = 4096;         // per instrument, rounded up to a power of two
  Duration snapshot_interval = milliseconds(10);
  std::size_t param_ring_bytes = 1U << 16;  // rounded up to a power of two, at least 4096
};

// Fills ring capacity for a session: 4 fills per order at the order rate limit (1000 orders/s when
// unlimited) over the longest time the slow tier may leave the ring undrained plus 1 s; at least
// 4096 and at most 2^24, rounded up to a power of two.
[[nodiscard]] inline std::size_t slow_fills_capacity(std::uint32_t orders_per_sec,
                                                     Duration longest_gap) noexcept {
  const std::uint64_t rate = orders_per_sec > 0 ? orders_per_sec : 1000U;
  const std::uint64_t gap_ns = longest_gap.ns > 0 ? static_cast<std::uint64_t>(longest_gap.ns) : 0U;
  const std::uint64_t seconds = ((gap_ns + 999'999'999U) / 1'000'000'000U) + 1U;
  const std::uint64_t n = std::clamp<std::uint64_t>(4U * rate * seconds, 4096U, 1U << 24);
  return std::bit_ceil(n);
}

class SlowChannel {
 public:
  explicit SlowChannel(const SlowChannelConfig& cfg)
      : cfg_(normalised(cfg)),
        states_(std::make_unique<SlowInstrumentState[]>(cfg_.instruments)),
        recent_(std::make_unique<SlowRecentRow[]>(cfg_.instruments * 2 * cfg_.recent_rows)),
        written_(std::make_unique<std::atomic<std::uint64_t>[]>(cfg_.instruments)),
        fills_(std::make_unique<SlowFill[]>(cfg_.fills_capacity)),
        param_ring_(cfg_.param_ring_bytes),
        sink_(ParamSink::to_ring(param_ring_)) {
    header_.instruments = static_cast<std::uint32_t>(cfg_.instruments);
    for (std::size_t i = 0; i < cfg_.instruments; ++i)
      written_[i].store(0, std::memory_order_relaxed);
  }
  SlowChannel(const SlowChannel&) = delete;
  SlowChannel& operator=(const SlowChannel&) = delete;
  SlowChannel(SlowChannel&&) = delete;
  SlowChannel& operator=(SlowChannel&&) = delete;
  ~SlowChannel() = default;

  [[nodiscard]] const SlowChannelConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] std::size_t instruments() const noexcept { return cfg_.instruments; }

  // ---- parameters (any thread) ----------------------------------------------------------------

  // The ring a live engine's RingFeed polls; the default sink.
  [[nodiscard]] MsgRing& param_ring() noexcept { return param_ring_; }
  // Where publish() sends updates. Set before the first publish.
  void set_param_sink(ParamSink sink) {
    const std::lock_guard<std::mutex> lock(mutex_);
    sink_ = sink;
  }

  // Sends one validated update: `fields` and `values` are schema indices and raw values, `inst` one
  // instrument or ParamUpdateMsg::kAllInstruments. Throws std::invalid_argument when the sizes
  // differ or exceed ParamUpdateMsg::kMaxFields; returns false when the sink refused the update or
  // the channel is closed. A mutex serialises calls, so the sink has one producer at a time.
  bool publish(InstrumentId inst,
               std::span<const std::uint16_t> fields,
               std::span<const std::int64_t> values) {
    if (fields.size() != values.size() || fields.size() > ParamUpdateMsg::kMaxFields) {
      throw std::invalid_argument("slow channel: at most " +
                                  std::to_string(ParamUpdateMsg::kMaxFields) +
                                  " (field, value) pairs of equal count per update");
    }
    ParamUpdateMsg m{};
    init_header(m, EventType::ParamUpdate, inst);
    m.hdr.flags = EventHeader::kSynthetic;
    m.count = static_cast<std::uint32_t>(fields.size());
    std::copy(fields.begin(), fields.end(), m.field);
    std::copy(values.begin(), values.end(), m.value);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    m.publish_seq = seq_ + 1;
    m.hdr.recv_ts = wall_now();
    if (sink_.push == nullptr || !sink_.push(sink_.ctx, m)) {
      ++refused_;
      return false;
    }
    ++seq_;
    return true;
  }
  // Later publishes return false (the session has stopped).
  void close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  [[nodiscard]] bool closed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }
  [[nodiscard]] std::uint64_t published() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
  }
  [[nodiscard]] std::uint64_t refused() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return refused_;
  }

  // ---- engine thread --------------------------------------------------------------------------

  // Publishes a snapshot: `fill(SlowInstrumentState* states, std::size_t n)` writes the instruments
  // while readers retry.
  template <class Fill>
  void write_snapshot(Timestamp now, bool quoting_enabled, bool killed, Fill&& fill) noexcept {
#ifdef FASTMM_SEQLOCK_TSAN
    const Guard guard(copy_lock_);
#endif
    const std::uint64_t s = snap_seq_.load(std::memory_order_relaxed);
    snap_seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    header_.ts_ns = now.ns;
    header_.version += 1;
    header_.quoting_enabled = quoting_enabled ? 1 : 0;
    header_.killed = killed ? 1 : 0;
    static_cast<Fill&&>(fill)(states_.get(), cfg_.instruments);
    std::atomic_thread_fence(std::memory_order_release);
    snap_seq_.store(s + 2, std::memory_order_relaxed);
  }

  // Appends a row to the instrument's recent window, overwriting the oldest.
  FASTMM_FORCE_INLINE void record(InstrumentId id, const SlowRecentRow& row) noexcept {
    if (FASTMM_UNLIKELY(id.value >= cfg_.instruments)) return;
#ifdef FASTMM_SEQLOCK_TSAN
    const Guard guard(copy_lock_);
#endif
    std::atomic<std::uint64_t>& w = written_[id.value];
    const std::uint64_t n = w.load(std::memory_order_relaxed);
    const std::size_t ring = 2 * cfg_.recent_rows;
    recent_[(id.value * ring) + (n & (ring - 1))] = row;
    w.store(n + 1, std::memory_order_release);
  }

  // Appends a fill and numbers it. False, with SlowFailure::FillsOverflow recorded, when the ring
  // is full; the fill is lost then and its number skipped.
  FASTMM_FORCE_INLINE bool push_fill(const SlowFill& f) noexcept {
    const std::uint64_t seq = ++fill_seq_;
    const std::uint64_t t = fills_tail_.load(std::memory_order_relaxed);
    if (FASTMM_UNLIKELY(t - fills_head_.load(std::memory_order_acquire) >= cfg_.fills_capacity)) {
      fail(SlowFailure::FillsOverflow);
      return false;
    }
    SlowFill& slot = fills_[t & (cfg_.fills_capacity - 1)];
    slot = f;
    slot.seq = seq;
    fills_tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // ---- slow tier ------------------------------------------------------------------------------

  // One attempt to copy the latest snapshot; false while it is being written. `out` receives at
  // most out.size() instruments.
  [[nodiscard]] bool try_snapshot(SlowSnapshotHeader& header,
                                  std::span<SlowInstrumentState> out) const noexcept {
    const std::uint64_t s1 = snap_seq_.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0) return false;
    {
#ifdef FASTMM_SEQLOCK_TSAN
      const Guard guard(copy_lock_);
#endif
      std::memcpy(&header, &header_, sizeof header);
      const std::size_t n = std::min(out.size(), cfg_.instruments);
      if (n > 0) std::memcpy(out.data(), states_.get(), n * sizeof(SlowInstrumentState));
      std::atomic_thread_fence(std::memory_order_acquire);
    }
    return s1 == snap_seq_.load(std::memory_order_relaxed);
  }
  // Spins until a consistent copy is obtained.
  void snapshot(SlowSnapshotHeader& header, std::span<SlowInstrumentState> out) const noexcept {
    while (!try_snapshot(header, out)) __builtin_ia32_pause();
  }

  struct RecentRead {
    std::size_t rows = 0;       // rows written to `out`, oldest first
    std::uint64_t dropped = 0;  // rows written after `cursor` that left the window before this read
  };
  // Copies the newest rows of the instrument's window (at most out.size()). `cursor` is the
  // reader's position: rows written after it that are no longer in the window count as dropped, and
  // it moves to the end of what was copied.
  RecentRead recent(InstrumentId id,
                    std::span<SlowRecentRow> out,
                    std::uint64_t& cursor) const noexcept {
    RecentRead r;
    if (id.value >= cfg_.instruments || out.empty()) return r;
    const std::size_t ring = 2 * cfg_.recent_rows;
    const std::atomic<std::uint64_t>& w = written_[id.value];
    const SlowRecentRow* base = &recent_[id.value * ring];
    for (;;) {
      const std::uint64_t w1 = w.load(std::memory_order_acquire);
      const std::uint64_t keep = std::min<std::uint64_t>(cfg_.recent_rows, out.size());
      const std::uint64_t lo = w1 > keep ? w1 - keep : 0;
      {
#ifdef FASTMM_SEQLOCK_TSAN
        const Guard guard(copy_lock_);
#endif
        for (std::uint64_t i = lo; i < w1; ++i) out[i - lo] = base[i & (ring - 1)];
      }
      const std::uint64_t w2 = w.load(std::memory_order_acquire);
      // Row i is being or has been overwritten once the writer has reached row i + ring.
      const std::uint64_t valid_from = w2 >= ring ? w2 - ring + 1 : 0;
      if (w1 > 0 && valid_from >= w1) continue;  // every copied row was overwritten: copy again
      const std::uint64_t first = std::max(lo, valid_from);
      if (first > lo)
        std::memmove(out.data(), &out[first - lo], (w1 - first) * sizeof(SlowRecentRow));
      r.rows = w1 - first;
      r.dropped = first > cursor ? first - cursor : 0;
      cursor = w1;
      return r;
    }
  }
  // Rows the engine has written to the instrument's window.
  [[nodiscard]] std::uint64_t recent_written(InstrumentId id) const noexcept {
    return id.value < cfg_.instruments ? written_[id.value].load(std::memory_order_acquire) : 0;
  }

  // Moves up to out.size() fills out of the ring, oldest first.
  std::size_t drain_fills(std::span<SlowFill> out) noexcept {
    const std::uint64_t h = fills_head_.load(std::memory_order_relaxed);
    const std::uint64_t t = fills_tail_.load(std::memory_order_acquire);
    const std::size_t n = std::min<std::uint64_t>(t - h, out.size());
    for (std::size_t i = 0; i < n; ++i) out[i] = fills_[(h + i) & (cfg_.fills_capacity - 1)];
    fills_head_.store(h + n, std::memory_order_release);
    return n;
  }
  [[nodiscard]] std::size_t fills_pending() const noexcept {
    return fills_tail_.load(std::memory_order_acquire) -
           fills_head_.load(std::memory_order_acquire);
  }

  // ---- watchdog -------------------------------------------------------------------------------
  // Times are steady-clock nanoseconds (std::chrono::steady_clock, Python time.monotonic_ns()).

  void heartbeat(std::int64_t steady_ns) noexcept {
    heartbeat_ns_.store(steady_ns, std::memory_order_relaxed);
  }
  // A slow call starts; check() reports SlowFailure::Timeout once `timeout` has passed.
  void begin_call(std::int64_t steady_ns, Duration timeout) noexcept {
    heartbeat(steady_ns);
    deadline_ns_.store(steady_ns + timeout.ns, std::memory_order_release);
  }
  void end_call(std::int64_t steady_ns) noexcept {
    deadline_ns_.store(0, std::memory_order_release);
    heartbeat(steady_ns);
  }
  // Records `f` unless a failure is already recorded; true when `f` is the one kept.
  bool fail(SlowFailure f) noexcept {
    std::uint8_t none = 0;
    return failure_.compare_exchange_strong(
        none, static_cast<std::uint8_t>(f), std::memory_order_acq_rel);
  }
  [[nodiscard]] SlowFailure failure() const noexcept {
    return static_cast<SlowFailure>(failure_.load(std::memory_order_acquire));
  }
  // The control thread's check: records SlowFailure::Timeout when a call has run past its deadline,
  // then returns the recorded failure.
  SlowFailure check(std::int64_t steady_ns) noexcept {
    const std::int64_t d = deadline_ns_.load(std::memory_order_acquire);
    if (d != 0 && steady_ns > d) fail(SlowFailure::Timeout);
    return failure();
  }
  [[nodiscard]] std::int64_t last_heartbeat() const noexcept {
    return heartbeat_ns_.load(std::memory_order_relaxed);
  }
  // Deadline of the running call (0: no call running).
  [[nodiscard]] std::int64_t call_deadline() const noexcept {
    return deadline_ns_.load(std::memory_order_acquire);
  }

 private:
#ifdef FASTMM_SEQLOCK_TSAN
  // Under ThreadSanitizer the optimistic copies are serialised, as in Seqlocked.
  class Guard {
   public:
    explicit Guard(std::atomic_flag& flag) noexcept : flag_(flag) {
      while (flag_.test_and_set(std::memory_order_acquire)) __builtin_ia32_pause();
    }
    ~Guard() { flag_.clear(std::memory_order_release); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&&) = delete;
    Guard& operator=(Guard&&) = delete;

   private:
    std::atomic_flag& flag_;
  };
  mutable std::atomic_flag copy_lock_ = ATOMIC_FLAG_INIT;
#endif

  static SlowChannelConfig normalised(SlowChannelConfig c) {
    if (c.instruments == 0) throw std::invalid_argument("slow channel: no instruments");
    c.fills_capacity = std::bit_ceil(std::max<std::size_t>(c.fills_capacity, 2));
    c.recent_rows = std::bit_ceil(std::max<std::size_t>(c.recent_rows, 2));
    c.param_ring_bytes = std::bit_ceil(std::max<std::size_t>(c.param_ring_bytes, 1U << 12));
    if (c.snapshot_interval.ns < 0) c.snapshot_interval = Duration{};
    return c;
  }

  SlowChannelConfig cfg_;
  // snapshot (the engine writes, readers retry)
  alignas(kCacheLine) std::atomic<std::uint64_t> snap_seq_{0};
  SlowSnapshotHeader header_{};
  std::unique_ptr<SlowInstrumentState[]> states_;
  // recent windows
  std::unique_ptr<SlowRecentRow[]> recent_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> written_;
  // fills ring
  std::unique_ptr<SlowFill[]> fills_;
  alignas(kCacheLine) std::atomic<std::uint64_t> fills_tail_{0};
  std::uint64_t fill_seq_ = 0;  // engine thread
  alignas(kCacheLine) std::atomic<std::uint64_t> fills_head_{0};
  // watchdog
  alignas(kCacheLine) std::atomic<std::int64_t> heartbeat_ns_{0};
  std::atomic<std::int64_t> deadline_ns_{0};
  std::atomic<std::uint8_t> failure_{0};
  // parameters
  MsgRing param_ring_;
  mutable std::mutex mutex_;
  ParamSink sink_;
  std::uint64_t seq_ = 0;
  std::uint64_t refused_ = 0;
  bool closed_ = false;
};

}  // namespace fastmm
