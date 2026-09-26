#include "fastmm/backtest/journal_source.hpp"

#include <stdexcept>

namespace fastmm::bt {

JournalSource::JournalSource(const std::string& path, bool strip_own) {
  auto r = reader_.open(path);
  if (!r) {
    throw std::runtime_error("JournalSource: cannot open " + path + ": " +
                             std::string(to_string(r.error())));
  }
  if (strip_own) {
    const OwnOrderLog log = collect_own_orders(reader_);
    if (!log.live) {
      throw std::runtime_error("JournalSource: strip_own: " + path +
                               " was not recorded by a live session (its feed has no own orders)");
    }
    orders_ = log.orders.size();
    stripper_ = std::make_unique<OwnOrderStripper>(log);
  }
  reader_.for_each([&](const EventHeader* h) {
    if (!is_market_data(h->type) || (h->flags & EventHeader::kOutbound) != 0) return;
    if (md_events_ == 0) first_ts_ = h->exch_ts.valid() ? h->exch_ts : h->recv_ts;
    ++md_events_;
  });
  reader_.reset();
}

const EventHeader* JournalSource::next() {
  for (;;) {
    const EventHeader* h = reader_.next();
    if (h == nullptr) return nullptr;
    if (!is_market_data(h->type) || (h->flags & EventHeader::kOutbound) != 0) continue;
    if (!stripper_) return h;
    if (const EventHeader* s = stripper_->strip(*h, buf_)) return s;
  }
}

std::string JournalSource::note() const {
  if (!stripper_) return {};
  const OwnOrderStripper::Stats& s = stripper_->stats();
  return "journal: own orders stripped (" + std::to_string(orders_) +
         " orders): " + std::to_string(s.levels_adjusted) + " levels reduced, " +
         std::to_string(s.levels_removed) + " removed; " + std::to_string(s.tickers_adjusted) +
         " tickers reduced, " + std::to_string(s.tickers_dropped) + " dropped";
}

void JournalSource::reset() {
  reader_.reset();
}

std::uint64_t write_md_journal(MdSource& source,
                               const std::string& path,
                               const InstrumentTable& instruments,
                               std::uint64_t seed,
                               std::string_view strategy,
                               std::uint64_t max_events) {
  MsgRing ring(1U << 22);
  JournalSessionInfo info;
  info.session_id = seed;
  info.start_ts = source.start_ts();
  info.rng_seed = seed;
  info.strategy = strategy;
  info.instruments = &instruments;
  JournalFileWriter fw(ring, path, info);
  if (!fw.ok()) throw std::runtime_error("write_md_journal: cannot create " + path);
  JournalWriter w(&ring);
  std::uint64_t n = 0;
  source.reset();
  while (max_events == 0 || n < max_events) {
    const EventHeader* h = source.next();
    if (h == nullptr) break;
    if (!w.record(*h)) throw std::runtime_error("write_md_journal: ring full");
    ++n;
    if ((n & 255U) == 0) fw.drain_once();
  }
  fw.drain_once();
  fw.stop();
  source.reset();
  return n;
}

}  // namespace fastmm::bt
