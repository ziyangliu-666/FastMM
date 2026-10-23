#include "fastmm/strategies/ab_test.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace fastmm {

namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000;
constexpr std::int64_t kNsPerDay = 86'400 * kNsPerSec;
// Salts of the two draws, so the instrument split and the block order are independent.
constexpr std::uint64_t kStratumSalt = 0x5354524154554dULL;  // "STRATUM"
constexpr std::uint64_t kBlockSalt = 0x424c4f434bULL;        // "BLOCK"

// days since 1970-01-01 <-> y/m/d (Howard Hinnant's algorithms).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
}

bool take_int(std::string_view& s, std::size_t digits, int& out) noexcept {
  if (s.size() < digits) return false;
  for (std::size_t i = 0; i < digits; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  std::from_chars(s.data(), s.data() + digits, out);
  s.remove_prefix(digits);
  return true;
}
bool take(std::string_view& s, char c) noexcept {
  if (s.empty() || s.front() != c) return false;
  s.remove_prefix(1);
  return true;
}

// splitmix64: the seeded draws.
std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// A permutation of 0..n-1 drawn from (seed, salt, index).
void draw(std::uint64_t seed,
          std::uint64_t salt,
          std::int64_t index,
          std::size_t n,
          std::uint8_t* out) noexcept {
  std::uint64_t state = mix(seed ^ mix(salt ^ mix(static_cast<std::uint64_t>(index))));
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(i);
  for (std::size_t i = n; i > 1; --i) {
    state = mix(state);
    std::swap(out[i - 1], out[state % i]);
  }
}

// "3d 04h", "41m 12s", "9s".
std::string span_text(std::int64_t ns) {
  const std::int64_t s = std::max<std::int64_t>(0, ns / kNsPerSec);
  if (s >= 86'400) return fmt::format("{}d {:02}h", s / 86'400, s % 86'400 / 3'600);
  if (s >= 3'600) return fmt::format("{}h {:02}m", s / 3'600, s % 3'600 / 60);
  if (s >= 60) return fmt::format("{}m {:02}s", s / 60, s % 60);
  return fmt::format("{}s", s);
}

std::string describe(const ParamMap& params) {
  if (params.empty()) return "the configured parameters";
  std::string out;
  for (const auto& [k, v] : params) {
    if (!out.empty()) out += ' ';
    out.append(k).append("=").append(v);
  }
  return out;
}

}  // namespace

std::optional<std::int64_t> parse_utc_time(std::string_view s) {
  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int sec = 0;
  if (!take_int(s, 4, y) || !take(s, '-') || !take_int(s, 2, mo) || !take(s, '-') ||
      !take_int(s, 2, d))
    return std::nullopt;
  if (!s.empty() && (s.front() == 'T' || s.front() == ' ')) {
    s.remove_prefix(1);
    if (!take_int(s, 2, h) || !take(s, ':') || !take_int(s, 2, mi)) return std::nullopt;
    if (take(s, ':') && !take_int(s, 2, sec)) return std::nullopt;
  }
  if (!s.empty() && (s == "Z" || s == "z")) s.remove_prefix(1);
  if (!s.empty()) return std::nullopt;
  static constexpr int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (mo < 1 || mo > 12 || d < 1 || d > kDays[mo - 1] || h > 23 || mi > 59 || sec > 59)
    return std::nullopt;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  if (mo == 2 && d == 29 && !leap) return std::nullopt;
  const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return days * kNsPerDay +
         (std::int64_t{h} * 3600 + std::int64_t{mi} * 60 + std::int64_t{sec}) * kNsPerSec;
}

std::string format_utc_time(std::int64_t ns) {
  const std::int64_t days = floor_div(ns, kNsPerDay);
  const std::int64_t secs = (ns - days * kNsPerDay) / kNsPerSec;
  std::int64_t y = 0;
  unsigned m = 0;
  unsigned d = 0;
  civil_from_days(days, y, m, d);
  return fmt::format(
      "{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", y, m, d, secs / 3600, secs % 3600 / 60, secs % 60);
}

// ---- AbPlan ---------------------------------------------------------------------------------

AbPlan::AbPlan(const AbSection& section,
               const InstrumentTable& instruments,
               std::span<const std::string> venue_names)
    : s_(section),
      start_ns_(std::numeric_limits<std::int64_t>::min()),
      end_ns_(std::numeric_limits<std::int64_t>::max()) {
  const std::size_t arms = s_.arms.size();
  if (arms < 2 || arms > kMaxAbArms)
    throw ConfigError(fmt::format("[ab] needs 2 to {} arms, got {}", kMaxAbArms, arms));
  if (s_.assignment == "time") {
    kind_ = AbAssignment::Time;
  } else if (s_.assignment == "crossover") {
    kind_ = AbAssignment::Crossover;
  } else if (s_.assignment == "instrument") {
    kind_ = AbAssignment::Instrument;
  } else if (s_.assignment == "branch") {
    kind_ = AbAssignment::Branch;
  } else {
    throw ConfigError("ab.assignment must be instrument, time, crossover or branch");
  }
  if (kind_ == AbAssignment::Time || kind_ == AbAssignment::Crossover) {
    if (s_.block_s <= 0 || s_.block_s > std::numeric_limits<std::int64_t>::max() / kNsPerSec / 4)
      throw ConfigError("ab.block_s must be > 0");
    block_ns_ = s_.block_s * kNsPerSec;
  }
  if (!s_.start.empty()) {
    const std::optional<std::int64_t> t = parse_utc_time(s_.start);
    if (!t) throw ConfigError("ab.start: '" + s_.start + "' is not a UTC time");
    start_ns_ = *t;
  }
  if (!s_.end.empty()) {
    const std::optional<std::int64_t> t = parse_utc_time(s_.end);
    if (!t) throw ConfigError("ab.end: '" + s_.end + "' is not a UTC time");
    end_ns_ = *t;
  }

  const std::size_t n = instruments.size();
  names_.reserve(n);
  for (const Instrument& inst : instruments) {
    std::string name(inst.symbol.view());
    if (inst.venue.value < venue_names.size())
      name = std::string(venue_names[inst.venue.value]).append(":").append(name);
    names_.push_back(std::move(name));
  }
  slot_.assign(n, 0);
  stratum_.assign(n, 0);
  if (kind_ == AbAssignment::Time || kind_ == AbAssignment::Branch) return;
  if (n < arms) {
    throw ConfigError(fmt::format(
        "[ab] assignment = \"{}\" splits {} instrument(s) over {} arms; it needs at least one "
        "instrument per arm",
        s_.assignment,
        n,
        arms));
  }
  std::vector<double> weight(n, 1.0);
  if (!s_.weights.empty()) {
    std::vector<bool> has(n, false);
    for (const auto& [key, w] : s_.weights) {
      std::size_t found = n;
      std::size_t matches = 0;
      for (std::size_t i = 0; i < n; ++i) {
        const bool hit =
            key.find(':') != std::string::npos
                ? names_[i] == key
                : instruments.get(InstrumentId{static_cast<std::uint32_t>(i)}).symbol.view() == key;
        if (!hit) continue;
        found = i;
        ++matches;
      }
      if (matches == 0) throw ConfigError("[ab.weights]: no instrument '" + key + "'");
      if (matches > 1)
        throw ConfigError("[ab.weights]: '" + key +
                          "' is on more than one venue; write it \"venue:symbol\"");
      weight[found] = w;
      has[found] = true;
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (!has[i])
        throw ConfigError("[ab.weights] has no weight for " + names_[i] +
                          ": give every instrument one, or none");
    }
  }
  // Heaviest first, the instrument order breaking ties; strata of `arms` in that order.
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return weight[a] > weight[b];
  });
  std::uint8_t perm[kMaxAbArms] = {};
  for (std::size_t r = 0; r < n; ++r) {
    const std::size_t j = r / arms;
    if (r % arms == 0) draw(s_.seed, kStratumSalt, static_cast<std::int64_t>(j), arms, perm);
    slot_[order[r]] = perm[r % arms];
    stratum_[order[r]] = j;
  }
}

