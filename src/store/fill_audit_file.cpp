#include "fastmm/store/fill_audit_file.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <cctype>
#include <charconv>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace fastmm::store {

namespace {

using Fields = std::map<std::string, std::string, std::less<>>;

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

// The first of `names` present (case-insensitive keys: `f` holds them lowered).
const std::string* field(const Fields& f, std::initializer_list<std::string_view> names) {
  for (const std::string_view n : names) {
    const auto it = f.find(lower(n));
    if (it != f.end()) return &it->second;
  }
  return nullptr;
}

bool parse_int(std::string_view s, std::int64_t& out) {
  s = trim(s);
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

// Unix ms, or "YYYY-MM-DD HH:MM:SS[.fff]" (or with a 'T', and a trailing 'Z'), UTC.
bool parse_time_ms(std::string_view s, std::int64_t& out) {
  s = trim(s);
  if (parse_int(s, out)) return true;
  if (!s.empty() && (s.back() == 'Z' || s.back() == 'z')) s.remove_suffix(1);
  std::tm tm{};
  int ms = 0;
  int frac_digits = 0;
  if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != ' ' && s[10] != 'T') ||
      s[13] != ':' || s[16] != ':')
    return false;
  std::int64_t v[6] = {};
  const std::size_t at[6] = {0, 5, 8, 11, 14, 17};
  const std::size_t len[6] = {4, 2, 2, 2, 2, 2};
  for (int i = 0; i < 6; ++i) {
    if (!parse_int(s.substr(at[i], len[i]), v[i])) return false;
  }
  if (s.size() > 19) {
    if (s[19] != '.') return false;
    for (std::size_t i = 20; i < s.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) return false;
      if (frac_digits < 3) {
        ms = ms * 10 + (s[i] - '0');
        ++frac_digits;
      }
    }
    while (frac_digits++ < 3) ms *= 10;
  }
  tm.tm_year = static_cast<int>(v[0]) - 1900;
  tm.tm_mon = static_cast<int>(v[1]) - 1;
  tm.tm_mday = static_cast<int>(v[2]);
  tm.tm_hour = static_cast<int>(v[3]);
  tm.tm_min = static_cast<int>(v[4]);
  tm.tm_sec = static_cast<int>(v[5]);
  out = timegm(&tm) * 1000 + ms;
  return true;
}

bool parse_decimal(std::string_view s, std::int64_t& raw) {
  const auto v = Qty::from_decimal(trim(s));
  if (!v) return false;
  raw = v->raw;
  return true;
}

// One execution from its fields; `row` names it in the error.
Result<AuditFill, std::string> from_fields(const Fields& f, std::size_t row) {
  const auto bad = [row](const std::string& what) {
    return fail(fmt::format("row {}: {}", row, what));
  };
  AuditFill a;
  const std::string* symbol = field(f, {"symbol"});
  const std::string* id = field(f, {"exec_id", "id", "trade_id", "tradeId"});
  const std::string* price = field(f, {"price"});
  const std::string* qty = field(f, {"qty", "quantity"});
  const std::string* time = field(f, {"time_ms", "time"});
  if (symbol == nullptr || symbol->empty()) return bad("no symbol");
  if (id == nullptr || id->empty()) return bad("no exec_id (or id)");
  if (price == nullptr || !parse_decimal(*price, a.price_raw))
    return bad("no price, or not a decimal");
  if (qty == nullptr || !parse_decimal(*qty, a.qty_raw)) return bad("no qty, or not a decimal");
  if (time == nullptr || !parse_time_ms(*time, a.time_ms))
    return bad("no time_ms, or neither Unix ms nor a UTC time");
  a.symbol = *symbol;
  a.exec_id = *id;
  if (const std::string* side = field(f, {"side"}); side != nullptr && !side->empty()) {
    const std::string s = lower(trim(*side));
    if (s == "buy" || s == "b") {
      a.side = Side::Buy;
    } else if (s == "sell" || s == "s") {
      a.side = Side::Sell;
    } else {
      return bad(fmt::format("side '{}' is neither buy nor sell", *side));
    }
  } else if (const std::string* buyer = field(f, {"isBuyer", "buyer"}); buyer != nullptr) {
    const std::string b = lower(trim(*buyer));
    if (b != "true" && b != "false" && b != "1" && b != "0")
      return bad(fmt::format("isBuyer '{}' is not a boolean", *buyer));
    a.side = b == "true" || b == "1" ? Side::Buy : Side::Sell;
  } else {
    return bad("no side (or isBuyer)");
  }
  if (const std::string* o = field(f, {"order_id", "orderId"})) a.order_id = *o;
  if (const std::string* c = field(f, {"cl_ord_id", "clientOrderId"})) a.cl_ord_id = *c;
  if (const std::string* fee = field(f, {"fee", "commission"}); fee != nullptr && !fee->empty()) {
    if (!parse_decimal(*fee, a.fee_raw)) return bad(fmt::format("fee '{}' is not a decimal", *fee));
    a.has_fee = true;
  }
  if (const std::string* asset = field(f, {"fee_asset", "commissionAsset"})) a.fee_asset = *asset;
  if (const std::string* venue = field(f, {"venue"})) a.venue = *venue;
  return a;
}

