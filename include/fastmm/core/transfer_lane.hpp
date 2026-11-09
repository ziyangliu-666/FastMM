#pragma once
// The transfer lane of an account pool: internal transfers the engine itself decides, holds,
// budgets and confirms, and the sweep that uses them to keep each spot position on its home
// ([venues.<primary>.pool] sweep, [venues.<primary>.transfers]; core/pool_homes.hpp).
//
// A transfer is an engine action with the shape of an order. The engine creates it from journaled
// inputs (the sweep, on its own timer): a sequence number, the payer, the receiver, the asset and
// the amount, and in the same step a hold of the amount on the payer's free estimate
// (BalanceBook::reserve), so no order is sized against it. It journals the intent (a
// ControlTransferMsg with kLane, TreasuryEventKind::Request, kSynthetic: a replay makes it again)
// and hands it to the transfer worker (core/transfer_worker.hpp) on a ring. The worker carries it
// out with the venue's blocking requests and reports back on the treasury ring, as journaled
// inputs: Sent (the venue took it), Failed (refused, nothing moved; the hold goes back), Sent with
// TransferState::Unknown (no usable answer: the hold stays), Done (from the venue's history).
//
// Confirmation. A venue announces an internal transfer on both accounts' user streams (Binance
// balanceUpdate, AccountFlowMsg). An outflow of the transfer's asset and amount on the payer moves
// the hold out of the payer's estimate; an inflow on the receiver adds it there; with both the
// transfer is done (journaled as a synthetic Done). The flows carry no client id, so a pool has at
// most one transfer of an asset in flight. The venue's history, by client id, decides a transfer
// whose flows do not come (an answer lost, a stream down, a restart).
//
// The sweep. Every 250 ms the engine looks at each spot instrument of a sweeping pool. Free base on
// an account other than the home (a piece), unchanged for sweep_delay, goes home in one transfer;
// an amount below the tradable minimum (dust) waits for sweep_dust, or goes with the delay when the
// home holds a tradable amount it can be sold with. What open orders lock stays. The plan
// (plan_sweeps) takes the largest pieces first within the budget, one transfer per asset.
//
// The budget: transfers in any minute, hour and day (per_minute, per_hour, per_day), and at most
// max_in_flight unresolved. The breaker: five failures or three unknown answers within ten minutes,
// or one rate-limit answer (HTTP 429, Binance -1003), pause the sweep for breaker_s; it resumes by
// itself once, and after a second trip stays off until the operator turns it on. Three refusals of
// one asset in a row for want of free balance (Binance -9000), with the engine's own hold in place,
// mark the asset as one the venue does not transfer: the session leaves its pieces to trade off.
//
// While a pool sweeps (Engine::sweeping), its reducing spot orders go to the home: a piece is swept
// instead of sold. While it does not (sweep off, paused, dry run, the day's budget used, the asset
// refused), routing is home placement's: pieces first.
//
// Everything here runs on the engine thread and allocates only in the constructor.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transfer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm {

// [venues.<primary>.pool] sweep.
enum class SweepMode : std::uint8_t {
  Off = 0,
  On = 1,
  Auto = 2,  // on when the primary's connector can carry transfers out (a transfer key); else off
};
[[nodiscard]] constexpr std::string_view to_string(SweepMode m) noexcept {
  switch (m) {
    case SweepMode::Off:
      return "false";
    case SweepMode::On:
      return "true";
    case SweepMode::Auto:
      return "auto";
  }
  return "?";
}

// One pool's lane: [venues.<primary>.transfers] and the sweep keys of [venues.<primary>.pool].
struct LaneConfig {
  SweepMode sweep = SweepMode::Off;
  Duration sweep_delay = milliseconds(3000);  // a piece unchanged this long is swept
  Duration sweep_dust = seconds(3600);        // ... dust this long
  Duration sweep_retry = seconds(30);         // a payer and asset after a refusal
  std::uint32_t max_in_flight = 8;            // unresolved transfers at once (at most kSlots)
  std::uint32_t per_minute = 20;              // transfers created in any minute
  std::uint32_t per_hour = 300;
  std::uint32_t per_day = 3000;
  Duration breaker = seconds(900);  // a trip pauses the sweep this long
  // An unresolved transfer this old is late (logged, counted; the hold stays); the worker gives up
  // on one the venue has no record of after it.
  Duration timeout = seconds(60);
  Duration status_interval = seconds(10);  // the worker asks for a transfer not confirmed by then
  bool dry_run = false;                    // journal the intents (Planned), hold and send nothing
  std::string state_file;                  // the worker's ledger; empty: kept in memory

