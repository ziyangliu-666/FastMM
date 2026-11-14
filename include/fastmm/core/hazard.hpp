#pragma once
// Hazard: the group hazard score ([[risk.hazard]]). A group is a set of instruments whose quotes
// the same taker reaches one after another: instruments priced off one factor. Each side of a
// group keeps one score, a sum of evidence that decays with `half_life`:
//   * a maker fill of one of our quotes on a member adds its notional times the member's weight;
//   * a public print that reaches a quote of ours resting on a member adds the same when it arrives
//     ahead of the fill's own report (`prints`); the two are copies of one match, and whichever
//     comes second adds only the quantity the first did not (matched by quantity per member and
//     side, within kCopyWindow);
//   * the strategy adds what only it can see (StrategyContext::hazard_add), a move of the
//     instrument its quotes are priced from for one.
// Evidence that takes the score to `threshold` engages the side: the engine cancels the group's
// quotes on that side and places none until `hold` after the last evidence that found the score at
// the threshold, then puts the strategy's last quotes back. A group in log mode keeps the same
// score and the same holds and sends nothing: the log says what would have been pulled and which
// fills arrived meanwhile, each with the venue's time of its match after the pull.
//
// Engine thread only. The inputs are the fills and trades the engine consumes, the strategy's calls
// and the engine clock, all of them in the journal or derived from it, so a replay engages the same
// sides at the same events. The tables are sized at configure(); nothing allocates after it.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace fastmm {

struct HazardGroupConfig {
  std::string name;
  std::vector<InstrumentId> members;
  std::vector<double> weights;  // one per member; empty: every member weighs 1
  double threshold = 0.0;       // score that engages a side, quote notional times weight
  Duration half_life = milliseconds(20);
  Duration hold = milliseconds(500);
  std::array<bool, 2> sides{true, true};  // bids, asks
  bool prints = true;                     // public prints that reach our quotes count
  bool log_only = false;                  // keep the score and the holds, pull nothing
};

struct HazardConfig {
  static constexpr std::size_t kMaxGroups = 16;
  std::vector<HazardGroupConfig> groups;
  [[nodiscard]] bool enabled() const noexcept { return !groups.empty(); }
};

// What StrategyContext::hazard returns for one group, per side (index 0 bids, 1 asks).
struct HazardView {
  std::array<double, 2> score{};          // decayed to the time of the call
  std::array<bool, 2> held{};             // the side is held now (log mode: would be)
  std::array<Timestamp, 2> held_since{};  // ... since then (engine time)
  // ... until then, unless evidence extends it
  std::array<Timestamp, 2> held_until{};
  std::array<std::uint64_t, 2> engagements{};
  std::array<std::uint64_t, 2> held_fills{};  // quote fills that arrived on a held side
  std::array<double, 2> held_notional{};      // ... and their notional
  double threshold = 0.0;
  bool log_only = false;
  bool enabled = false;  // the group exists
};

class Hazard {
 public:
  static constexpr std::uint32_t kNoGroup = UINT32_MAX;
  // A print and the fill report of the same match arrive within this of each other.
  static constexpr Duration kCopyWindow = seconds(1);

  enum class Outcome : std::uint8_t { None, Engaged, Extended };

  // `instruments`: ids run from 0 to instruments - 1. A member outside it is dropped.
  void configure(const HazardConfig& c, std::size_t instruments) {
    groups_.clear();
    group_of_.assign(instruments, kNoGroup);
    weight_.assign(instruments, 0.0);
    copy_.assign(instruments * 2, Copy{});
    for (const HazardGroupConfig& g : c.groups) {
      if (g.threshold <= 0.0 || groups_.size() == HazardConfig::kMaxGroups) continue;
      Group out;
      out.name = g.name;
      out.threshold = g.threshold;
      // exp(-decay * dt_ns) halves the score every half_life.
      out.decay =
          g.half_life.ns > 0 ? std::numbers::ln2 / static_cast<double>(g.half_life.ns) : 0.0;
      out.hold = g.hold;
      out.sides = g.sides;
      out.prints = g.prints;
      out.log_only = g.log_only;
      const auto index = static_cast<std::uint32_t>(groups_.size());
      for (std::size_t k = 0; k < g.members.size(); ++k) {
        const std::uint32_t id = g.members[k].value;
        if (id >= instruments || group_of_[id] != kNoGroup) continue;
        group_of_[id] = index;
        weight_[id] = k < g.weights.size() ? g.weights[k] : 1.0;
        out.members.push_back(g.members[k]);
      }
      groups_.push_back(std::move(out));
    }
  }

