#pragma once
// Home placement for account pools ([venues.<primary>.pool], core/account_pool.hpp).
//
// Each instrument of a pool has a home: the account its position lives on. With placement = "home"
// the engine sends every order that adds to the position to the home, and every order that reduces
// it to the accounts that hold it, the pieces away from home first. A position therefore cannot
// spread over the accounts, and a piece that exists (a re-home, a restart, the routing before)
// drains by trading. With placement = "balance" (the default) the routing is the balance and
// order-window choice it always was, and the homes are only kept and journaled: the observe mode.
//
// A home changes only through a PoolHome control message (ControlCommand::PoolHome), journaled:
// from the planner, the operator (fastmm-ctl pool home), the strategy (ctx.set_home) and the
// engine itself (a pin at the start, the reconciliation once every account of the pool has
// reported its balances, a first order with no home yet, a spill).
//
//   PoolPlacement  one pool's settings
//   PoolHomesPlan  every pool's settings and the pinned homes, by instrument
//   PoolLoad       what the planner reads of a running engine: its orders by instrument, the
//                  homes, the accounts' windows and which accounts are usable (any thread)
//   PoolFunding    what a pool treasury reads of a running engine: the funding needs by home,
//                  the payout it holds (any thread; core/treasury.hpp)
//   HomePlanner    the planner: pure and deterministic, off the hot path
//
// The planner. Every 10 s it takes the orders each instrument sent (new orders and replaces) as
// one window, and keeps plan_lookback of them. Every plan_interval it starts from the current
// homes, gives a home to each instrument with orders and none, and then moves at most
// plan_max_moves instruments, each time the one off the worst account that lowers the cost most:
//   cost = max over accounts of (its peak in the pool's busiest 1% of windows / its 10 s limit)
//        + max over accounts of (its share of the orders / an even share)
// where an account's load is the orders of the instruments it is home to. Pinned instruments
// ([venues.<primary>.pool] homes, the operator's moves) never move; an account that is not usable
// takes none. First homes are given on top of plan_max_moves. A move
// costs nothing structural: the old home stops adding, and its part drains.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm {

enum class Placement : std::uint8_t {
  Balance = 0,  // the account whose balance covers the order with the most window left
  Home = 1,     // adding orders on the home, reducing orders on the holders
};
[[nodiscard]] constexpr std::string_view to_string(Placement p) noexcept {
  return p == Placement::Home ? "home" : "balance";
}

// What an adding order does when its home cannot take it.
enum class HomeSpill : std::uint8_t {
  Never = 0,  // a home that is not usable (order link down, kill switch) refuses HomeUnavailable
  Flat = 1,   // ... and a flat instrument is re-homed to the usable account with the most window
  // A spot order goes to the account the balance routing picks, the home staying the home, when
  // the home is not usable, its 10 s window is past spill_at of what its connector admits (or
  // another of its windows is full), it does not cover the order, or [capacity] prices its slot
  // above the order; otherwise to the home. A derivative's home spills as with Flat.
  Busy = 2,
};
[[nodiscard]] constexpr std::string_view to_string(HomeSpill s) noexcept {
  switch (s) {
    case HomeSpill::Never:
      return "never";
    case HomeSpill::Flat:
      return "flat";
    case HomeSpill::Busy:
      return "busy";
  }
  return "?";
}

// Who changed a home (ControlMsg::arg of ControlCommand::PoolHome, kPoolHomeSourceShift).
enum class HomeSource : std::uint8_t {
  Operator = 0,   // fastmm-ctl pool home
  Planner = 1,    // HomePlanner
  Strategy = 2,   // ctx.set_home
  Reconcile = 3,  // once every account of the pool has reported its balances
  Pin = 4,        // [venues.<primary>.pool] homes, at the start
  First = 5,      // the first order of an instrument with no home
  Spill = 6,      // spill = "flat": the home was not usable
};
[[nodiscard]] constexpr std::string_view to_string(HomeSource s) noexcept {
  switch (s) {
    case HomeSource::Operator:
      return "operator";
    case HomeSource::Planner:
      return "planner";
    case HomeSource::Strategy:
      return "strategy";
    case HomeSource::Reconcile:
      return "reconcile";
    case HomeSource::Pin:
      return "pin";
    case HomeSource::First:
      return "first";
    case HomeSource::Spill:
      return "spill";
  }
  return "?";
}