  [[nodiscard]] bool on() const noexcept { return sweep != SweepMode::Off; }
};

// ControlCommand::PoolSweep (fastmm-ctl pool sweep): hdr.venue the pool's primary, arg one of
// these; kSweepNow names hdr.instrument, whose pieces go home at the next look whatever their age.
inline constexpr std::uint64_t kSweepOff = 0;
inline constexpr std::uint64_t kSweepOn = 1;
inline constexpr std::uint64_t kSweepPause = 2;  // for breaker_s, as a trip
inline constexpr std::uint64_t kSweepNow = 3;

// One message from the engine to the transfer worker (core/transfer_worker.hpp).
struct LaneIntent {
  enum class Kind : std::uint8_t {
    Send = 0,      // carry it out
    Resolved = 1,  // the engine saw it done: drop it from the ledger
  };
  Kind kind = Kind::Send;
  VenueId primary;
  VenueId from;
  VenueId to;
  FixedString<8> asset;
  std::int64_t amount = 0;  // raw
  std::uint64_t seq = 0;
  std::int64_t created_ns = 0;  // engine time
};

// A lane transfer's client id: "fs" + the primary's venue id and the sequence number, both base 36
// (at most 17 characters).
inline void lane_client_id(FixedString<31>& out, VenueId primary, std::uint64_t seq) noexcept {
  char buf[32] = {'f', 's'};
  std::size_t n = 2;
  const auto put = [&](std::uint64_t v) {
    char tmp[16];
    std::size_t k = 0;
    do {
      const auto d = static_cast<unsigned>(v % 36);
      tmp[k++] = d < 10 ? static_cast<char>('0' + d) : static_cast<char>('a' + (d - 10));
      v /= 36;
    } while (v != 0 && k < sizeof tmp);
    while (k > 0) buf[n++] = tmp[--k];
  };
  put(primary.value);
  put(seq);
  out.assign(std::string_view(buf, n));
}

// A piece the sweep may move (plan_sweeps).
struct SweepCandidate {
  InstrumentId instrument;
  VenueId from;
  VenueId to;
  FixedString<8> asset;
  std::int64_t amount = 0;  // raw, the payer's free estimate
  double value = 0.0;       // its notional at the mid, quote currency
  bool dust = false;        // below the tradable minimum
};

// The candidates the lane takes now, in place: the first n of `c` on return, in the order to send.
// Tradable pieces before dust, larger values first (ties: instrument, then payer, so the plan is
// deterministic), one per asset, none of an asset in `busy`, at most `room`.
template <class Busy>
[[nodiscard]] std::size_t plan_sweeps(std::span<SweepCandidate> c,
                                      std::size_t room,
                                      Busy busy) noexcept {
  std::sort(c.begin(), c.end(), [](const SweepCandidate& a, const SweepCandidate& b) {
    if (a.dust != b.dust) return !a.dust;
    if (a.value != b.value) return a.value > b.value;
    if (a.instrument != b.instrument) return a.instrument.value < b.instrument.value;
    return a.from.value < b.from.value;
  });
  std::size_t n = 0;
  for (std::size_t i = 0; i < c.size() && n < room; ++i) {
    if (busy(c[i].asset.view())) continue;
    bool dup = false;
    for (std::size_t k = 0; k < n; ++k)
      dup = dup || same_currency(c[k].asset.view(), c[i].asset.view());
    if (dup) continue;
    if (i != n) std::swap(c[i], c[n]);
    ++n;
  }
  return n;
}

// Counts events over sliding windows: the last minute, hour and day. A ring of event times sized
// for the day's limit.
class EventWindows {
 public:
  explicit EventWindows(std::size_t capacity)
      : cap_(std::max<std::size_t>(capacity, 1) + 1),
        times_(std::make_unique<std::int64_t[]>(cap_)) {}

  void add(std::int64_t now_ns) noexcept {
    roll(now_ns);
    if (size() + 1 >= cap_) advance_day();  // full: the oldest goes
    times_[head_] = now_ns;
    head_ = next(head_);
  }
  // Events within the last minute, hour and day of `now_ns`.
  [[nodiscard]] std::uint32_t minute(std::int64_t now_ns) noexcept {
    roll(now_ns);
    return count(minute_);
  }
  [[nodiscard]] std::uint32_t hour(std::int64_t now_ns) noexcept {
    roll(now_ns);
    return count(hour_);
  }
  [[nodiscard]] std::uint32_t day(std::int64_t now_ns) noexcept {
    roll(now_ns);
    return count(day_);
  }