  [[nodiscard]] bool enabled() const noexcept { return !groups_.empty(); }
  [[nodiscard]] std::uint32_t groups() const noexcept {
    return static_cast<std::uint32_t>(groups_.size());
  }
  [[nodiscard]] FASTMM_FORCE_INLINE std::uint32_t group_of(InstrumentId id) const noexcept {
    return id.value < group_of_.size() ? group_of_[id.value] : kNoGroup;
  }
  [[nodiscard]] const std::string& name(std::uint32_t g) const noexcept { return groups_[g].name; }
  [[nodiscard]] std::span<const InstrumentId> members(std::uint32_t g) const noexcept {
    return groups_[g].members;
  }
  [[nodiscard]] bool log_only(std::uint32_t g) const noexcept { return groups_[g].log_only; }
  [[nodiscard]] Duration hold(std::uint32_t g) const noexcept { return groups_[g].hold; }
  // Whether public prints on `id` are evidence for the quotes resting on `quote_side`.
  [[nodiscard]] FASTMM_FORCE_INLINE bool wants_print(InstrumentId id,
                                                     Side quote_side) const noexcept {
    const std::uint32_t g = group_of(id);
    return g != kNoGroup && groups_[g].prints && groups_[g].sides[index(quote_side)];
  }

  // Evidence for side `s` of group `g`: `amount` is added to the score (a negative amount takes
  // evidence back; the score stays at or above zero). Engaged: the side was not held and is now.
  // Extended: it was held and the hold now ends `hold` from `now`.
  Outcome add(std::uint32_t g, Side s, double amount, Timestamp now) noexcept {
    Group& grp = groups_[g];
    const std::size_t i = index(s);
    if (!grp.sides[i]) return Outcome::None;
    Track& t = grp.track[i];
    t.score = decayed(grp, t, now) + amount;
    t.at = now;
    if (t.score < 0.0) t.score = 0.0;
    if (amount <= 0.0 || t.score < grp.threshold) return Outcome::None;
    t.until = now + grp.hold;
    if (t.until < next_lift_) next_lift_ = t.until;
    if (t.held) return Outcome::Extended;
    t.held = true;
    t.since = now;
    ++t.engagements;
    return Outcome::Engaged;
  }

  // A maker fill of `qty` of a quote on member `id`: the quantity that is new evidence, the rest
  // having been counted when its print arrived. `notional` is the fill's, for the held-fill count.
  [[nodiscard]] std::int64_t fill_qty(
      InstrumentId id, Side s, std::int64_t qty, double notional, Timestamp now) noexcept {
    Track& t = groups_[group_of_[id.value]].track[index(s)];
    if (t.held) {
      ++t.held_fills;
      t.held_notional += notional;
    }
    Copy& c = copy(id, s, now);
    const std::int64_t counted = std::clamp<std::int64_t>(c.ahead, 0, qty);
    c.ahead -= qty;
    return qty - counted;
  }
  // A public print of `qty` at a price where `resting` of our quotes on `quote_side` of member `id`
  // still rests at or better: the quantity that is new evidence. Fills already reported are matched
  // first (the print is their second copy); of the rest, what our quotes could have traded counts.
  [[nodiscard]] std::int64_t print_qty(InstrumentId id,
                                       Side quote_side,
                                       std::int64_t qty,
                                       std::int64_t resting,
                                       Timestamp now) noexcept {
    Copy& c = copy(id, quote_side, now);
    const std::int64_t reported = std::clamp<std::int64_t>(-c.ahead, 0, qty);
    const std::int64_t rest = qty - reported;
    const std::int64_t fresh = std::min(rest, resting);
    c.ahead += reported + fresh;
    return fresh;
  }
  [[nodiscard]] double weight(InstrumentId id) const noexcept { return weight_[id.value]; }