std::size_t AbPlan::block_draw(std::int64_t block) const noexcept {
  const auto n = static_cast<std::int64_t>(arms());
  const std::int64_t round = floor_div(block, n);
  std::uint8_t perm[kMaxAbArms] = {};
  draw(s_.seed, kBlockSalt, round, arms(), perm);
  return perm[block - round * n];
}

int AbPlan::arm_at(std::size_t inst, std::int64_t t) const noexcept {
  if (t < start_ns_ || t >= end_ns_ || inst >= slot_.size()) return kNoArm;
  switch (kind_) {
    case AbAssignment::Instrument:
      return slot_[inst];
    case AbAssignment::Time:
      return static_cast<int>(block_draw(block_of(t)));
    case AbAssignment::Crossover:
      return static_cast<int>((slot_[inst] + block_draw(block_of(t))) % arms());
    case AbAssignment::Branch:
      return branch_;
  }
  return kNoArm;
}

std::int64_t AbPlan::block_of(std::int64_t t) const noexcept {
  return block_ns_ > 0 ? floor_div(t, block_ns_) : 0;
}

std::int64_t AbPlan::round_of(std::int64_t t) const noexcept {
  return floor_div(block_of(t), static_cast<std::int64_t>(arms()));
}

std::int64_t AbPlan::next_change(std::int64_t t) const noexcept {
  std::int64_t next = std::numeric_limits<std::int64_t>::max();
  if (t < start_ns_) next = start_ns_;
  if (t < end_ns_ && end_ns_ < next) next = end_ns_;
  if (block_ns_ > 0 && t < end_ns_) {
    const std::int64_t k = block_of(t);
    if (k < std::numeric_limits<std::int64_t>::max() / block_ns_ - 1) {
      const std::int64_t boundary = (k + 1) * block_ns_;
      if (boundary > start_ns_ && boundary < next) next = boundary;
    }
  }
  return next;
}

