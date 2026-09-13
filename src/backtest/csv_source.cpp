#include "fastmm/backtest/csv_source.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fastmm::bt {

namespace {

std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("CsvSource: cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

bool parse_u64(std::string_view s, std::uint64_t& out) noexcept {
  if (s.empty()) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;  // overflow
    v = v * 10 + d;
  }
  out = v;
  return true;
}
bool parse_i64(std::string_view s, std::int64_t& out) noexcept {
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    s.remove_prefix(1);
  }
  std::uint64_t v = 0;
  if (!parse_u64(s, v) || v > static_cast<std::uint64_t>(INT64_MAX)) return false;
  out = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
  return true;
}

}  // namespace

bool CsvSource::parse_row(std::string_view line, Row& out) {
  line = trim(line);
  if (line.empty() || line.front() == '#') return false;
  std::string_view f[7];
  std::size_t n = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      if (n < 7) f[n] = trim(line.substr(start, i - start));
      ++n;
      start = i + 1;
    }
  }
  if (n != 7) throw std::runtime_error("CsvSource: expected 7 fields, got " + std::to_string(n));
  if (!parse_i64(f[0], out.ts)) {
    if (f[0] == "ts_ns" || f[0] == "ts") return false;  // header line
    throw std::runtime_error("CsvSource: bad ts '" + std::string(f[0]) + "'");
  }
  if (f[1] == "S" || f[1] == "0") {
    out.type = RowType::Snapshot;
  } else if (f[1] == "D" || f[1] == "1") {
    out.type = RowType::Delta;
  } else if (f[1] == "T" || f[1] == "2") {
    out.type = RowType::Trade;
  } else if (f[1] == "B" || f[1] == "3") {
    out.type = RowType::Ticker;
  } else {
    throw std::runtime_error("CsvSource: bad type '" + std::string(f[1]) + "'");
  }
  std::uint64_t inst = 0;
  if (!parse_u64(f[2], inst))
    throw std::runtime_error("CsvSource: bad inst '" + std::string(f[2]) + "'");
  out.inst = static_cast<std::uint32_t>(inst);
  if (f[3] == "B" || f[3] == "0" || f[3] == "bid" || f[3] == "buy") {
    out.side = Side::Buy;
  } else if (f[3] == "A" || f[3] == "1" || f[3] == "ask" || f[3] == "sell") {
    out.side = Side::Sell;
  } else {
    throw std::runtime_error("CsvSource: bad side '" + std::string(f[3]) + "'");
  }
  const auto px = Price::from_decimal(f[4]);
  const auto qty = Qty::from_decimal(f[5]);
  if (!px) throw std::runtime_error("CsvSource: bad price '" + std::string(f[4]) + "'");
  if (!qty) throw std::runtime_error("CsvSource: bad qty '" + std::string(f[5]) + "'");
  out.price = *px;
  out.qty = *qty;
  if (!parse_u64(f[6], out.seq))
    throw std::runtime_error("CsvSource: bad seq '" + std::string(f[6]) + "'");
  return true;
}

CsvSource::CsvSource(std::string text, VenueId venue, int) : text_(std::move(text)), asm_(venue) {
  // First pass: count rows and find the first timestamp (validates the whole file).
  Row r;
  std::int64_t prev_ts = 0;
  std::size_t pos = 0;
  std::size_t line_no = 0;
  while (pos < text_.size()) {
    const std::size_t nl = text_.find('\n', pos);
    const std::string_view line(text_.data() + pos,
                                (nl == std::string::npos ? text_.size() : nl) - pos);
    pos = nl == std::string::npos ? text_.size() : nl + 1;
    ++line_no;
    try {
      if (!parse_row(line, r)) continue;
    } catch (const std::runtime_error& e) {
      throw std::runtime_error(std::string(e.what()) + " (line " + std::to_string(line_no) + ")");
    }
    if (rows_ == 0) {
      first_ts_ = Timestamp{r.ts};
    } else if (r.ts < prev_ts) {
      throw std::runtime_error("CsvSource: ts_ns decreases (line " + std::to_string(line_no) + ")");
    }
    prev_ts = r.ts;
    ++rows_;
  }
}

CsvSource::CsvSource(const std::string& path, VenueId venue)
    : CsvSource(read_all(path), venue, 0) {}

CsvSource CsvSource::from_text(std::string text, VenueId venue) {
  return CsvSource(std::move(text), venue, 0);
}

bool CsvSource::provide(Row& r) {
  while (pos_ < text_.size()) {
    const std::size_t nl = text_.find('\n', pos_);
    const std::string_view line(text_.data() + pos_,
                                (nl == std::string::npos ? text_.size() : nl) - pos_);
    pos_ = nl == std::string::npos ? text_.size() : nl + 1;
    ++line_no_;
    if (parse_row(line, r)) return true;
  }
  return false;
}

const EventHeader* CsvSource::next() {
  return asm_.next([this](Row& r) { return provide(r); });
}

void CsvSource::reset() {
  pos_ = 0;
  line_no_ = 0;
  asm_.reset();
}

}  // namespace fastmm::bt
