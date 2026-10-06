#include "fastmm/core/fill_audit.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace fastmm {

namespace {

std::string dec(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  const std::size_t n = Qty::from_raw(raw).to_decimal(buf);
  return {buf, n};
}

struct Key {
  std::string symbol;
  std::string exec_id;
  auto operator<=>(const Key&) const = default;
};

bool in_window(std::int64_t t, std::int64_t from_ms, std::int64_t to_ms) noexcept {
  return (from_ms == 0 || t >= from_ms) && (to_ms == 0 || t <= to_ms);
}

// The fields of `b` that differ from `v`; the order and the fee only where both name one.
std::uint8_t differing(const AuditFill& v, const AuditFill& b) noexcept {
  std::uint8_t f = 0;
  if (v.qty_raw != b.qty_raw) f |= kAuditQty;
  if (v.price_raw != b.price_raw) f |= kAuditPrice;
  if (v.has_fee && b.has_fee && v.fee_raw != b.fee_raw) f |= kAuditFee;
  if (v.side != b.side) f |= kAuditSide;
  if (!v.order_id.empty() && !b.order_id.empty()) {
    if (v.order_id != b.order_id) f |= kAuditOrder;
  } else if (!v.cl_ord_id.empty() && !b.cl_ord_id.empty() && v.cl_ord_id != b.cl_ord_id) {
    f |= kAuditOrder;
  }
  return f;
}

}  // namespace

std::string audit_fields_text(std::uint8_t fields) {
  static constexpr std::pair<std::uint8_t, std::string_view> kNames[] = {
      {kAuditQty, "qty"},
      {kAuditPrice, "price"},
      {kAuditFee, "fee"},
      {kAuditSide, "side"},
      {kAuditOrder, "order"},
  };
  std::string out;
  for (const auto& [bit, name] : kNames) {
    if ((fields & bit) == 0) continue;
    if (!out.empty()) out += ',';
    out += name;
  }
  return out;
}

std::int64_t FillAuditReport::first_missing_ms() const noexcept {
  std::int64_t t = 0;
  for (const AuditFill& f : missing) {
    if (t == 0 || f.time_ms < t) t = f.time_ms;
  }
  return t;
}

std::string normalize_symbol(std::string_view symbol) {
  std::string out;
  out.reserve(symbol.size());
  for (const char c : symbol) {
    if (c == '-' || c == '_' || c == '/' || c == ' ') continue;
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

FillAuditReport audit_fills(std::span<const AuditFill> venue,
                            std::span<const AuditFill> booked,
                            std::int64_t from_ms,
                            std::int64_t to_ms) {
  FillAuditReport r;
  r.from_ms = from_ms;
  r.to_ms = to_ms;
  struct Group {
    std::vector<const AuditFill*> venue;
    std::vector<const AuditFill*> booked;
  };
  std::map<Key, Group> groups;
  const auto same_side = [](const std::vector<const AuditFill*>& rows, Side s) {
    return std::find_if(rows.begin(), rows.end(), [s](const AuditFill* f) { return f->side == s; });
  };
  for (const AuditFill& f : venue) {
    if (f.exec_id.empty()) continue;
    Group& g = groups[Key{normalize_symbol(f.symbol), f.exec_id}];
    if (same_side(g.venue, f.side) != g.venue.end()) continue;  // the same row twice
    g.venue.push_back(&f);
  }
  for (const AuditFill& f : booked) {
    if (f.exec_id.empty()) continue;
    Group& g = groups[Key{normalize_symbol(f.symbol), f.exec_id}];
    if (same_side(g.booked, f.side) != g.booked.end()) {
      if (in_window(f.time_ms, from_ms, to_ms)) r.duplicates.push_back(f);
      continue;
    }
    g.booked.push_back(&f);
  }
  const auto inside = [&](const AuditFill* f) { return in_window(f->time_ms, from_ms, to_ms); };
  for (auto& [key, g] : groups) {
    for (const AuditFill* f : g.venue) r.venue_rows += inside(f) ? 1U : 0U;
    for (const AuditFill* f : g.booked) r.booked_rows += inside(f) ? 1U : 0U;
    // The same side first, then whatever is left pairs up as a side mismatch.
    std::vector<std::pair<const AuditFill*, const AuditFill*>> pairs;
    for (auto it = g.venue.begin(); it != g.venue.end();) {
      const auto b = same_side(g.booked, (*it)->side);
      if (b == g.booked.end()) {
        ++it;
        continue;
      }
      pairs.emplace_back(*it, *b);
      g.booked.erase(b);
      it = g.venue.erase(it);
    }
    while (!g.venue.empty() && !g.booked.empty()) {
      pairs.emplace_back(g.venue.front(), g.booked.front());
      g.venue.erase(g.venue.begin());
      g.booked.erase(g.booked.begin());
    }
    for (const auto& [v, b] : pairs) {
      if (!inside(v) && !inside(b)) continue;
      const std::uint8_t fields = differing(*v, *b);
      if (fields == 0) {
        ++r.matched;
      } else {
        r.mismatched.push_back(AuditMismatch{*v, *b, fields});
      }
    }
    for (const AuditFill* v : g.venue) {
      if (inside(v)) r.missing.push_back(*v);
    }
    for (const AuditFill* b : g.booked) {
      if (inside(b)) r.phantom.push_back(*b);
    }
  }
  const auto by_time = [](const AuditFill& a, const AuditFill& b) { return a.time_ms < b.time_ms; };
  std::stable_sort(r.missing.begin(), r.missing.end(), by_time);
  std::stable_sort(r.phantom.begin(), r.phantom.end(), by_time);
  std::stable_sort(r.duplicates.begin(), r.duplicates.end(), by_time);
  std::stable_sort(
      r.mismatched.begin(), r.mismatched.end(), [](const AuditMismatch& a, const AuditMismatch& b) {
        return a.venue.time_ms < b.venue.time_ms;
      });
  return r;
}

std::string describe(const AuditFill& f) {
  std::string out = fmt::format(
      "{} {} {} {} @ {}", f.symbol, f.exec_id, to_string(f.side), dec(f.qty_raw), dec(f.price_raw));
  if (f.has_fee) {
    out += fmt::format(" fee {}", dec(f.fee_raw));
    if (!f.fee_asset.empty()) out += fmt::format(" {}", f.fee_asset);
  }
  if (!f.order_id.empty()) out += fmt::format(" order {}", f.order_id);
  if (!f.cl_ord_id.empty()) out += fmt::format(" ({})", f.cl_ord_id);
  out += fmt::format(" at {}", f.time_ms);
  return out;
}

}  // namespace fastmm
