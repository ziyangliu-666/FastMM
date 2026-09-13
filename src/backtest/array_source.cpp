#include "fastmm/backtest/array_source.hpp"

#include <cmath>
#include <stdexcept>

namespace fastmm::bt {

namespace {
std::string size_error(const char* col, std::size_t got, std::size_t want) {
  return std::string("column '") + col + "' has " + std::to_string(got) + " rows, expected " +
         std::to_string(want);
}
}  // namespace

std::string ArrayColumns::validate() const {
  const std::size_t n = ts.size();
  if (type.size() != n) return size_error("type", type.size(), n);
  if (inst.size() != n) return size_error("inst", inst.size(), n);
  if (side.size() != n) return size_error("side", side.size(), n);
  if (!seq.empty() && seq.size() != n) return size_error("seq", seq.size(), n);
  if (n == 0) return {};
  if (price_i64.empty() == price_f64.empty())
    return "exactly one of the int64 / float64 price columns must be given";
  if (qty_i64.empty() == qty_f64.empty())
    return "exactly one of the int64 / float64 qty columns must be given";
  if (!price_i64.empty() && price_i64.size() != n) return size_error("price", price_i64.size(), n);
  if (!price_f64.empty() && price_f64.size() != n) return size_error("price", price_f64.size(), n);
  if (!qty_i64.empty() && qty_i64.size() != n) return size_error("qty", qty_i64.size(), n);
  if (!qty_f64.empty() && qty_f64.size() != n) return size_error("qty", qty_f64.size(), n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::string row = " at row " + std::to_string(i);
    if (type[i] > 3) return "type " + std::to_string(type[i]) + " is not 0..3" + row;
    if (side[i] != 0 && side[i] != 1)
      return "side " + std::to_string(side[i]) + " is not 0/1" + row;
    if (i > 0 && ts[i] < ts[i - 1]) return "ts decreases" + row;
    if (!price_f64.empty() && !std::isfinite(price_f64[i])) return "price is not finite" + row;
    if (!qty_f64.empty() && !std::isfinite(qty_f64[i])) return "qty is not finite" + row;
  }
  return {};
}

ArraySource::ArraySource(const ArrayColumns& cols, VenueId venue) : cols_(cols), asm_(venue) {
  if (std::string err = cols_.validate(); !err.empty())
    throw std::invalid_argument("ArraySource: " + err);
}

bool ArraySource::provide(Row& r) noexcept {
  if (i_ >= cols_.size()) return false;
  const std::size_t i = i_++;
  r.ts = cols_.ts[i];
  r.type = static_cast<RowType>(cols_.type[i]);
  r.inst = cols_.inst[i];
  r.side = cols_.side[i] == 0 ? Side::Buy : Side::Sell;
  r.price = cols_.price_i64.empty() ? Price::from_double(cols_.price_f64[i])
                                    : Price::from_raw(cols_.price_i64[i]);
  r.qty =
      cols_.qty_i64.empty() ? Qty::from_double(cols_.qty_f64[i]) : Qty::from_raw(cols_.qty_i64[i]);
  r.seq = cols_.seq.empty() ? 0 : cols_.seq[i];
  return true;
}

const EventHeader* ArraySource::next() {
  return asm_.next([this](Row& r) { return provide(r); });
}

void ArraySource::reset() {
  i_ = 0;
  asm_.reset();
}

}  // namespace fastmm::bt
