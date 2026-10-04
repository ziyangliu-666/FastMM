#include "fastmm/backtest/journal_source.hpp"

#include "fastmm/core/log.hpp"

#include <fmt/format.h>

#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace fastmm::bt {

void JournalSource::open_part(const std::string& path) {
  JournalReader r;
  if (auto res = r.open(path); !res) {
    throw std::runtime_error("JournalSource: cannot open " + path + ": " +
                             std::string(to_string(res.error())));
  }
  parts_.push_back(std::move(r));
  paths_.push_back(path);
}

JournalSource::JournalSource(const std::string& path,
                             bool strip_own,
                             bool parts,
                             const InstrumentTable* remap)
    : remap_(remap != nullptr) {
  open_part(path);
  if (remap != nullptr) {
    // Every part repeats the first's instrument table, so one map serves them all.
    for (const Instrument& j : parts_.front().instruments()) {
      const Instrument* c = remap->find(j.venue, j.symbol.view());
      for (const Instrument* x = remap->begin(); c == nullptr && x != remap->end(); ++x) {
        if (x->symbol.view() == j.symbol.view()) c = x;
      }
      if (c == nullptr) ++unmapped_;
      map_.push_back(c == nullptr ? kUnmapped : c->id.value);
      map_venue_.push_back(c == nullptr ? VenueId{} : c->venue);
    }
    rbuf_.resize((kMaxMsgBytes + 7) / 8);
  }
  for (std::uint64_t n = 1; parts; ++n) {
    const std::string next_path = JournalFileWriter::part_path(path, n);
    if (!std::filesystem::exists(next_path)) break;
    open_part(next_path);
    const JournalReader& prev = parts_[parts_.size() - 2];
    JournalReader& cur = parts_.back();
    if (cur.header().session_id != prev.header().session_id) {
      throw std::runtime_error(
          fmt::format("JournalSource: {} is not a part of the session of {} (session {}, not {})",
                      next_path,
                      path,
                      cur.header().session_id,
                      prev.header().session_id));
    }
    const EventHeader* first = cur.next();
    cur.reset();
    if (first != nullptr && first->seq != prev.last_seq() + 1) {
      if (seq_gaps_++ == 0) {
        gap_ = paths_[paths_.size() - 2] + " ends at seq " + std::to_string(prev.last_seq()) +
               ", " + next_path + " starts at " + std::to_string(first->seq);
      }
      FASTMM_LOG_WARN("journal: {}: a part is missing between them, or the first was cut short",
                      gap_);
    }
  }
  if (strip_own) {
    const OwnOrderLog log = collect_own_orders(parts_);
    if (!log.live) {
      throw std::runtime_error("JournalSource: strip_own: " + path +
                               " was not recorded by a live session (its feed has no own orders)");
    }
    orders_ = log.orders.size();
    stripper_ = std::make_unique<OwnOrderStripper>(parts_);
  }
  // Per venue: no snapshot seen yet, in the first one, done.
  constexpr std::uint8_t kNotYet = 0;
  constexpr std::uint8_t kIn = 1;
  constexpr std::uint8_t kDone = 2;
  std::uint8_t state[kMaxVenues] = {};
  std::size_t index[kMaxVenues] = {};
  for (JournalReader& r : parts_) {
    r.for_each([&](const EventHeader* h) {
      if (h->type == EventType::Balance && h->venue.value < kMaxVenues &&
          state[h->venue.value] != kDone) {
        const auto& m = msg_cast<BalanceMsg>(h);
        if ((m.flags & (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd)) == 0) return;
        std::uint8_t& st = state[h->venue.value];
        if (st == kNotYet) {
          st = kIn;
          index[h->venue.value] = balances_.size();
          balances_.push_back(sim::SimAccountConfig{h->venue, {}});
        }
        sim::SimAccountConfig& a = balances_[index[h->venue.value]];
        if ((m.flags & BalanceMsg::kAccount) == 0 && !m.asset.empty())
          a.balances.push_back(sim::SimBalance{m.asset, m.total});
        if ((m.flags & BalanceMsg::kSnapshotEnd) != 0) st = kDone;
        return;
      }
      if (!is_market_data(h->type) || (h->flags & EventHeader::kOutbound) != 0) return;
      if (md_events_ == 0) first_ts_ = h->exch_ts.valid() ? h->exch_ts : h->recv_ts;
      ++md_events_;
    });
    r.reset();
  }
}

const EventHeader* JournalSource::next() {
  while (at_ < parts_.size()) {
    const EventHeader* h = parts_[at_].next();
    if (h == nullptr) {
      ++at_;
      continue;
    }
    if (!is_market_data(h->type) || (h->flags & EventHeader::kOutbound) != 0) continue;
    if (stripper_) {
      h = stripper_->strip(*h, buf_);
      if (h == nullptr) continue;
    }
    if (!remap_) return h;
    const std::uint32_t to =
        h->instrument.value < map_.size() ? map_[h->instrument.value] : kUnmapped;
    if (to == kUnmapped) {
      ++dropped_;
      continue;
    }
    // The configured id and venue, in a copy: the reader's record is read-only.
    FASTMM_CHECK(h->len <= kMaxMsgBytes);
    std::memcpy(rbuf_.data(), h, h->len);
    auto* out = reinterpret_cast<EventHeader*>(rbuf_.data());
    out->instrument = InstrumentId{to};
    out->venue = map_venue_[h->instrument.value];
    return out;
  }
  return nullptr;
}

std::string JournalSource::note() const {
  std::string s;
  if (parts_.size() > 1) {
    s = "journal: " + std::to_string(parts_.size()) + " parts, " + paths_.front() + " to " +
        paths_.back();
    if (seq_gaps_ != 0) {
      s += "; WARNING: the sequence numbers break " + std::to_string(seq_gaps_) +
           (seq_gaps_ == 1 ? " time (" : " times (first: ") + gap_ + ")";
    }
  }
  if (stripper_) {
    if (!s.empty()) s += '\n';
    const OwnOrderStripper::Stats& st = stripper_->stats();
    s += "journal: own orders stripped (" + std::to_string(orders_) +
         " orders): " + std::to_string(st.levels_adjusted) + " levels reduced, " +
         std::to_string(st.levels_removed) + " removed; " + std::to_string(st.tickers_adjusted) +
         " tickers reduced, " + std::to_string(st.tickers_dropped) + " dropped";
  }
  if (remap_) {
    if (!s.empty()) s += '\n';
    s += "journal: remap: " + std::to_string(map_.size() - unmapped_) + " of " +
         std::to_string(map_.size()) + " instruments in the configuration, " +
         std::to_string(dropped_) + (dropped_ == 1 ? " event dropped" : " events dropped");
  }
  return s;
}

void JournalSource::reset() {
  for (JournalReader& r : parts_) r.reset();
  at_ = 0;
  dropped_ = 0;
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