// ControlCommand::PoolHome: hdr.instrument the instrument, arg the new home's venue id in its low
// byte, the HomeSource above it and the old home (0xFF: none) above that. The engine's own changes
// (strategy, reconcile, pin, first, spill) are journaled with EventHeader::kSynthetic: a replay
// makes them again and does not take them as input.
inline constexpr unsigned kPoolHomeSourceShift = 8;
inline constexpr unsigned kPoolHomeFromShift = 16;
// An operator's move pins the home (the planner and a spill leave it), unless arg has this bit
// (fastmm-ctl pool home --free).
inline constexpr std::uint64_t kPoolHomeFree = std::uint64_t{1} << 24;
[[nodiscard]] constexpr std::uint64_t pool_home_arg(VenueId to,
                                                    HomeSource source,
                                                    VenueId from = {}) noexcept {
  return static_cast<std::uint64_t>(to.value) |
         (static_cast<std::uint64_t>(source) << kPoolHomeSourceShift) |
         (static_cast<std::uint64_t>(from.value) << kPoolHomeFromShift);
}
[[nodiscard]] constexpr VenueId pool_home_to(std::uint64_t arg) noexcept {
  return VenueId{static_cast<std::uint8_t>(arg & 0xFFU)};
}
[[nodiscard]] constexpr HomeSource pool_home_source(std::uint64_t arg) noexcept {
  return static_cast<HomeSource>((arg >> kPoolHomeSourceShift) & 0xFFU);
}
[[nodiscard]] constexpr VenueId pool_home_from(std::uint64_t arg) noexcept {
  return VenueId{static_cast<std::uint8_t>((arg >> kPoolHomeFromShift) & 0xFFU)};
}

// One pool's [venues.<primary>.pool].
struct PoolPlacement {
  bool configured = false;  // the table is present
  Placement placement = Placement::Balance;
  // Accounts that may carry reducing orders of one instrument and side at once: with 1, a piece
  // is worked off before the home's part is offered.
  std::uint8_t reduce_accounts = 1;
  HomeSpill spill = HomeSpill::Never;
  double spill_at = 0.8;                    // spill = "busy": the share of the 10 s window
  Duration plan_interval = seconds(3600);   // zero: no planner
  Duration plan_lookback = seconds(10800);  // 3 h
  std::uint32_t plan_max_moves = 3;

  [[nodiscard]] bool homes() const noexcept { return placement == Placement::Home; }
  [[nodiscard]] bool planner() const noexcept { return configured && plan_interval.ns > 0; }
};

struct PoolHomesPlan {
  std::array<PoolPlacement, PoolPlan::kVenues> pool{};  // by the primary's venue id
  std::array<VenueId, kMaxInstruments> pin{};           // invalid: not pinned

  [[nodiscard]] const PoolPlacement& of(VenueId primary) const noexcept {
    static constexpr PoolPlacement kNone{};
    return primary.value < pool.size() ? pool[primary.value] : kNone;
  }
  // A pool keeps homes: it has a [venues.<primary>.pool] table.
  [[nodiscard]] bool active() const noexcept {
    for (const PoolPlacement& p : pool) {
      if (p.configured) return true;
    }
    return false;
  }
};

// What the planner reads of a running engine (Engine::pool_load; any thread). The instruments of
// one pool in id order.
struct PoolLoad {
  static constexpr std::size_t kMax = kMaxInstruments;
  std::uint32_t count = 0;
  std::array<InstrumentId, kMax> instrument{};
  std::array<std::uint64_t, kMax> orders{};  // new orders and replaces since the start
  std::array<VenueId, kMax> home{};          // invalid: none yet
  std::array<bool, kMax> pinned{};           // a pin or an operator's move: not moved
  PoolMembers members;
  // Each account's 10 s order limit as its connector published it (0: not known), and whether
  // it is usable (order link live, kill switch not engaged), by index in `members`.
  std::array<std::int64_t, PoolMembers::kMax> limit_10s{};
  std::array<bool, PoolMembers::kMax> usable{};
};