 private:
  static constexpr std::int64_t kMinute = 60'000'000'000;
  static constexpr std::int64_t kHour = 60 * kMinute;
  static constexpr std::int64_t kDay = 24 * kHour;

  [[nodiscard]] std::size_t next(std::size_t i) const noexcept { return (i + 1) % cap_; }
  [[nodiscard]] std::size_t size() const noexcept { return (head_ + cap_ - day_) % cap_; }
  [[nodiscard]] std::uint32_t count(std::size_t from) const noexcept {
    return static_cast<std::uint32_t>((head_ + cap_ - from) % cap_);
  }
  // The day's oldest event goes; the hour's and the minute's tails never fall behind it.
  void advance_day() noexcept {
    if (hour_ == day_) hour_ = next(hour_);
    if (minute_ == day_) minute_ = next(minute_);
    day_ = next(day_);
  }
  void roll(std::int64_t now_ns) noexcept {
    while (day_ != head_ && times_[day_] <= now_ns - kDay) advance_day();
    while (hour_ != head_ && times_[hour_] <= now_ns - kHour) hour_ = next(hour_);
    while (minute_ != head_ && times_[minute_] <= now_ns - kMinute) minute_ = next(minute_);
  }

  std::size_t cap_;
  std::unique_ptr<std::int64_t[]> times_;
  std::size_t head_ = 0;  // the next free slot
  std::size_t day_ = 0;   // the oldest event within the day
  std::size_t hour_ = 0;  // ... the hour
  std::size_t minute_ = 0;
};

enum class BreakerState : std::uint8_t {
  Closed = 0,  // sweeping
  Paused = 1,  // a trip or the operator: until paused_until
  Off = 2,     // a second trip or the operator: until the operator turns it on
};
[[nodiscard]] constexpr std::string_view to_string(BreakerState s) noexcept {
  switch (s) {
    case BreakerState::Closed:
      return "closed";
    case BreakerState::Paused:
      return "paused";
    case BreakerState::Off:
      return "off";
  }
  return "?";
}

struct LaneStats {
  std::uint64_t planned = 0;  // dry run: intents journaled
  std::uint64_t requested = 0;
  std::uint64_t sent = 0;
  std::uint64_t done = 0;
  std::uint64_t done_by_flows = 0;  // ... confirmed by both accounts' flows
  std::uint64_t failed = 0;
  std::uint64_t unknown = 0;
  std::uint64_t late = 0;   // unresolved past timeout
  std::uint64_t trips = 0;  // breaker trips
  std::uint64_t blocked_assets = 0;
  std::uint64_t worker_full = 0;  // intents or notices the worker's ring had no room for
  std::uint32_t in_flight = 0;
  std::int64_t moved_value = 0;  // raw Notional: done transfers at the mid when they were made
};

// The pools' lanes as the status file and fastmm-top show them, summed over the pools
// (Engine::publish_live). The breaker is the worst of the pools'.
struct LaneStatus {
  std::uint32_t pools = 0;
  std::uint32_t in_flight = 0;
  std::uint32_t used_hour = 0;  // transfers created in the last hour, and day
  std::uint32_t used_day = 0;
  std::uint64_t planned = 0;
  std::uint64_t requested = 0;
  std::uint64_t sent = 0;
  std::uint64_t done = 0;
  std::uint64_t done_by_flows = 0;
  std::uint64_t failed = 0;
  std::uint64_t unknown = 0;
  std::uint64_t late = 0;
  std::uint64_t trips = 0;
  std::int64_t held_value_raw = 0;   // in flight, at the mid when made (raw quote)
  std::int64_t moved_value_raw = 0;  // done
  std::uint8_t breaker = 0;          // BreakerState
  std::uint8_t sweeping = 0;         // pools sweeping now
  std::uint8_t dry_run = 0;          // pools in a dry run
  std::uint8_t blocked = 0;          // assets the venue does not transfer
  std::uint32_t pad_ = 0;
};
static_assert(sizeof(LaneStatus) == 112);

class TransferLane {
 public:
  static constexpr std::size_t kSlots = 16;
  static constexpr std::size_t kMaxBlocked = 32;
  static constexpr std::int64_t kBreakerWindowNs = 600'000'000'000;  // 10 minutes
  static constexpr std::uint32_t kTripFailures = 5;
  static constexpr std::uint32_t kTripUnknowns = 3;
  static constexpr std::uint32_t kAssetStrikes = 3;  // refusals for want of balance in a row

