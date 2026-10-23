#pragma once
// A/B tests of strategy parameters ([ab], docs/how-to/strategies/ab-test.md).
//
// AbPlan says which arm each instrument is in at any time. It is a function of the [ab] table, the
// instrument table and the time only, so fastmm-live, a backtest and fastmm-data ab (which
// rebuilds it from a journal's configuration) agree on it.
//
//   instrument  each instrument stays in one arm. The instruments, heaviest first, are cut into
//               strata of as many instruments as there are arms, and each stratum gives every arm
//               one instrument, in an order drawn from the seed.
//   time        every instrument is in the same arm. Time is cut into blocks of block_s from the
//               Unix epoch, and each run of as many blocks as there are arms gives every arm one
//               block, in an order drawn from the seed.
//   crossover   both: an instrument's arm is its stratum slot plus the block's draw, modulo the
//               number of arms, so each block splits the instruments and each run of blocks puts
//               every instrument in every arm once.
//   branch      a backtest runs once per arm over the same data and seed, every instrument in that
//               run's arm (set_branch) from the test's start, the start of trading when it names
//               none (set_default_start); fastmm-live refuses it. The runs share the market and the
//               draws until their order flows differ, so each instrument is paired with itself.
//
// AbController keeps a session's instruments in the plan's arms. When an instrument's arm changes
// it publishes the arm's parameters (the configured values with the arm's on top) as one
// ParamUpdate, Origin::Experiment with the arm in ParamUpdateMsg::arm. The engine applies it like
// any update, the journal records it, and the orders sent after it carry the arm (Order::arm).
// Off the engine thread: it allocates.
#include "fastmm/config/config.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

inline constexpr std::size_t kMaxAbArms = 16;

enum class AbAssignment : std::uint8_t { Instrument, Time, Crossover, Branch };
[[nodiscard]] constexpr std::string_view to_string(AbAssignment a) noexcept {
  switch (a) {
    case AbAssignment::Instrument:
      return "instrument";
    case AbAssignment::Time:
      return "time";
    case AbAssignment::Crossover:
      return "crossover";
    case AbAssignment::Branch:
      return "branch";
  }
  return "?";
}

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM" or "YYYY-MM-DD HH:MM:SS", with a T for the space and a trailing
// Z allowed, UTC, as ns since the epoch.
[[nodiscard]] std::optional<std::int64_t> parse_utc_time(std::string_view s);
// "YYYY-MM-DD HH:MM:SS UTC".
[[nodiscard]] std::string format_utc_time(std::int64_t ns);

class AbPlan {
 public:
  static constexpr int kNoArm = -1;

  // `venue_names` name the venue ids of `instruments` (Config::venues order), for [ab.weights]
  // keys written "venue:symbol". Throws ConfigError for a weight naming no instrument or an
  // instrument with no weight (when there are weights), and for an instrument or crossover split
  // over fewer instruments than arms.
  AbPlan(const AbSection& section,
         const InstrumentTable& instruments,
         std::span<const std::string> venue_names);

  [[nodiscard]] const AbSection& section() const noexcept { return s_; }
  [[nodiscard]] AbAssignment assignment() const noexcept { return kind_; }
  [[nodiscard]] std::size_t arms() const noexcept { return s_.arms.size(); }
  [[nodiscard]] std::size_t instruments() const noexcept { return slot_.size(); }
  [[nodiscard]] const std::string& arm_name(int arm) const {
    return s_.arms.at(static_cast<std::size_t>(arm)).name;
  }
  // The instrument's name in the configuration's terms: "venue:symbol".
  [[nodiscard]] const std::string& instrument_name(std::size_t inst) const {
    return names_.at(inst);
  }
  [[nodiscard]] std::int64_t start_ns() const noexcept { return start_ns_; }  // INT64_MIN: none
  [[nodiscard]] std::int64_t end_ns() const noexcept { return end_ns_; }      // INT64_MAX: none
  [[nodiscard]] std::int64_t block_ns() const noexcept { return block_ns_; }  // 0: no blocks
  // branch: the arm of this run, and the start when [ab] start names none.
  void set_branch(int arm) noexcept { branch_ = arm; }
  [[nodiscard]] int branch() const noexcept { return branch_; }
  void set_default_start(std::int64_t ns) noexcept {
    if (s_.start.empty()) start_ns_ = ns;
  }