// What the engine says of one pool's funding (IEngineRunner::pool_funding; any thread): each
// account's need, the funding needs of the instruments whose home it is, in the treasury's asset;
// and the payout it holds for a transfer (reserve = true).
struct PoolFunding {
  PoolMembers members;
  std::array<Notional, PoolMembers::kMax> need{};  // by index in `members`
  bool declared = false;       // an instrument of the pool has a need above zero
  std::uint64_t hold_seq = 0;  // the transfer the engine holds for (ControlTransferMsg::seq)
  Notional held;               // ... and how much; hold_seq 0: nothing held

  [[nodiscard]] Notional need_of(VenueId v) const noexcept {
    for (std::size_t k = 0; k < members.size(); ++k) {
      if (members[k] == v) return need[k];
    }
    return {};
  }
};

struct HomeMove {
  InstrumentId instrument;
  VenueId from;  // invalid: the instrument had no home
  VenueId to;
  constexpr bool operator==(const HomeMove&) const noexcept = default;
};

struct HomePlannerStats {
  std::uint64_t samples = 0;
  std::uint64_t plans = 0;
  std::uint64_t moves = 0;     // homes changed, first homes not counted
  std::uint64_t assigned = 0;  // first homes given
  double last_cost = 0.0;      // the cost after the last plan
};

class HomePlanner {
 public:
  static constexpr std::int64_t kWindowNs = 10'000'000'000;  // one window: 10 s
  static constexpr std::int64_t kDefaultLimit = 100;         // a 10 s limit not published

  // `pins`: by instrument id, the pinned home (invalid: free to move).
  HomePlanner(VenueId primary, const PoolPlacement& cfg, std::span<const VenueId> pins = {});

  [[nodiscard]] VenueId primary() const noexcept { return primary_; }
  [[nodiscard]] const PoolPlacement& config() const noexcept { return cfg_; }
  [[nodiscard]] const HomePlannerStats& stats() const noexcept { return stats_; }

  // When the next window closes and the next plan is due (clock ns; the first call to step sets
  // them).
  [[nodiscard]] std::int64_t next_sample_ns() const noexcept { return next_sample_ns_; }
  [[nodiscard]] std::int64_t next_plan_ns() const noexcept { return next_plan_ns_; }

  // At clock time `now_ns`: closes the window when it is due (the orders each instrument sent
  // since the last one), and plans when that is due. The moves to make, else empty.
  [[nodiscard]] std::vector<HomeMove> step(std::int64_t now_ns, const PoolLoad& load);

  // The parts of step(), for tests.
  void sample(const PoolLoad& load);
  [[nodiscard]] std::vector<HomeMove> plan(const PoolLoad& load);

  // The windows held, oldest first, for one instrument's slot (tests).
  [[nodiscard]] std::size_t windows() const noexcept { return filled_; }

 private:
  [[nodiscard]] std::size_t slot_of(InstrumentId id) const noexcept;

  VenueId primary_;
  PoolPlacement cfg_;
  std::array<VenueId, kMaxInstruments> pin_{};
  std::size_t capacity_ = 0;  // windows kept
  std::size_t filled_ = 0;
  std::size_t head_ = 0;  // the next window's index in the ring
  // counts_[slot * capacity_ + w]: an instrument's orders in window w; slots by instrument id.
  std::vector<std::uint32_t> counts_;
  std::array<std::uint16_t, kMaxInstruments> slot_{};
  std::vector<InstrumentId> slots_;
  std::array<std::uint64_t, kMaxInstruments> last_{};  // the cumulative orders at the last sample
  std::array<bool, kMaxInstruments> seen_{};
  std::int64_t next_sample_ns_ = 0;
  std::int64_t next_plan_ns_ = 0;
  bool started_ = false;
  HomePlannerStats stats_;
};

}  // namespace fastmm