  enum class Phase : std::uint8_t {
    Requested = 0,  // held and handed to the worker; no answer yet
    Sent = 1,       // the venue took it
    Unknown = 2,    // no usable answer: the venue may have it
  };
  struct Slot {
    bool active = false;
    Phase phase = Phase::Requested;
    bool out_seen = false;  // the payer's outflow arrived
    bool in_seen = false;   // the receiver's inflow arrived
    bool late = false;
    bool held = false;  // the payer's hold is in place
    std::uint64_t seq = 0;
    InstrumentId instrument;
    VenueId from;
    VenueId to;
    FixedString<8> asset;
    std::int64_t amount = 0;  // raw
    std::int64_t hold = 0;    // raw, what the payer's estimate holds for it
    std::int64_t value = 0;   // raw Notional at the mid when made
    Timestamp created;
    Timestamp sent;
  };

  TransferLane(VenueId primary, const LaneConfig& cfg)
      : primary_(primary), cfg_(cfg), windows_(cfg.per_day) {}

  [[nodiscard]] VenueId primary() const noexcept { return primary_; }
  [[nodiscard]] const LaneConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const LaneStats& stats() const noexcept { return stats_; }
  [[nodiscard]] LaneStats& stats() noexcept { return stats_; }

  // ---- budget -------------------------------------------------------------------------------

  // Transfers the budget admits now: what the minute, hour and day leave, and the free slots.
  [[nodiscard]] std::size_t room(Timestamp now) noexcept {
    const std::int64_t t = now.ns;
    const auto left = [](std::uint32_t limit, std::uint32_t used) -> std::size_t {
      return limit > used ? limit - used : 0;
    };
    std::size_t r = std::min({left(cfg_.per_minute, windows_.minute(t)),
                              left(cfg_.per_hour, windows_.hour(t)),
                              left(cfg_.per_day, windows_.day(t))});
    const std::size_t slots = std::min<std::size_t>(cfg_.max_in_flight, kSlots);
    const std::size_t used = in_flight();
    return std::min(r, slots > used ? slots - used : 0);
  }
  // The day's budget is used up.
  [[nodiscard]] bool day_spent(Timestamp now) noexcept {
    return windows_.day(now.ns) >= cfg_.per_day;
  }
  void count(Timestamp now) noexcept { windows_.add(now.ns); }
  [[nodiscard]] std::uint32_t used_minute(Timestamp now) noexcept {
    return windows_.minute(now.ns);
  }
  [[nodiscard]] std::uint32_t used_hour(Timestamp now) noexcept { return windows_.hour(now.ns); }
  [[nodiscard]] std::uint32_t used_day(Timestamp now) noexcept { return windows_.day(now.ns); }

  // ---- breaker ------------------------------------------------------------------------------

  // The breaker now; a pause that has run out closes it.
  [[nodiscard]] BreakerState breaker(Timestamp now) noexcept {
    if (breaker_ == BreakerState::Paused && now >= paused_until_) breaker_ = BreakerState::Closed;
    return breaker_;
  }
  [[nodiscard]] Timestamp paused_until() const noexcept { return paused_until_; }
  // A refusal; `rate_limited`: the venue said to slow down. True: the breaker tripped.
  bool note_failure(Timestamp now, bool rate_limited) noexcept {
    push(failures_, now);
    if (rate_limited || within(failures_, now) >= kTripFailures) return trip(now);
    return false;
  }
  bool note_unknown(Timestamp now) noexcept {
    push(unknowns_, now);
    if (within(unknowns_, now) >= kTripUnknowns) return trip(now);
    return false;
  }
  // The operator (ControlCommand::PoolSweep).
  void set_on() noexcept {
    breaker_ = BreakerState::Closed;
    trips_since_on_ = 0;
    failures_.fill(0);
    unknowns_.fill(0);
  }
  void set_off() noexcept { breaker_ = BreakerState::Off; }
  void pause(Timestamp now) noexcept {
    breaker_ = BreakerState::Paused;
    paused_until_ = now + cfg_.breaker;
  }

  // ---- assets the venue does not transfer ---------------------------------------------------

