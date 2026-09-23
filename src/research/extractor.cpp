#include "fastmm/research/extractor.hpp"

#include "fastmm/core/book/book_features.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"

#include <algorithm>
#include <stdexcept>

namespace fastmm::research {

namespace {

constexpr std::size_t kReserveRows = 1U << 16;

std::int64_t event_time(const EventHeader& h) noexcept {
  return (h.exch_ts.valid() ? h.exch_ts : h.recv_ts).ns;
}

std::vector<std::int64_t> normalize(const std::vector<Duration>& in) {
  std::vector<std::int64_t> out;
  if (in.empty()) {
    out.assign(std::begin(kDefaultHorizonsNs), std::end(kDefaultHorizonsNs));
    return out;
  }
  for (Duration d : in) {
    if (d.ns > 0) out.push_back(d.ns);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

std::vector<Duration> default_horizons() {
  std::vector<Duration> out;
  out.reserve(std::size(kDefaultHorizonsNs));
  for (std::int64_t ns : kDefaultHorizonsNs) out.push_back(Duration{ns});
  return out;
}

FeatureTable extract_features(MdSource& source, const FeatureConfig& cfg) {
  if (cfg.instruments == 0) throw std::invalid_argument("features: instruments must be > 0");
  if (cfg.imbalance_levels == 0)
    throw std::invalid_argument("features: imbalance_levels must be > 0");

  const std::vector<std::int64_t> horizons = normalize(cfg.horizons);
  FeatureTable table;
  table.coverage.resize(horizons.size());
  for (std::size_t j = 0; j < horizons.size(); ++j) table.coverage[j].horizon_ns = horizons[j];
  FeatureRows& rows = table.rows;
  rows.horizon_ns = horizons;
  rows.forward_mid.resize(horizons.size());
  rows.reserve(kReserveRows);

  const auto n_inst = static_cast<std::size_t>(cfg.instruments);
  std::vector<L2Book<256>> books(n_inst);
  std::vector<std::int64_t> last_row_ts(n_inst, 0);
  std::vector<bool> have_row(n_inst, false);
  std::vector<std::size_t> next(horizons.size(), 0);  // per horizon, the first unresolved row

  // The mid of the row's instrument at the deadline. 0 (the book lost a side) is recorded as
  // unset, exactly like a horizon past the end of the data: never a substitute price.
  const auto record = [&](std::size_t j, std::size_t i) {
    const std::int64_t m = books[rows.instrument[i]].mid().raw;
    rows.forward_mid[j][i] = m;
    if (m == 0) {
      ++table.coverage[j].excluded_no_mid;
    } else {
      ++table.coverage[j].resolved;
    }
  };
  // Rows whose horizon fell strictly before `t`. The book holds every event at or before the
  // previous event time, and no event lies between that and the deadline, so this is the book
  // at the deadline.
  const auto resolve_before = [&](std::int64_t t) {
    for (std::size_t j = 0; j < horizons.size(); ++j) {
      while (next[j] < rows.size() && rows.ts[next[j]] + horizons[j] < t) record(j, next[j]++);
    }
  };

  const auto emit = [&](std::uint32_t inst, std::int64_t t) {
    const L2Book<256>& book = books[inst];
    if (book.depth(Side::Buy) == 0 || book.depth(Side::Sell) == 0) {
      ++table.skipped_one_sided;
      return;
    }
    if (cfg.subsample.ns > 0 && have_row[inst] && t - last_row_ts[inst] < cfg.subsample.ns) {
      ++table.skipped_subsample;
      return;
    }
    last_row_ts[inst] = t;
    have_row[inst] = true;
    const Level bb = book.best_bid();
    const Level ba = book.best_ask();
    rows.ts.push_back(t);
    rows.instrument.push_back(inst);
    rows.mid.push_back(book.mid().raw);
    rows.microprice.push_back(microprice(book).raw);
    rows.best_bid.push_back(bb.price.raw);
    rows.best_ask.push_back(ba.price.raw);
    rows.bid_qty.push_back(bb.qty.raw);
    rows.ask_qty.push_back(ba.qty.raw);
    rows.imbalance.push_back(imbalance(book, cfg.imbalance_levels).raw);
    rows.spread.push_back(book.spread().raw);
    for (std::vector<std::int64_t>& col : rows.forward_mid) col.push_back(0);
  };

  bool any = false;
  std::int64_t last_ts = 0;
  while (const EventHeader* h = source.next()) {
    const std::int64_t t = event_time(*h);
    ++table.events;
    resolve_before(t);
    if (!any) {
      table.start_ts = t;
      any = true;
    }
    last_ts = t;
    const std::uint32_t inst = h->instrument.value;
    if (inst >= n_inst) continue;
    switch (h->type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
        books[inst].apply_delta(msg_cast<BookDeltaMsg>(h));
        ++table.book_updates;
        if (cfg.sample_book_updates) emit(inst, t);
        break;
      case EventType::Trade:
        if (cfg.sample_trades) emit(inst, t);
        break;
      default:
        break;
    }
  }
  table.end_ts = last_ts;
  // The clock stops at the last event, so a horizon that lands on it is measured and anything
  // beyond it is not.
  for (std::size_t j = 0; j < horizons.size(); ++j) {
    while (next[j] < rows.size() && rows.ts[next[j]] + horizons[j] <= last_ts) record(j, next[j]++);
    table.coverage[j].excluded_past_end = rows.size() - next[j];
  }
  return table;
}

}  // namespace fastmm::research
