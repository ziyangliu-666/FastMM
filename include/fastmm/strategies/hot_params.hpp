#pragma once
// Parameters of a Python hot strategy (ADR-0013): a table built at run time from the class's
// fastmm.Param declarations and their offsets in the `self` record. It gives ParamPublisher a
// ParamSchema to validate publishes against (publisher()), the journal its parameter table
// (schema()), and HotStrategy the place of each parameter in the parameter block (slots()).
//
// Field types follow fastmm.Param: Int (int64), Bool (uint8) and Double (double, with an int64
// `_raw` twin holding the value in 1e-8 fixed point). A ParamUpdate carries an Int as its value, a
// Bool as 0 or 1 and a Double as the bits of the double. Writing a Double also writes its twin,
// rounded by hot_fixed_raw() like fastmm._hot.decl.fixed_raw: the shortest decimal form of the
// double, to the nearest 1e-8, halves away from zero.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/params.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fastmm {

// `v` in 1e-8 fixed point from its shortest decimal form, halves away from zero. False when `v`
// is not finite or the result does not fit in int64.
[[nodiscard]] inline bool hot_fixed_raw(double v, std::int64_t& out) noexcept {
  if (!std::isfinite(v)) return false;
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
  if (r.ec != std::errc{}) return false;
  const char* p = buf;
  const bool negative = *p == '-';
  if (negative) ++p;
  std::uint64_t digits = 0;
  int n_digits = 0;
  for (; p < r.ptr && *p != 'e'; ++p) {
    if (*p == '.') continue;
    digits = digits * 10 + static_cast<std::uint64_t>(*p - '0');
    ++n_digits;
  }
  if (p == r.ptr) return false;
  ++p;  // 'e'
  const bool exp_negative = *p == '-';
  if (*p == '-' || *p == '+') ++p;
  int exp = 0;
  if (std::from_chars(p, r.ptr, exp).ec != std::errc{}) return false;
  if (exp_negative) exp = -exp;
  // |v| == digits * 10^(exp - (n_digits - 1)), so raw == digits * 10^k.
  const int k = exp - (n_digits - 1) + kFixedDecimals;
  using U = Uint128;
  const auto pow10 = [](int e) {
    U x = 1;
    for (int i = 0; i < e; ++i) x *= 10;
    return x;
  };
  U mag = 0;
  if (k >= 0) {
    if (digits != 0) {
      if (k > 19) return false;  // digits * 10^20 >= 1e20 > INT64_MAX
      mag = U{digits} * pow10(k);
    }
  } else if (-k <= 20) {
    const U div = pow10(-k);
    mag = U{digits} / div;
    if (2 * (U{digits} % div) >= div) ++mag;
  }  // else below half a raw unit: digits < 1e17 < 10^-k / 2
  if (mag > static_cast<U>(std::numeric_limits<std::int64_t>::max())) return false;
  const auto m = static_cast<std::int64_t>(mag);
  out = negative ? -m : m;
  return true;
}

// Where one parameter sits in the parameter block (HotProgram::params); the index is the
// ParamUpdate field index.
struct HotParamSlot {
  std::uint32_t offset = 0;
  std::int32_t raw_offset = -1;  // Double: the `_raw` twin, or -1
  ParamType type = ParamType::Double;
};

// Engine thread: assigns a ParamUpdate raw value to a parameter block (no checks).
inline void hot_write_param(const HotParamSlot& s, std::uint8_t* block, std::int64_t raw) noexcept {
  switch (s.type) {
    case ParamType::Bool: {
      const std::uint8_t b = raw != 0 ? 1 : 0;
      std::memcpy(block + s.offset, &b, 1);
      return;
    }
    case ParamType::Double: {
      const auto d = std::bit_cast<double>(raw);
      std::memcpy(block + s.offset, &d, sizeof d);
      if (s.raw_offset >= 0) {
        std::int64_t fixed = 0;
        if (!hot_fixed_raw(d, fixed)) fixed = 0;
        std::memcpy(block + s.raw_offset, &fixed, sizeof fixed);
      }
      return;
    }
    default:
      std::memcpy(block + s.offset, &raw, sizeof raw);
      return;
  }
}

// One parameter as fastmm.Param declares it.
struct HotParamField {
  std::string name;
  ParamType type = ParamType::Double;  // Int, Double or Bool
  std::uint32_t offset = 0;            // in the record
  std::int32_t raw_offset = -1;        // Double: the `_raw` twin, or -1
  std::optional<double> min;           // Double: inclusive bounds
  std::optional<double> max;
  std::optional<std::int64_t> int_min;  // Int: inclusive bounds
  std::optional<std::int64_t> int_max;
  double def = 0.0;  // display only
  std::string doc;
};

class HotParamTable;

// A publisher's copy of one parameter block.
struct HotParamBlock {
  const HotParamTable* table = nullptr;
  std::vector<std::uint8_t> bytes;
};