  [[nodiscard]] bool blocked(std::string_view asset) const noexcept {
    for (std::size_t i = 0; i < n_blocked_; ++i) {
      if (same_currency(blocked_[i].view(), asset)) return true;
    }
    return false;
  }
  // A refusal of `asset`: `short_balance` (Binance -9000) counts towards blocking it, anything else
  // starts the count again. True: the asset is now blocked.
  bool note_asset_refusal(std::string_view asset, bool short_balance) noexcept {
    Strike& s = strike_of(asset);
    s.count = short_balance ? s.count + 1 : 0;
    if (s.count < kAssetStrikes || blocked(asset) || n_blocked_ == kMaxBlocked) return false;
    blocked_[n_blocked_++].assign(asset);
    ++stats_.blocked_assets;
    return true;
  }
  void note_asset_done(std::string_view asset) noexcept { strike_of(asset).count = 0; }

  // ---- transfers in flight ------------------------------------------------------------------

  [[nodiscard]] std::size_t in_flight() const noexcept { return stats_.in_flight; }
  [[nodiscard]] Slot* find(std::uint64_t seq) noexcept {
    for (Slot& s : slots_) {
      if (s.active && s.seq == seq) return &s;
    }
    return nullptr;
  }
  [[nodiscard]] const Slot* by_asset(std::string_view asset) const noexcept {
    for (const Slot& s : slots_) {
      if (s.active && same_currency(s.asset.view(), asset)) return &s;
    }
    return nullptr;
  }
  [[nodiscard]] Slot* by_asset(std::string_view asset) noexcept {
    for (Slot& s : slots_) {
      if (s.active && same_currency(s.asset.view(), asset)) return &s;
    }
    return nullptr;
  }
  [[nodiscard]] bool busy(std::string_view asset) const noexcept {
    return by_asset(asset) != nullptr;
  }
  // A free slot, made active; null when every slot is taken.
  [[nodiscard]] Slot* open(std::uint64_t seq) noexcept {
    for (Slot& s : slots_) {
      if (s.active) continue;
      s = Slot{};
      s.active = true;
      s.seq = seq;
      ++stats_.in_flight;
      return &s;
    }
    return nullptr;
  }
  void close(Slot& s) noexcept {
    if (!s.active) return;
    s.active = false;
    --stats_.in_flight;
  }
  [[nodiscard]] std::span<Slot> slots() noexcept { return slots_; }
  [[nodiscard]] std::span<const Slot> slots() const noexcept { return slots_; }

  // The next sequence number: the session's start second above 20 bits of a counter, so a transfer
  // keeps its client id across restarts and none is reused.
  [[nodiscard]] std::uint64_t next_seq(Timestamp start) noexcept {
    const auto base =
        static_cast<std::uint64_t>(std::max<std::int64_t>(start.ns, 0) / 1'000'000'000);
    return (base << 20) | (++counter_ & 0xFFFFFU);
  }

 private:
  struct Strike {
    FixedString<8> asset;
    std::uint32_t count = 0;
  };
  Strike& strike_of(std::string_view asset) noexcept {
    for (Strike& s : strikes_) {
      if (!s.asset.empty() && same_currency(s.asset.view(), asset)) return s;
    }
    Strike& s = strikes_[next_strike_++ % strikes_.size()];
    s.asset.assign(asset);
    s.count = 0;
    return s;
  }
  static void push(std::array<std::int64_t, 8>& ring, Timestamp now) noexcept {
    std::rotate(ring.begin(), ring.begin() + 1, ring.end());
    ring.back() = now.ns;
  }
  [[nodiscard]] static std::uint32_t within(const std::array<std::int64_t, 8>& ring,
                                            Timestamp now) noexcept {
    std::uint32_t n = 0;
    for (const std::int64_t t : ring) n += t != 0 && t > now.ns - kBreakerWindowNs ? 1U : 0U;
    return n;
  }
  bool trip(Timestamp now) noexcept {
    if (breaker_ != BreakerState::Closed) return false;
    ++stats_.trips;
    failures_.fill(0);
    unknowns_.fill(0);
    if (++trips_since_on_ >= 2) {
      breaker_ = BreakerState::Off;
    } else {
      pause(now);
    }
    return true;
  }

  VenueId primary_;
  LaneConfig cfg_;
  EventWindows windows_;
  LaneStats stats_;
  std::array<Slot, kSlots> slots_{};
  std::uint64_t counter_ = 0;
  BreakerState breaker_ = BreakerState::Closed;
  Timestamp paused_until_{};
  std::uint32_t trips_since_on_ = 0;
  std::array<std::int64_t, 8> failures_{};
  std::array<std::int64_t, 8> unknowns_{};
  std::array<FixedString<8>, kMaxBlocked> blocked_{};
  std::size_t n_blocked_ = 0;
  std::array<Strike, 16> strikes_{};
  std::size_t next_strike_ = 0;
};

}  // namespace fastmm