  // The arm instrument `inst` is in at `t` (ns since the epoch): kNoArm before start and from end.
  [[nodiscard]] int arm_at(std::size_t inst, std::int64_t t) const noexcept;
  // The first time after `t` at which arm_at() may change, INT64_MAX when it never does.
  [[nodiscard]] std::int64_t next_change(std::int64_t t) const noexcept;
  // The block `t` falls in (time, crossover), 0 with instrument.
  [[nodiscard]] std::int64_t block_of(std::int64_t t) const noexcept;
  // The run of arms() blocks `t` falls in, each arm once in it (time, crossover); 0 with
  // instrument.
  [[nodiscard]] std::int64_t round_of(std::int64_t t) const noexcept;
  // The instrument's stratum (instrument, crossover; 0 with time) and its slot in it.
  [[nodiscard]] std::size_t stratum(std::size_t inst) const noexcept { return stratum_[inst]; }
  [[nodiscard]] std::size_t slot(std::size_t inst) const noexcept { return slot_[inst]; }
  // Every arm names the same parameters with the same values: an A/A test.
  [[nodiscard]] bool identical_arms() const noexcept;

 private:
  [[nodiscard]] std::size_t block_draw(std::int64_t block) const noexcept;

  AbSection s_;
  AbAssignment kind_ = AbAssignment::Instrument;
  std::int64_t start_ns_;
  std::int64_t end_ns_;
  std::int64_t block_ns_ = 0;
  int branch_ = 0;
  std::vector<std::uint8_t> slot_;
  std::vector<std::size_t> stratum_;
  std::vector<std::string> names_;
};

class AbController {
 public:
  using ParamValue = ParamPublisher::ParamValue;
  enum class State : std::uint8_t { Waiting, Running, Ended };

  // `publisher`'s values now are the configured parameters each arm starts from; it must outlive
  // the controller. Throws std::invalid_argument when an arm names a parameter the strategy does
  // not have or a value its schema or validate() refuses, or when the plan splits the
  // instruments and the strategy keeps one parameter set (ParamScope::Shared).
  AbController(AbPlan plan, ParamPublisher& publisher);

  [[nodiscard]] const AbPlan& plan() const noexcept { return plan_; }
  // Publishes the parameters of each instrument whose arm differs at `now` from the one it was
  // last put in, one update for every instrument when they all move to the same arm. False when
  // the sink refused one: the instruments not yet moved are moved by the next call.
  bool poll(Timestamp now);
  // The next time poll() has something to do after `now`; Timestamp::max() once ended.
  [[nodiscard]] Timestamp next_change(Timestamp now) const noexcept;
  // Ends the test at the next poll(): every instrument leaves its arm and gets the configured
  // parameters, or with `promote` the parameters of that arm. Later polls do nothing.
  void stop(int promote = AbPlan::kNoArm) noexcept;
  [[nodiscard]] State state(Timestamp now) const noexcept;
  [[nodiscard]] int promoted() const noexcept { return promote_; }
  // The arm's index by name, kNoArm when none has it.
  [[nodiscard]] int arm(std::string_view name) const noexcept;
  // A parameter some arm names: the test sets it while it runs.
  [[nodiscard]] bool owns(std::string_view param) const noexcept;
  // The values an arm's update carries (kNoArm: the configured ones), in name order.
  [[nodiscard]] const std::vector<ParamValue>& values(int arm) const;
  // The arm the instrument was last put in, kNoArm when none.
  [[nodiscard]] int current(std::size_t inst) const noexcept { return current_[inst]; }
  // Updates the publisher refused when they were published (a validate() that no longer holds
  // for an instrument's parameters), and the last reason.
  [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
  // fastmm-ctl ab: the test on the first line, then one line per topic: its arms, the block and
  // end, and each instrument's arm.
  [[nodiscard]] std::string status(Timestamp now) const;

 private:
  bool publish(InstrumentId inst, int values, std::uint8_t arm);

  AbPlan plan_;
  ParamPublisher* pub_;
  std::vector<ParamValue> base_;
  std::vector<std::vector<ParamValue>> arm_values_;
  std::vector<int> current_;
  std::string source_;
  bool stopping_ = false;
  bool ended_ = false;
  int promote_ = AbPlan::kNoArm;
  std::uint64_t refused_ = 0;
  std::string last_error_;
};

[[nodiscard]] constexpr std::string_view to_string(AbController::State s) noexcept {
  switch (s) {
    case AbController::State::Waiting:
      return "waiting";
    case AbController::State::Running:
      return "running";
    case AbController::State::Ended:
      return "ended";
  }
  return "?";
}

}  // namespace fastmm