bool AbPlan::identical_arms() const noexcept {
  for (const AbArmSection& a : s_.arms) {
    if (a.params != s_.arms.front().params) return false;
  }
  return true;
}

// ---- AbController ---------------------------------------------------------------------------

AbController::AbController(AbPlan plan, ParamPublisher& publisher)
    : plan_(std::move(plan)), pub_(&publisher), current_(plan_.instruments(), AbPlan::kNoArm) {
  const AbSection& s = plan_.section();
  source_ = "ab:" + s.name;
  if ((plan_.assignment() == AbAssignment::Instrument ||
       plan_.assignment() == AbAssignment::Crossover) &&
      !publisher.per_instrument()) {
    throw std::invalid_argument(fmt::format(
        "[ab] assignment = \"{}\" gives instruments different parameters, and the strategy keeps "
        "one set for all of them (ParamScope::Shared); use assignment = \"time\"",
        s.assignment));
  }
  // Each parameter an arm names, with the first arm naming it.
  std::map<std::string, std::string> owned;
  for (const AbArmSection& a : s.arms) {
    for (const auto& [name, value] : a.params) owned.emplace(name, a.name);
  }
  const std::vector<ParamPublisher::Value> configured = publisher.values();
  for (const auto& [name, arm] : owned) {
    const auto it = std::find_if(configured.begin(),
                                 configured.end(),
                                 [&](const ParamPublisher::Value& v) { return v.name == name; });
    if (it == configured.end())
      throw std::invalid_argument(
          std::string("[ab] arm '").append(arm).append("': unknown parameter '").append(name) +
          "'");
    base_.emplace_back(name, it->text);
  }
  for (const AbArmSection& a : s.arms) {
    std::vector<ParamValue> values = base_;
    for (ParamValue& v : values) {
      if (const auto it = a.params.find(v.first); it != a.params.end()) v.second = it->second;
    }
    ParamUpdateMsg m{};
    if (auto err = publisher.build(values, ParamPublisher::kAllInstruments, m))
      throw std::invalid_argument("[ab] arm '" + a.name + "': " + *err);
    arm_values_.push_back(std::move(values));
  }
}

const std::vector<AbController::ParamValue>& AbController::values(int arm) const {
  return arm == AbPlan::kNoArm ? base_ : arm_values_.at(static_cast<std::size_t>(arm));
}

bool AbController::publish(InstrumentId inst, int values, std::uint8_t arm) {
  try {
    if (pub_->publish_as(
            this->values(values), inst, ParamUpdateMsg::Origin::Experiment, source_, arm) == 0)
      return false;
  } catch (const std::invalid_argument& e) {
    // The instrument's parameters no longer take the arm's (validate() across a value an
    // operator changed since): it keeps its own, counted and named in status().
    ++refused_;
    last_error_ = e.what();
  }
  return true;
}

