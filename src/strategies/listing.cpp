#include "fastmm/strategies/listing.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace fastmm {

namespace {

void append_json_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::array<char, 8> buf{};
          const int n = std::snprintf(buf.data(), buf.size(), "\\u%04x", static_cast<unsigned>(c));
          out.append(buf.data(), static_cast<std::size_t>(n));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// Shortest text that reads back as the same double; null for non-finite values.
void append_json_number(std::string& out, double v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  std::array<char, 32> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  if (ec != std::errc{}) {
    out += "null";
    return;
  }
  out.append(buf.data(), end);
}

std::string text_line(const ParamDesc& d) {
  const std::string type(to_string(d.type));
  std::string line(256, '\0');
  for (int pass = 0; pass < 2; ++pass) {
    const int n = std::snprintf(line.data(),
                                line.size(),
                                "  %-26s %-7s default=%-10g [%g, %g]  %s\n",
                                d.name,
                                type.c_str(),
                                d.def,
                                d.min,
                                d.max,
                                d.doc);
    if (n < 0) return {};
    if (static_cast<std::size_t>(n) < line.size()) {
      line.resize(static_cast<std::size_t>(n));
      return line;
    }
    line.assign(static_cast<std::size_t>(n) + 1, '\0');  // too long: once more with room
  }
  return line;
}

}  // namespace

std::optional<ListFormat> parse_list_format(std::string_view s) noexcept {
  if (s == "text") return ListFormat::Text;
  if (s == "json") return ListFormat::Json;
  return std::nullopt;
}

std::string format_strategies(const StrategyRegistry& r, TransportKind kind, ListFormat format) {
  std::string out;
  if (format == ListFormat::Text) {
    for (const StrategyEntry& e : r.entries()) {
      if (!e.supports(kind)) continue;
      out.append(e.name).append("\n");
      for (const ParamDesc& d : *e.schema) out += text_line(d);
    }
    return out;
  }
  static constexpr std::array<TransportKind, 3> kOrder{
      TransportKind::Sim, TransportKind::Replay, TransportKind::Live};
  out += "{\"strategies\": [";
  bool first = true;
  for (const StrategyEntry& e : r.entries()) {
    if (!e.supports(kind)) continue;
    out += first ? "\n  {\"name\": " : ",\n  {\"name\": ";
    first = false;
    append_json_string(out, e.name);
    out += ", \"transports\": [";
    bool first_kind = true;
    for (const TransportKind k : kOrder) {
      if (!e.supports(k)) continue;
      if (!first_kind) out += ", ";
      first_kind = false;
      append_json_string(out, to_string(k));
    }
    out += "], \"params\": [";
    bool first_param = true;
    for (const ParamDesc& d : *e.schema) {
      out += first_param ? "\n    {\"name\": " : ",\n    {\"name\": ";
      first_param = false;
      append_json_string(out, d.name);
      out += ", \"type\": ";
      append_json_string(out, to_string(d.type));
      out += ", \"default\": ";
      append_json_number(out, d.def);
      out += ", \"min\": ";
      append_json_number(out, d.min);
      out += ", \"max\": ";
      append_json_number(out, d.max);
      out += ", \"doc\": ";
      append_json_string(out, d.doc);
      out += '}';
    }
    out += first_param ? "]}" : "\n  ]}";
  }
  out += first ? "]}\n" : "\n]}\n";
  return out;
}

}  // namespace fastmm
