#pragma once
// Strategy parameter schema (8.6, ADR-0012).
//
//   struct MyParams {
//     FASTMM_PARAMS(MyParams)
//     FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
//     FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
//     FASTMM_PARAM(bool, hedge, false, false, true, "hedge fills")
//     FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000_qty, "quantity per level, base units")
//     FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 1000_bps, "half spread around mid")
//     FASTMM_PARAM_MS(stale_ms, milliseconds(2000), milliseconds(0), milliseconds(60000), "...")
//
//     // Optional cross-field check, run by StrategyBase::configure() after every key is applied.
//     std::optional<std::string> validate() const {
//       if (quote_qty > max_inventory) return "quote_qty must not exceed max_inventory";
//       return std::nullopt;
//     }
//   };
//
// FASTMM_PARAM declares the field *and* a schema entry (name, type, default, range, doc, parser
// and formatter). Field types: bool, integers, floating point (schema types int, double, bool) and
// Price, Qty, Notional (schema type decimal, parsed exactly). FASTMM_PARAM_BPS declares a Ratio
// configured in basis points (type bps, up to 4 decimals) and FASTMM_PARAM_MS a Duration configured
// in whole milliseconds (type ms). Exact values accept exponent notation ("2e-05"), because TOML
// floats reach the parser formatted by fmt and Python floats by repr; a value is rejected only if
// more decimals remain after applying the exponent than the type holds. Range checks compare typed
// values; `def`, `min` and `max` in ParamDesc are doubles for display and Python only.
//
// The schema is collected once by default-constructing the struct with a thread-local collector
// active, so registration is deterministic and needs no static initialisers. apply() maps string
// values (from TOML / CLI / Python) onto the fields. All of this is startup-only code (std::string
// and std::map are fine).
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace fastmm {

enum class ParamType : std::uint8_t {
  Int = 0,
  Double = 1,
  Bool = 2,
  Decimal = 3,  // Price, Qty, Notional
  Bps = 4,      // Ratio configured in basis points
  Millis = 5,   // Duration configured in milliseconds
};
[[nodiscard]] constexpr std::string_view to_string(ParamType t) noexcept {
  switch (t) {
    case ParamType::Int:
      return "int";
    case ParamType::Double:
      return "double";
    case ParamType::Bool:
      return "bool";
    case ParamType::Decimal:
      return "decimal";
    case ParamType::Bps:
      return "bps";
    case ParamType::Millis:
      return "ms";
  }
  return "?";
}

struct ParamDesc {
  const char* name;
  ParamType type;
  double def;  // display only (listings, Python); range checks use the typed bounds
  double min;
  double max;
  const char* doc;
  // Parses `value` exactly, checks it against the typed range and assigns the field. On failure
  // the field is unchanged and the message (without the parameter name) is returned.
  std::optional<std::string> (*parse)(void* obj, std::string_view value);
  // The field's current value in a form parse() accepts ("0.01", "2.5", "true", "2000").
  std::string (*format)(const void* obj);
};

inline constexpr std::size_t kMaxParams = 32;

class ParamSchema {
 public:
  bool add(const ParamDesc& d) noexcept { return descs_.push_back(d); }
  [[nodiscard]] const ParamDesc* find(std::string_view name) const noexcept {
    for (const ParamDesc& d : descs_) {
      if (name == d.name) return &d;
    }
    return nullptr;
  }
  [[nodiscard]] std::size_t size() const noexcept { return descs_.size(); }
  [[nodiscard]] const ParamDesc* begin() const noexcept { return descs_.begin(); }
  [[nodiscard]] const ParamDesc* end() const noexcept { return descs_.end(); }

 private:
  StaticVector<ParamDesc, kMaxParams> descs_;
};

using ParamMap = std::map<std::string, std::string>;

namespace detail {

inline thread_local ParamSchema* t_param_collector = nullptr;

// static_cast unless the types already match (keeps -Wuseless-cast quiet in macro expansions).
template <class T, class U>
constexpr T param_cast(U v) noexcept {
  if constexpr (std::is_same_v<T, U>) {
    return v;
  } else {
    return static_cast<T>(v);
  }
}

// v / 10^decimals exactly, without trailing zeros: "2.5", "-0.0001", "3".
inline std::string format_scaled(std::int64_t v, int decimals) {
  const std::uint64_t mag =
      v < 0 ? 0U - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
  std::uint64_t scale = 1;
  for (int k = 0; k < decimals; ++k) scale *= 10;
  std::string out = v < 0 ? "-" : "";
  out += std::to_string(mag / scale);
  std::uint64_t frac = mag % scale;
  if (frac != 0) {
    std::string digits(static_cast<std::size_t>(decimals), '0');
    for (std::size_t k = digits.size(); k > 0; --k) {
      digits[k - 1] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    while (digits.back() == '0') digits.pop_back();
    out += '.';
    out += digits;
  }
  return out;
}

struct PlainUnit {};
struct BpsUnit {};
struct MsUnit {};

// ParamCodec<T, Unit>: schema type, exact parse, format and display value of one field type.
template <class T, class Unit>
struct ParamCodec {
  static_assert(sizeof(T) == 0,
                "fastmm: FASTMM_PARAM supports bool, integers, floating point, Price, Qty and "
                "Notional; use FASTMM_PARAM_BPS for a Ratio and FASTMM_PARAM_MS for a Duration");
};

template <>
struct ParamCodec<bool, PlainUnit> {
  static constexpr ParamType kType = ParamType::Bool;
  static constexpr std::string_view kHint = " (true or false)";
  [[nodiscard]] static std::optional<bool> parse(std::string_view s) noexcept {
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return std::nullopt;
  }
  [[nodiscard]] static std::string format(bool v) { return v ? "true" : "false"; }
  [[nodiscard]] static double display(bool v) noexcept { return v ? 1.0 : 0.0; }
};

template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct ParamCodec<T, PlainUnit> {
  static constexpr ParamType kType = ParamType::Int;
  static constexpr std::string_view kHint = " (a whole number)";
  [[nodiscard]] static std::optional<T> parse(std::string_view s) noexcept {
    const std::optional<std::int64_t> v = parse_scaled_decimal(s, 0);  // "3", "3.0", "2e3"
    if (!v || !std::in_range<T>(*v)) return std::nullopt;
    return param_cast<T>(*v);
  }
  [[nodiscard]] static std::string format(T v) { return std::to_string(v); }
  [[nodiscard]] static double display(T v) noexcept { return static_cast<double>(v); }
};

template <class T>
  requires std::is_floating_point_v<T>
struct ParamCodec<T, PlainUnit> {
  static constexpr ParamType kType = ParamType::Double;
  static constexpr std::string_view kHint = " (a finite number)";
  [[nodiscard]] static std::optional<T> parse(std::string_view s) noexcept {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    double d = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(d))
      return std::nullopt;
    return param_cast<T>(d);
  }
  [[nodiscard]] static std::string format(T v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest round-trip form
    return {buf, r.ptr};
  }
  [[nodiscard]] static double display(T v) noexcept { return param_cast<double>(v); }
};

template <class Tag>
  requires(std::is_same_v<Fixed<Tag>, Price> || std::is_same_v<Fixed<Tag>, Qty> ||
           std::is_same_v<Fixed<Tag>, Notional>)
struct ParamCodec<Fixed<Tag>, PlainUnit> {
  static constexpr ParamType kType = ParamType::Decimal;
  static constexpr std::string_view kHint = " (at most 8 decimals)";
  [[nodiscard]] static std::optional<Fixed<Tag>> parse(std::string_view s) noexcept {
    return Fixed<Tag>::parse(s);
  }
  [[nodiscard]] static std::string format(Fixed<Tag> v) {
    return format_scaled(v.raw, kFixedDecimals);
  }
  [[nodiscard]] static double display(Fixed<Tag> v) noexcept { return v.to_double(); }
};

template <>
struct ParamCodec<Ratio, BpsUnit> {
  static constexpr ParamType kType = ParamType::Bps;
  static constexpr std::string_view kHint = " (basis points, at most 4 decimals)";
  [[nodiscard]] static std::optional<Ratio> parse(std::string_view s) noexcept {
    const std::optional<std::int64_t> raw = parse_scaled_decimal(s, kBpsDecimals);
    if (!raw) return std::nullopt;
    return Ratio::from_raw(*raw);
  }
  [[nodiscard]] static std::string format(Ratio v) { return format_scaled(v.raw, kBpsDecimals); }
  [[nodiscard]] static double display(Ratio v) noexcept { return v.to_bps(); }
};

template <>
struct ParamCodec<Duration, MsUnit> {
  static constexpr ParamType kType = ParamType::Millis;
  static constexpr std::string_view kHint = " (whole milliseconds)";
  static constexpr std::int64_t kNsPerMs = 1'000'000;
  [[nodiscard]] static std::optional<Duration> parse(std::string_view s) noexcept {
    const std::optional<std::int64_t> ms = parse_scaled_decimal(s, 0);
    constexpr std::int64_t kMaxMs = std::numeric_limits<std::int64_t>::max() / kNsPerMs;
    if (!ms || *ms > kMaxMs || *ms < -kMaxMs) return std::nullopt;
    return milliseconds(*ms);
  }
  [[nodiscard]] static std::string format(Duration v) { return format_scaled(v.ns, 6); }
  [[nodiscard]] static double display(Duration v) noexcept {
    return static_cast<double>(v.ns) / static_cast<double>(kNsPerMs);
  }
};

template <class Codec, class T>
std::optional<std::string> assign_param(T& field, std::string_view value, T lo, T hi) {
  const std::optional<T> v = Codec::parse(value);
  std::string err;
  if (!v) {
    return err.append("cannot parse '")
        .append(value)
        .append("' as ")
        .append(to_string(Codec::kType))
        .append(Codec::kHint);
  }
  if (*v < lo || hi < *v) {
    return err.append("value ")
        .append(value)
        .append(" outside [")
        .append(Codec::format(lo))
        .append(", ")
        .append(Codec::format(hi))
        .append("]");
  }
  field = *v;
  return std::nullopt;
}

inline void register_param(const ParamDesc& d) noexcept {
  if (t_param_collector != nullptr) t_param_collector->add(d);
}

template <class Self>
ParamSchema collect_schema() {
  static_assert(std::is_default_constructible_v<Self>);
  ParamSchema s;
  t_param_collector = &s;
  Self tmp{};
  static_cast<void>(tmp);
  t_param_collector = nullptr;
  return s;
}

// Applies every entry of `m`; returns the first error message, if any.
inline std::optional<std::string> apply_params(const ParamSchema& schema,
                                               void* obj,
                                               const ParamMap& m) {
  for (const auto& [key, value] : m) {
    const ParamDesc* d = schema.find(key);
    std::string err;  // built with append: startup path, but no needless temporaries
    if (d == nullptr) return err.append("unknown parameter '").append(key).append("'");
    if (auto e = d->parse(obj, value)) {
      return err.append("parameter '").append(key).append("': ").append(*e);
    }
  }
  return std::nullopt;
}

inline std::string describe_params(const ParamSchema& schema, const void* obj) {
  std::string out;
  for (const ParamDesc& d : schema) {
    out += d.name;
    out += '=';
    out += d.format(obj);
    out += ' ';
  }
  if (!out.empty()) out.pop_back();
  return out;
}

// Runs `std::optional<std::string> validate() const` when the params struct declares one.
template <class P>
std::optional<std::string> validate_params(const P& p) {
  if constexpr (requires { p.validate(); }) {
    static_assert(
        std::is_same_v<decltype(p.validate()), std::optional<std::string>>,
        "fastmm: validate() must be declared std::optional<std::string> validate() const");
    return p.validate();
  } else {
    static_assert(
        !requires(P& q) { q.validate(); },
        "fastmm: validate() must be const: std::optional<std::string> validate() const");
    return std::nullopt;
  }
}

}  // namespace detail
}  // namespace fastmm