class HotParamTable {
 public:
  // Throws std::invalid_argument for more than kMaxParams fields, a duplicate or empty name, a
  // type other than Int, Double and Bool, or a field outside the first `param_bytes` bytes.
  HotParamTable(std::vector<HotParamField> fields, std::size_t param_bytes)
      : fields_(std::move(fields)), param_bytes_(param_bytes) {
    if (fields_.size() > kMaxParams) {
      throw std::invalid_argument("fastmm: at most " + std::to_string(kMaxParams) +
                                  " parameters, got " + std::to_string(fields_.size()));
    }
    for (std::size_t i = 0; i < fields_.size(); ++i) {
      const HotParamField& f = fields_[i];
      const std::string where = "fastmm: parameter '" + f.name + "'";
      if (f.name.empty()) throw std::invalid_argument("fastmm: a parameter has no name");
      for (std::size_t j = 0; j < i; ++j) {
        if (fields_[j].name == f.name) throw std::invalid_argument(where + " is declared twice");
      }
      std::size_t width = 8;
      if (f.type == ParamType::Bool) {
        width = 1;
      } else if (f.type != ParamType::Int && f.type != ParamType::Double) {
        throw std::invalid_argument(where + ": type must be int, double or bool");
      }
      if (std::size_t{f.offset} + width > param_bytes_)
        throw std::invalid_argument(where + " lies outside the parameter block");
      if (f.type == ParamType::Double && f.raw_offset >= 0 &&
          static_cast<std::size_t>(f.raw_offset) + 8 > param_bytes_)
        throw std::invalid_argument(where + ": its _raw field lies outside the parameter block");
      if (f.type != ParamType::Double && f.raw_offset >= 0)
        throw std::invalid_argument(where + ": only a double has a _raw field");
      const Fns& fns = fns_table()[i];
      ParamDesc d{};
      d.name = fields_[i].name.c_str();
      d.type = f.type;
      d.def = f.def;
      const double inf = std::numeric_limits<double>::infinity();
      d.min = f.min.value_or(-inf);
      d.max = f.max.value_or(inf);
      if (f.type == ParamType::Int) {
        d.min = f.int_min.has_value() ? static_cast<double>(f.int_min.value()) : -inf;
        d.max = f.int_max.has_value() ? static_cast<double>(f.int_max.value()) : inf;
      }
      d.doc = fields_[i].doc.c_str();
      d.parse = fns.parse;
      d.format = fns.format;
      d.get_raw = fns.get_raw;
      d.set_raw = fns.set_raw;
      static_cast<void>(schema_.add(d));
    }
  }
  // The schema's names and the blocks point into the table.
  HotParamTable(const HotParamTable&) = delete;
  HotParamTable& operator=(const HotParamTable&) = delete;
  HotParamTable(HotParamTable&&) = delete;
  HotParamTable& operator=(HotParamTable&&) = delete;
  ~HotParamTable() = default;

  [[nodiscard]] const ParamSchema& schema() const noexcept { return schema_; }
  [[nodiscard]] std::span<const HotParamField> fields() const noexcept { return fields_; }
  [[nodiscard]] std::size_t param_bytes() const noexcept { return param_bytes_; }
  [[nodiscard]] std::vector<HotParamSlot> slots() const {
    std::vector<HotParamSlot> out;
    out.reserve(fields_.size());
    for (const HotParamField& f : fields_) out.push_back({f.offset, f.raw_offset, f.type});
    return out;
  }

  // A publisher whose copy starts from `block` (the record's first param_bytes() bytes), one copy
  // per instrument. The table must outlive it.
  [[nodiscard]] std::unique_ptr<ParamPublisher> publisher(ParamSink sink,
                                                          std::span<const std::uint8_t> block,
                                                          std::size_t instruments) const {
    if (block.size() < param_bytes_)
      throw std::invalid_argument("fastmm: the parameter block is shorter than the table");
    HotParamBlock initial{
        this,
        std::vector<std::uint8_t>(block.begin(),
                                  block.begin() + static_cast<std::ptrdiff_t>(param_bytes_))};
    return std::make_unique<ParamPublisher>(sink, schema_, kBlockOps, &initial, instruments);
  }

