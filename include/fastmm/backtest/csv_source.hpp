#pragma once
// CsvSource: hand-rolled parser for the row format documented in data_source.hpp
// (`ts_ns,type,inst,side,price,qty,seq`, optional header line, '#' comments). Prices and
// quantities are parsed exactly with Fixed::from_decimal; the whole file is read once at
// construction and next() allocates nothing.
#include "fastmm/backtest/data_source.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace fastmm::bt {

class CsvSource final : public MdSource {
 public:
  // Throws std::runtime_error when the file cannot be read or a row is malformed.
  explicit CsvSource(const std::string& path, VenueId venue = VenueId{0});
  // Parses in-memory text (tests).
  static CsvSource from_text(std::string text, VenueId venue = VenueId{0});

  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] std::size_t rows() const noexcept { return rows_; }

  // Parses one CSV line into a Row; false for blank/comment lines. Throws on bad fields.
  static bool parse_row(std::string_view line, Row& out);

 private:
  CsvSource(std::string text, VenueId venue, int /*tag*/);
  bool provide(Row& r);

  std::string text_;
  std::size_t pos_ = 0;
  std::size_t line_no_ = 0;
  std::size_t rows_ = 0;
  Timestamp first_ts_{};
  RowAssembler asm_;
};

}  // namespace fastmm::bt