#define FASTMM_PARAMS(Self)                                                          \
  using FastmmParamsSelf = Self;                                                     \
  static const ::fastmm::ParamSchema& schema() {                                     \
    static const ::fastmm::ParamSchema s = ::fastmm::detail::collect_schema<Self>(); \
    return s;                                                                        \
  }                                                                                  \
  std::optional<std::string> apply(const ::fastmm::ParamMap& m) {                    \
    return ::fastmm::detail::apply_params(schema(), this, m);                        \
  }                                                                                  \
  [[nodiscard]] std::string describe() const {                                       \
    return ::fastmm::detail::describe_params(schema(), this);                        \
  }

// Implementation of the FASTMM_PARAM* macros: the field, then a zero-size registrar whose
// constructor adds the schema entry while collect_schema() runs.
#define FASTMM_PARAM_IMPL(unit, type, name, def, lo, hi, doc)                                   \
  type name = def;                                                                              \
  struct FastmmReg_##name {                                                                     \
    FastmmReg_##name() noexcept {                                                               \
      using FastmmCodec = ::fastmm::detail::ParamCodec<type, unit>;                             \
      ::fastmm::detail::register_param(::fastmm::ParamDesc{                                     \
          #name,                                                                                \
          FastmmCodec::kType,                                                                   \
          FastmmCodec::display(::fastmm::detail::param_cast<type>(def)),                        \
          FastmmCodec::display(::fastmm::detail::param_cast<type>(lo)),                         \
          FastmmCodec::display(::fastmm::detail::param_cast<type>(hi)),                         \
          doc,                                                                                  \
          +[](void* fastmm_obj, std::string_view fastmm_value) {                                \
            return ::fastmm::detail::assign_param<FastmmCodec, type>(                           \
                static_cast<FastmmParamsSelf*>(fastmm_obj)->name,                               \
                fastmm_value,                                                                   \
                ::fastmm::detail::param_cast<type>(lo),                                         \
                ::fastmm::detail::param_cast<type>(hi));                                        \
          },                                                                                    \
          +[](const void* fastmm_obj) {                                                         \
            return FastmmCodec::format(static_cast<const FastmmParamsSelf*>(fastmm_obj)->name); \
          }});                                                                                  \
    }                                                                                           \
  };                                                                                            \
  [[no_unique_address]] FastmmReg_##name fastmm_reg_##name{};

// FASTMM_PARAM(type, name, default, min, max, doc): bool, integer, floating point, Price, Qty or
// Notional field.
#define FASTMM_PARAM(type, name, def, lo, hi, doc) \
  FASTMM_PARAM_IMPL(::fastmm::detail::PlainUnit, type, name, def, lo, hi, doc)
// FASTMM_PARAM_BPS(name, default, min, max, doc): Ratio field configured in basis points
// (5_bps, 0.25_bps; up to 4 decimals).
#define FASTMM_PARAM_BPS(name, def, lo, hi, doc) \
  FASTMM_PARAM_IMPL(::fastmm::detail::BpsUnit, ::fastmm::Ratio, name, def, lo, hi, doc)
// FASTMM_PARAM_MS(name, default, min, max, doc): Duration field configured in whole milliseconds
// (bounds as Durations: milliseconds(2000)).
#define FASTMM_PARAM_MS(name, def, lo, hi, doc) \
  FASTMM_PARAM_IMPL(::fastmm::detail::MsUnit, ::fastmm::Duration, name, def, lo, hi, doc)