// A JSON array of flat objects, read with a small scanner (the store library carries no JSON
// parser): string values unescaped, numbers and literals as written (exact decimals), nested
// arrays and objects skipped.
class JsonScanner {
 public:
  explicit JsonScanner(std::string_view s) : s_(s) {}

  Result<std::vector<AuditFill>, std::string> rows() {
    std::vector<AuditFill> out;
    if (!take('[')) return fail(std::string("the JSON is not an array of executions"));
    if (take(']')) return finish(std::move(out));
    for (std::size_t row = 1;; ++row) {
      Fields f;
      if (!object(f)) return fail(fmt::format("row {}: {}", row, error_));
      auto a = from_fields(f, row);
      if (!a) return fail(a.error());
      out.push_back(std::move(*a));
      if (take(',')) continue;
      if (take(']')) return finish(std::move(out));
      return fail(fmt::format("row {}: expected ',' or ']' after it", row));
    }
  }

 private:
  Result<std::vector<AuditFill>, std::string> finish(std::vector<AuditFill> out) {
    skip_ws();
    if (i_ != s_.size()) return fail(std::string("text after the array"));
    return out;
  }
  void skip_ws() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])) != 0) ++i_;
  }
  bool take(char c) {
    skip_ws();
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool bad(const char* what) {
    error_ = what;
    return false;
  }
  bool string(std::string& out) {
    if (!take('"')) return bad("expected a string");
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) break;
      const char e = s_[i_++];
      switch (e) {
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          out += '\r';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'u': {
          // Only ASCII is meaningful in an id or a symbol; anything else is kept as '?'.
          if (i_ + 4 > s_.size()) return bad("bad \\u escape");
          unsigned v = 0;
          const auto [p, ec] = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, v, 16);
          if (ec != std::errc{} || p != s_.data() + i_ + 4) return bad("bad \\u escape");
          out += v < 0x80 ? static_cast<char>(v) : '?';
          i_ += 4;
          break;
        }
        default:
          out += e;  // \" \\ \/
      }
    }
    return bad("unterminated string");
  }
  // A nested array or object, skipped whole.
  bool skip_nested() {
    int depth = 0;
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '"') {
        std::string ignored;
        if (!string(ignored)) return false;
        continue;
      }
      ++i_;
      if (c == '[' || c == '{') ++depth;
      if (c == ']' || c == '}') {
        if (--depth == 0) return true;
      }
    }
    return bad("unterminated array or object");
  }
  bool value(std::string& out, bool& keep) {
    skip_ws();
    keep = true;
    if (i_ >= s_.size()) return bad("expected a value");
    const char c = s_[i_];
    if (c == '"') return string(out);
    if (c == '[' || c == '{') {
      keep = false;
      return skip_nested();
    }
    const std::size_t start = i_;
    while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
           std::isspace(static_cast<unsigned char>(s_[i_])) == 0)
      ++i_;
    if (i_ == start) return bad("expected a value");
    out = s_.substr(start, i_ - start);
    if (out == "null") out.clear();
    return true;
  }
  bool object(Fields& f) {
    if (!take('{')) return bad("not a JSON object");
    if (take('}')) return true;
    for (;;) {
      std::string key;
      if (!string(key)) return false;
      if (!take(':')) return bad("expected ':'");
      std::string v;
      bool keep = true;
      if (!value(v, keep)) return false;
      if (keep) f[lower(key)] = std::move(v);
      if (take(',')) continue;
      if (take('}')) return true;
      return bad("expected ',' or '}'");
    }
  }

  std::string_view s_;
  std::size_t i_ = 0;
  const char* error_ = "";
};

Result<std::vector<AuditFill>, std::string> parse_json(std::string_view text) {
  return JsonScanner(text).rows();
}

// One CSV line into cells: commas, double-quoted cells with "" for a quote.
std::vector<std::string> split_csv(std::string_view line) {
  std::vector<std::string> cells;
  std::string cell;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        cell += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        cell += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      cells.emplace_back(trim(cell));
      cell.clear();
    } else {
      cell += c;
    }
  }
  cells.emplace_back(trim(cell));
  return cells;
}

Result<std::vector<AuditFill>, std::string> parse_csv(std::string_view text) {
  std::vector<AuditFill> out;
  std::vector<std::string> header;
  std::size_t row = 0;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++row;
    if (trim(line).empty()) continue;
    std::vector<std::string> cells = split_csv(line);
    if (header.empty()) {
      for (std::string& c : cells) c = lower(c);
      header = std::move(cells);
      continue;
    }
    if (cells.size() != header.size())
      return fail(
          fmt::format("row {}: {} cells, the header has {}", row, cells.size(), header.size()));
    Fields f;
    for (std::size_t i = 0; i < cells.size(); ++i) f[header[i]] = std::move(cells[i]);
    auto a = from_fields(f, row);
    if (!a) return fail(a.error());
    out.push_back(std::move(*a));
  }
  if (header.empty()) return fail(std::string("empty file: no header row"));
  return out;
}

}  // namespace

Result<std::vector<AuditFill>, std::string> parse_venue_fills(std::string_view text) {
  const std::string_view body = trim(text);
  if (!body.empty() && body.front() == '[') return parse_json(body);
  return parse_csv(text);
}

Result<std::vector<AuditFill>, std::string> load_venue_fills(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  auto r = parse_venue_fills(ss.str());
  if (!r) return fail(path + ": " + r.error());
  return r;
}

}  // namespace fastmm::store
