#pragma once
// FillCluster: the fill-cluster guard ([risk] fill_cluster_names). A taker that sweeps one side of
// many books at once fills our quotes on several instruments within a few hundred milliseconds,
// and the instruments it reaches last are the ones it has priced best. When maker fills of our
// quotes on one side cover `names` distinct instruments within `window`, that side is held on every
// instrument for `hold`: the engine cancels the quotes resting there and places none until the hold
// ends (QuoteManager::hold_side), then re-applies the strategy's last quotes. Each further fill
// that still completes a cluster extends the hold.
//
// Engine thread only. The inputs are the fills the engine consumes and its clock, both in the
// journal, so a replay holds the same sides at the same times. No allocation: each side keeps the
// last kMaxNames instruments it filled on.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm {

struct FillClusterConfig {
  static constexpr std::uint32_t kMaxNames = 16;
  std::uint32_t names = 0;  // distinct instruments that make a cluster; 0 = off
  Duration window = milliseconds(500);
  Duration hold = milliseconds(500);
  [[nodiscard]] constexpr bool enabled() const noexcept { return names > 0; }
};

// What StrategyContext::fill_cluster returns, per side (index 0 bids, 1 asks).
struct FillClusterView {
  std::array<bool, 2> held{};             // the side is held now
  std::array<Timestamp, 2> held_until{};  // ... until then (engine time)
  std::array<std::uint64_t, 2> engagements{};
  bool enabled = false;
};

class FillCluster {
 public:
  static constexpr std::size_t kSlots = FillClusterConfig::kMaxNames;

  void configure(const FillClusterConfig& c) noexcept { cfg_ = c; }
  [[nodiscard]] const FillClusterConfig& config() const noexcept { return cfg_; }

  // A maker fill of a quote on `id`, side `s`, consumed at `now`. True when it engages the side
  // (the side was not held); a fill that completes a cluster while the side is held extends it.
  FASTMM_FORCE_INLINE bool on_fill(InstrumentId id, Side s, Timestamp now) noexcept {
    Track& t = sides_[index(s)];
    // The instrument's slot, else the oldest one.
    std::size_t at = 0;
    for (std::size_t i = 0; i < kSlots; ++i) {
      if (t.ts[i].valid() && t.id[i] == id) {
        at = i;
        break;
      }
      if (!t.ts[i].valid() || t.ts[i] < t.ts[at]) at = i;
    }
    t.id[at] = id;
    t.ts[at] = now;
    std::uint32_t distinct = 0;
    const Timestamp since = now - cfg_.window;
    for (std::size_t i = 0; i < kSlots; ++i)
      distinct += t.ts[i].valid() && t.ts[i] >= since ? 1U : 0U;
    if (distinct < cfg_.names) return false;
    const bool engaged = !t.active;
    t.active = true;
    t.until = now + cfg_.hold;
    if (engaged) ++t.engagements;
    return engaged;
  }

  [[nodiscard]] FASTMM_FORCE_INLINE bool held(Side s) const noexcept {
    return sides_[index(s)].active;
  }
  // A held side whose hold has run out by `now`: lift() it.
  [[nodiscard]] FASTMM_FORCE_INLINE bool lift_due(Timestamp now) const noexcept {
    return (sides_[0].active && now >= sides_[0].until) ||
           (sides_[1].active && now >= sides_[1].until);
  }
  // Ends the hold of `s` if it has run out by `now`; true when it did.
  bool lift(Side s, Timestamp now) noexcept {
    Track& t = sides_[index(s)];
    if (!t.active || now < t.until) return false;
    t.active = false;
    return true;
  }

  [[nodiscard]] FillClusterView view() const noexcept {
    FillClusterView v;
    v.enabled = cfg_.enabled();
    for (std::size_t i = 0; i < 2; ++i) {
      v.held[i] = sides_[i].active;
      v.held_until[i] = sides_[i].active ? sides_[i].until : Timestamp{};
      v.engagements[i] = sides_[i].engagements;
    }
    return v;
  }

 private:
  struct Track {
    std::array<InstrumentId, kSlots> id{};
    std::array<Timestamp, kSlots> ts{};  // engine time of the instrument's last fill; invalid: free
    Timestamp until{};
    std::uint64_t engagements = 0;
    bool active = false;
  };
  [[nodiscard]] static constexpr std::size_t index(Side s) noexcept {
    return s == Side::Buy ? 0U : 1U;
  }

  FillClusterConfig cfg_{};
  std::array<Track, 2> sides_{};
};

}  // namespace fastmm