  [[nodiscard]] FASTMM_FORCE_INLINE bool held(std::uint32_t g, Side s) const noexcept {
    return groups_[g].track[index(s)].held;
  }
  // Some hold may have run out by `now`: call lift().
  [[nodiscard]] FASTMM_FORCE_INLINE bool lift_due(Timestamp now) const noexcept {
    return now >= next_lift_;
  }
  // Ends each hold that has run out by `now` and calls f(group, side) for it.
  template <class F>
  void lift(Timestamp now, F&& f) noexcept {
    Timestamp next = Timestamp::max();
    for (std::uint32_t g = 0; g < groups_.size(); ++g) {
      for (const Side s : {Side::Buy, Side::Sell}) {
        Track& t = groups_[g].track[index(s)];
        if (!t.held) continue;
        if (now < t.until) {
          if (t.until < next) next = t.until;
          continue;
        }
        t.held = false;
        f(g, s);
      }
    }
    next_lift_ = next;
  }

  [[nodiscard]] HazardView view(std::uint32_t g, Timestamp now) const noexcept {
    HazardView v;
    if (g >= groups_.size()) return v;
    const Group& grp = groups_[g];
    v.enabled = true;
    v.threshold = grp.threshold;
    v.log_only = grp.log_only;
    for (std::size_t i = 0; i < 2; ++i) {
      const Track& t = grp.track[i];
      v.score[i] = decayed(grp, t, now);
      v.held[i] = t.held;
      v.held_since[i] = t.held ? t.since : Timestamp{};
      v.held_until[i] = t.held ? t.until : Timestamp{};
      v.engagements[i] = t.engagements;
      v.held_fills[i] = t.held_fills;
      v.held_notional[i] = t.held_notional;
    }
    return v;
  }

 private:
  struct Track {
    double score = 0.0;
    Timestamp at{};  // when `score` was current
    Timestamp since{};
    Timestamp until{};
    std::uint64_t engagements = 0;
    std::uint64_t held_fills = 0;
    double held_notional = 0.0;
    bool held = false;
  };
  struct Group {
    std::string name;
    std::vector<InstrumentId> members;
    double threshold = 0.0;
    double decay = 0.0;
    Duration hold{};
    std::array<bool, 2> sides{};
    bool prints = false;
    bool log_only = false;
    std::array<Track, 2> track{};
  };
  // Per member and side: the quantity prints have counted that no fill report has matched yet
  // (positive), or fills that no print has matched yet (negative).
  struct Copy {
    std::int64_t ahead = 0;
    Timestamp at{};
  };

  [[nodiscard]] static constexpr std::size_t index(Side s) noexcept {
    return s == Side::Buy ? 0U : 1U;
  }
  [[nodiscard]] static double decayed(const Group& g, const Track& t, Timestamp now) noexcept {
    if (t.score <= 0.0 || now <= t.at) return t.score;
    if (g.decay <= 0.0) return 0.0;
    return t.score * std::exp(-g.decay * static_cast<double>((now - t.at).ns));
  }
  [[nodiscard]] Copy& copy(InstrumentId id, Side s, Timestamp now) noexcept {
    Copy& c = copy_[static_cast<std::size_t>(id.value) * 2 + index(s)];
    if (now - c.at > kCopyWindow) c.ahead = 0;
    c.at = now;
    return c;
  }

  std::vector<Group> groups_;
  std::vector<std::uint32_t> group_of_;  // by instrument id
  std::vector<double> weight_;           // by instrument id
  std::vector<Copy> copy_;               // by instrument id and side
  Timestamp next_lift_ = Timestamp::max();
};

}  // namespace fastmm