  // `value` into field `i` of `bytes`: parsed and range-checked as fastmm.Param does. The error
  // message has no parameter name; `bytes` is unchanged then.
  [[nodiscard]] std::optional<std::string> parse(std::size_t i,
                                                 std::uint8_t* bytes,
                                                 std::string_view value) const {
    const HotParamField& f = fields_[i];
    std::string err;
    const auto cannot = [&](std::string_view hint) {
      return err.append("cannot parse '")
          .append(value)
          .append("' as ")
          .append(to_string(f.type))
          .append(hint);
    };
    const auto outside = [&](const std::string& lo, const std::string& hi) {
      return err.append("value ")
          .append(value)
          .append(" outside [")
          .append(lo)
          .append(", ")
          .append(hi)
          .append("]");
    };
    switch (f.type) {
      case ParamType::Bool: {
        const std::optional<bool> v = detail::ParamCodec<bool, detail::PlainUnit>::parse(value);
        if (!v) return cannot(detail::ParamCodec<bool, detail::PlainUnit>::kHint);
        hot_write_param({f.offset, -1, f.type}, bytes, *v ? 1 : 0);
        return std::nullopt;
      }
      case ParamType::Int: {
        using C = detail::ParamCodec<std::int64_t, detail::PlainUnit>;
        const std::optional<std::int64_t> v = C::parse(value);
        if (!v) return cannot(C::kHint);
        if ((f.int_min && *v < *f.int_min) || (f.int_max && *f.int_max < *v)) {
          return outside(f.int_min ? std::to_string(*f.int_min) : "-inf",
                         f.int_max ? std::to_string(*f.int_max) : "inf");
        }
        hot_write_param({f.offset, -1, f.type}, bytes, *v);
        return std::nullopt;
      }
      default: {
        using C = detail::ParamCodec<double, detail::PlainUnit>;
        const std::optional<double> v = C::parse(value);
        if (!v) return cannot(C::kHint);
        if ((f.min && *v < *f.min) || (f.max && *f.max < *v)) {
          return outside(f.min ? C::format(*f.min) : "-inf", f.max ? C::format(*f.max) : "inf");
        }
        std::int64_t fixed = 0;
        if (!hot_fixed_raw(*v, fixed))
          return err.append("value ").append(value).append(" does not fit in 1e-8 fixed point");
        hot_write_param({f.offset, f.raw_offset, f.type}, bytes, std::bit_cast<std::int64_t>(*v));
        return std::nullopt;
      }
    }
  }

  [[nodiscard]] std::int64_t get_raw(std::size_t i, const std::uint8_t* bytes) const noexcept {
    const HotParamField& f = fields_[i];
    if (f.type == ParamType::Bool) return bytes[f.offset] != 0 ? 1 : 0;
    std::int64_t raw = 0;
    std::memcpy(&raw, bytes + f.offset, sizeof raw);  // an int64 or the bits of a double
    return raw;
  }

  [[nodiscard]] std::string format(std::size_t i, const std::uint8_t* bytes) const {
    const HotParamField& f = fields_[i];
    const std::int64_t raw = get_raw(i, bytes);
    if (f.type == ParamType::Bool) return raw != 0 ? "true" : "false";
    if (f.type == ParamType::Int) return std::to_string(raw);
    return detail::ParamCodec<double, detail::PlainUnit>::format(std::bit_cast<double>(raw));
  }

 private:
  struct Fns {
    std::optional<std::string> (*parse)(void* obj, std::string_view value);
    std::string (*format)(const void* obj);
    std::int64_t (*get_raw)(const void* obj) noexcept;
    void (*set_raw)(void* obj, std::int64_t raw) noexcept;
  };

  // ParamDesc's functions take only the block, so field i gets its own instantiation.
  template <std::size_t I>
  struct Access {
    static std::optional<std::string> parse(void* obj, std::string_view value) {
      auto* b = static_cast<HotParamBlock*>(obj);
      return b->table->parse(I, b->bytes.data(), value);
    }
    static std::string format(const void* obj) {
      const auto* b = static_cast<const HotParamBlock*>(obj);
      return b->table->format(I, b->bytes.data());
    }
    static std::int64_t get_raw(const void* obj) noexcept {
      const auto* b = static_cast<const HotParamBlock*>(obj);
      return b->table->get_raw(I, b->bytes.data());
    }
    static void set_raw(void* obj, std::int64_t raw) noexcept {
      auto* b = static_cast<HotParamBlock*>(obj);
      const HotParamField& f = b->table->fields_[I];
      hot_write_param({f.offset, f.raw_offset, f.type}, b->bytes.data(), raw);
    }
  };

  template <std::size_t... I>
  static constexpr std::array<Fns, sizeof...(I)> make_fns(std::index_sequence<I...>) noexcept {
    return {
        {Fns{&Access<I>::parse, &Access<I>::format, &Access<I>::get_raw, &Access<I>::set_raw}...}};
  }
  static const std::array<Fns, kMaxParams>& fns_table() noexcept {
    static constexpr std::array<Fns, kMaxParams> kFns =
        make_fns(std::make_index_sequence<kMaxParams>{});
    return kFns;
  }

  static constexpr ParamPublisher::BlockOps kBlockOps{
      [](const void* src) -> void* {
        return new HotParamBlock(*static_cast<const HotParamBlock*>(src));
      },
      [](void* p) noexcept { delete static_cast<HotParamBlock*>(p); },
      [](const void*) -> std::optional<std::string> { return std::nullopt; }};

  std::vector<HotParamField> fields_;
  std::size_t param_bytes_;
  ParamSchema schema_;
};

}  // namespace fastmm
