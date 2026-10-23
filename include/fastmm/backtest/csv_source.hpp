#pragma once
// CsvSource: hand-rolled parser for the row format documented in data_source.hpp
// (`ts_ns,type,inst,side,price,qty,seq`, optional header line, '#' comments). Prices and
// quantities are parsed exactly with Fixed::from_decimal. A file is streamed through
// CsvLineReader, read once at construction to validate it and count its rows, then again from
// the start; next() allocates nothing.
#include "fastmm/backtest/binance_source.hpp"
#include "fastmm/backtest/data_source.hpp"

#include <cstddef>
#include <memory>
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
  CsvSource(std::string text, std::unique_ptr<CsvLineReader> file, VenueId venue);
  bool next_line(std::string_view& line);
  bool provide(Row& r);

  std::string text_;                     // from_text(): the whole input
  std::unique_ptr<CsvLineReader> file_;  // a path: read a buffer at a time
  std::size_t pos_ = 0;
  std::size_t line_no_ = 0;
  std::size_t rows_ = 0;
  Timestamp first_ts_{};
  RowAssembler asm_;
};

}  // namespace fastmm::bt
