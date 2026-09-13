#pragma once
// Strategy parameter schema (8.6).
//
//   struct MyParams {
//     FASTMM_PARAMS(MyParams)
//     FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
//     FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
//   };
//
// FASTMM_PARAM declares the field *and* a schema entry (name, type, default, range, doc,
// setter/getter). The schema is collected once by default-constructing the struct with a
// thread-local collector active, so registration is deterministic and needs no static
// initialisers. apply() maps string values (from TOML / CLI / Python) onto the fields with
// range validation. All of this is startup-only code (std::string / std::map are fine).
#include "fastmm/core/containers/static_vector.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace fastmm {

enum class ParamType : std::uint8_t { Int = 0, Double = 1, Bool = 2 };
[[nodiscard]] constexpr std::string_view to_string(ParamType t) noexcept {
  switch (t) {
    case ParamType::Int:
      return "int";
    case ParamType::Double:
      return "double";
    case ParamType::Bool:
      return "bool";
  }
  return "?";
}

struct ParamDesc {
  const char* name;
  ParamType type;
  double def;
  double min;
  double max;
  const char* doc;
  void (*set)(void* obj, double v);
  double (*get)(const void* obj);
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

template <class T>
constexpr ParamType param_type_of() noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return ParamType::Bool;
  } else if constexpr (std::is_integral_v<T>) {
    return ParamType::Int;
  } else {
    static_assert(std::is_floating_point_v<T>,
                  "FASTMM_PARAM supports bool, integral and floating types");
    return ParamType::Double;
  }
}

// Cast-free for double so -Wuseless-cast stays quiet in the macro expansion.
template <class T>
constexpr double as_double(T v) noexcept {
  if constexpr (std::is_same_v<T, double>) {
    return v;
  } else {
    return static_cast<double>(v);
  }
}

template <class T>
constexpr T from_double(double v) noexcept {
  if constexpr (std::is_same_v<T, double>) {
    return v;
  } else {
    return static_cast<T>(v);
  }
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

inline std::optional<double> parse_param_value(ParamType t, std::string_view v) {
  std::string s(v);
  if (t == ParamType::Bool) {
    if (s == "true" || s == "1" || s == "yes" || s == "on") return 1.0;
    if (s == "false" || s == "0" || s == "no" || s == "off") return 0.0;
    return std::nullopt;
  }
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return std::nullopt;
  if (t == ParamType::Int && d != static_cast<double>(static_cast<long long>(d)))
    return std::nullopt;
  return d;
}

// Applies every entry of `m`; returns the first error message, if any.
inline std::optional<std::string> apply_params(const ParamSchema& schema,
                                               void* obj,
                                               const ParamMap& m) {
  for (const auto& [key, value] : m) {
    const ParamDesc* d = schema.find(key);
    if (d == nullptr) return "unknown parameter '" + key + "'";
    const auto v = parse_param_value(d->type, value);
    if (!v)
      return "parameter '" + key + "': cannot parse '" + value + "' as " +
             std::string(to_string(d->type));
    if (*v < d->min || *v > d->max) {
      return "parameter '" + key + "': value " + value + " outside [" + std::to_string(d->min) +
             ", " + std::to_string(d->max) + "]";
    }
    d->set(obj, *v);
  }
  return std::nullopt;
}

inline std::string describe_params(const ParamSchema& schema, const void* obj) {
  std::string out;
  for (const ParamDesc& d : schema) {
    out += d.name;
    out += '=';
    const double v = d.get(obj);
    if (d.type == ParamType::Bool) {
      out += v != 0.0 ? "true" : "false";
    } else if (d.type == ParamType::Int) {
      out += std::to_string(static_cast<long long>(v));
    } else {
      out += std::to_string(v);
    }
    out += ' ';
  }
  if (!out.empty()) out.pop_back();
  return out;
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

#define FASTMM_PARAM(type, name, def, lo, hi, doc)                                             \
  type name = def;                                                                             \
  struct FastmmReg_##name {                                                                    \
    FastmmReg_##name() noexcept {                                                              \
      ::fastmm::detail::register_param(::fastmm::ParamDesc{                                    \
          #name,                                                                               \
          ::fastmm::detail::param_type_of<type>(),                                             \
          ::fastmm::detail::as_double(def),                                                    \
          ::fastmm::detail::as_double(lo),                                                     \
          ::fastmm::detail::as_double(hi),                                                     \
          doc,                                                                                 \
          +[](void* o, double v) {                                                             \
            static_cast<FastmmParamsSelf*>(o)->name = ::fastmm::detail::from_double<type>(v);  \
          },                                                                                   \
          +[](const void* o) {                                                                 \
            return ::fastmm::detail::as_double(static_cast<const FastmmParamsSelf*>(o)->name); \
          }});                                                                                 \
    }                                                                                          \
  };                                                                                           \
  [[no_unique_address]] FastmmReg_##name fastmm_reg_##name{};