bool AbController::poll(Timestamp now) {
  if (ended_) return true;
  const std::size_t n = current_.size();
  if (stopping_) {
    if (!publish(ParamPublisher::kAllInstruments, promote_, ParamUpdateMsg::kArmNone)) return false;
    current_.assign(n, AbPlan::kNoArm);
    ended_ = true;
    return true;
  }
  const auto tag = [](int arm) {
    return arm == AbPlan::kNoArm ? ParamUpdateMsg::kArmNone : static_cast<std::uint8_t>(arm + 1);
  };
  bool moved = false;
  bool together = true;
  const int first = n == 0 ? AbPlan::kNoArm : plan_.arm_at(0, now.ns);
  for (std::size_t i = 0; i < n; ++i) {
    const int arm = plan_.arm_at(i, now.ns);
    moved = moved || arm != current_[i];
    together = together && arm == first;
  }
  if (!moved) return true;
  if (together) {
    if (!publish(ParamPublisher::kAllInstruments, first, tag(first))) return false;
    current_.assign(n, first);
    return true;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const int arm = plan_.arm_at(i, now.ns);
    if (arm == current_[i]) continue;
    if (!publish(InstrumentId{static_cast<std::uint32_t>(i)}, arm, tag(arm))) return false;
    current_[i] = arm;
  }
  return true;
}

Timestamp AbController::next_change(Timestamp now) const noexcept {
  if (ended_) return Timestamp::max();
  if (stopping_) return now;
  return Timestamp{plan_.next_change(now.ns)};
}

void AbController::stop(int promote) noexcept {
  if (ended_ || stopping_) return;
  stopping_ = true;
  promote_ = promote;
}

AbController::State AbController::state(Timestamp now) const noexcept {
  if (ended_ || stopping_ || now.ns >= plan_.end_ns()) return State::Ended;
  return now.ns < plan_.start_ns() ? State::Waiting : State::Running;
}

int AbController::arm(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < plan_.arms(); ++i) {
    if (plan_.arm_name(static_cast<int>(i)) == name) return static_cast<int>(i);
  }
  return AbPlan::kNoArm;
}

bool AbController::owns(std::string_view param) const noexcept {
  return std::any_of(
      base_.begin(), base_.end(), [&](const ParamValue& v) { return v.first == param; });
}

std::string AbController::status(Timestamp now) const {
  const AbSection& s = plan_.section();
  const State st = state(now);
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "{}: {}, assignment {}, {} arms, seed {}",
                 s.name,
                 to_string(st),
                 s.assignment,
                 plan_.arms(),
                 s.seed);
  if (plan_.block_ns() > 0) fmt::format_to(it, ", block {} s", s.block_s);
  out += '\n';
  for (std::size_t a = 0; a < plan_.arms(); ++a) {
    const auto in_arm = std::count(current_.begin(), current_.end(), static_cast<int>(a));
    fmt::format_to(it,
                   "arm        {} {}: {}; {} instrument(s) now\n",
                   a,
                   s.arms[a].name,
                   describe(s.arms[a].params),
                   in_arm);
  }
  if (ended_ || stopping_) {
    fmt::format_to(it,
                   "stopped    every instrument on {}\n",
                   promote_ == AbPlan::kNoArm
                       ? std::string("the configured parameters")
                       : "arm " + plan_.arm_name(promote_) + "'s parameters (promoted)");
  } else if (st == State::Waiting) {
    fmt::format_to(it,
                   "start      {} (in {})\n",
                   format_utc_time(plan_.start_ns()),
                   span_text(plan_.start_ns() - now.ns));
  } else if (st == State::Running && plan_.block_ns() > 0) {
    const std::int64_t k = plan_.block_of(now.ns);
    const std::int64_t until = (k + 1) * plan_.block_ns();
    fmt::format_to(it,
                   "block      {} until {} (in {})\n",
                   k,
                   format_utc_time(until),
                   span_text(until - now.ns));
  }
  if (!ended_ && !stopping_ && plan_.end_ns() != std::numeric_limits<std::int64_t>::max()) {
    fmt::format_to(it,
                   "end        {} ({})\n",
                   format_utc_time(plan_.end_ns()),
                   now.ns < plan_.end_ns() ? "in " + span_text(plan_.end_ns() - now.ns)
                                           : std::string("passed"));
  }
  if (refused_ != 0) fmt::format_to(it, "refused    {}: {}\n", refused_, last_error_);
  for (std::size_t i = 0; i < current_.size(); ++i) {
    fmt::format_to(it,
                   "instrument {} {}\n",
                   plan_.instrument_name(i),
                   current_[i] == AbPlan::kNoArm ? std::string("-") : plan_.arm_name(current_[i]));
  }
  return out;
}

}  // namespace fastmm
