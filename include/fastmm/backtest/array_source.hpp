#pragma once
// ArraySource: zero-copy structure-of-arrays input, the numpy path of the Python bindings
// (8.5). The caller keeps the columns alive for the lifetime of the source. Column dtypes:
//
//   ts     int64    event time, ns, non-decreasing
//   type   uint8    RowType: 0 snapshot level, 1 delta level, 2 trade, 3 book ticker
//   inst   uint32   dense instrument id
//   side   int8     0 bid / buy aggressor, 1 ask / sell aggressor
//   price  int64 (raw 1e-8 fixed point) or float64      exactly one of the two
//   qty    int64 (raw 1e-8 fixed point) or float64      exactly one of the two
//   seq    uint64   optional venue update id (empty span == 0 for every row)
//
// The columns are validated once at construction (sizes, enum ranges, ordering, finite
// doubles); next() allocates nothing.
#include "fastmm/backtest/data_source.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fastmm::bt {

struct ArrayColumns {
  std::span<const std::int64_t> ts;
  std::span<const std::uint8_t> type;
  std::span<const std::uint32_t> inst;
  std::span<const std::int8_t> side;
  std::span<const std::int64_t> price_i64;
  std::span<const double> price_f64;
  std::span<const std::int64_t> qty_i64;
  std::span<const double> qty_f64;
  std::span<const std::uint64_t> seq;

  [[nodiscard]] std::size_t size() const noexcept { return ts.size(); }
  // Empty string when the columns are usable, otherwise a description of the first problem.
  [[nodiscard]] std::string validate() const;
};

class ArraySource final : public MdSource {
 public:
  // Throws std::invalid_argument with validate()'s message when the columns are unusable.
  explicit ArraySource(const ArrayColumns& cols, VenueId venue = VenueId{0});
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override {
    return cols_.size() == 0 ? Timestamp{} : Timestamp{cols_.ts[0]};
  }
  [[nodiscard]] std::size_t rows() const noexcept { return cols_.size(); }

 private:
  bool provide(Row& r) noexcept;
  ArrayColumns cols_;
  std::size_t i_ = 0;
  RowAssembler asm_;
};

}  // namespace fastmm::bt
