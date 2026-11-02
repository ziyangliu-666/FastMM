#include "fastmm/backtest/mark_fills.hpp"

#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/messages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastmm::bt {

namespace {

struct Query {
  std::int64_t t = 0;
  std::uint32_t file = 0;
  std::uint32_t row = 0;
  std::uint32_t column = 0;
};

// The columns of a CSV line, without quoting (these files have none).
std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream s(line);
  for (std::string f; std::getline(s, f, ',');) out.push_back(f);
  return out;
}

}  // namespace

std::uint64_t mark_fills(const std::string& journal,
                         const std::vector<std::string>& inputs,
                         const MarkOptions& opt) {
  JournalSource source(journal, opt.strip_own);
  const auto instruments = source.reader().instruments();
  std::unordered_map<std::string, std::uint32_t> id_of;
  std::vector<std::string> symbols;
  std::vector<std::uint32_t> primary(instruments.size());
  for (std::uint32_t i = 0; i < instruments.size(); ++i) {
    symbols.emplace_back(instruments[i].symbol.view());
    primary[i] = id_of.try_emplace(symbols.back(), i).first->second;
  }

  const std::size_t columns = opt.horizons_ns.size();
  std::vector<std::vector<Query>> queries(instruments.size());
  std::vector<std::vector<double>> marks(inputs.size());
  std::uint64_t rows_total = 0;
  for (std::uint32_t f = 0; f < inputs.size(); ++f) {
    std::ifstream in(inputs[f]);
    if (!in) throw std::runtime_error("mark: cannot read " + inputs[f]);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("mark: empty file " + inputs[f]);
    const std::vector<std::string> head = split(line);
    const auto column_of = [&](const char* name) {
      const auto it = std::find(head.begin(), head.end(), name);
      if (it == head.end()) throw std::runtime_error(inputs[f] + ": no column " + name);
      return static_cast<std::size_t>(it - head.begin());
    };
    const std::size_t ct = column_of("t_ns");
    const std::size_t cs = column_of("symbol");
    std::uint32_t row = 0;
    while (std::getline(in, line)) {
      const std::vector<std::string> v = split(line);
      if (v.size() <= std::max(ct, cs)) throw std::runtime_error(inputs[f] + ": short row");
      const std::int64_t t = std::stoll(v[ct]);
      if (const auto it = id_of.find(v[cs]); it != id_of.end()) {
        for (std::uint32_t c = 0; c < columns; ++c)
          queries[it->second].push_back({t + opt.horizons_ns[c], f, row, c});
      }
      ++row;
    }
    marks[f].assign(row * columns, std::numeric_limits<double>::quiet_NaN());
    rows_total += row;
  }
  for (auto& q : queries)
    std::sort(q.begin(), q.end(), [](const Query& a, const Query& b) { return a.t < b.t; });

  std::vector<std::size_t> next(instruments.size(), 0);
  std::vector<double> mid(instruments.size(), std::numeric_limits<double>::quiet_NaN());
  std::vector<double> written(instruments.size(), std::numeric_limits<double>::quiet_NaN());

  std::FILE* bars = nullptr;
  std::int64_t bar_end = 0;
  if (!opt.bars_out.empty()) {
    bars = std::fopen(opt.bars_out.c_str(), "w");
    if (bars == nullptr) throw std::runtime_error("mark: cannot write " + opt.bars_out);
    std::fputs("bar_ns,symbol,mid\n", bars);
  }
  const auto close_bars = [&](std::int64_t t) {
    if (bar_end == 0) bar_end = (t / opt.bar_ns + 1) * opt.bar_ns;
    while (t >= bar_end) {
      for (std::uint32_t i = 0; i < mid.size(); ++i) {
        if (std::isnan(mid[i]) || mid[i] == written[i]) continue;
        std::fprintf(
            bars, "%lld,%s,%.10g\n", static_cast<long long>(bar_end), symbols[i].c_str(), mid[i]);
        written[i] = mid[i];
      }
      bar_end += opt.bar_ns;
    }
  };

  while (const EventHeader* h = source.next()) {
    if (h->type != EventType::BookTicker || !h->instrument.valid() ||
        h->instrument.value >= instruments.size())
      continue;
    const auto& m = msg_cast<BookTickerMsg>(h);
    if (!m.bid_px.is_positive() || !m.ask_px.is_positive()) continue;
    const std::int64_t t = venue_ts(*h).ns;
    if (bars != nullptr) close_bars(t);
    const std::uint32_t i = primary[h->instrument.value];
    auto& q = queries[i];
    std::size_t& n = next[i];
    while (n < q.size() && q[n].t < t) {
      if (!std::isnan(mid[i])) marks[q[n].file][q[n].row * columns + q[n].column] = mid[i];
      ++n;
    }
    mid[i] = (m.bid_px.to_double() + m.ask_px.to_double()) / 2;
  }
  if (bars != nullptr) std::fclose(bars);

  for (std::uint32_t f = 0; f < inputs.size(); ++f) {
    const std::string path = inputs[f] + ".marks";
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) throw std::runtime_error("mark: cannot write " + path);
    for (std::size_t k = 0; k < marks[f].size(); ++k) {
      if (!std::isnan(marks[f][k])) std::fprintf(out, "%.10g", marks[f][k]);
      std::fputc((k + 1) % columns == 0 ? '\n' : ',', out);
    }
    std::fclose(out);
  }
  return rows_total;
}

}  // namespace fastmm::bt
