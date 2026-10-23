#include "fastmm/backtest/journal_source.hpp"

#include "fastmm/backtest/journal_state.hpp"
#include "fastmm/backtest/recorded_latency.hpp"
#include "fastmm/core/log.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fastmm::bt {

JournalSource::JournalSource(const std::string& path,
                             bool strip_own,
                             bool parts,
                             const InstrumentTable* remap)
    : remap_(remap != nullptr) {
  // The parts: `path`, then each next one that exists. Opening one reads all of it (it checks
  // every block's checksum), so they are opened at once: on a slow file system (a Windows drive
  // under WSL) reading them side by side takes a fraction of reading them in turn. The checks
  // between parts then run in order, as they would have one part at a time.
  std::vector<std::string> paths{path};
  for (std::uint64_t n = 1; parts; ++n) {
    std::string next_path = JournalFileWriter::part_path(path, n);
    if (!std::filesystem::exists(next_path)) break;
    paths.push_back(std::move(next_path));
  }
  std::vector<JournalReader> readers(paths.size());
  std::vector<std::optional<JournalError>> failed(paths.size());
  {
    std::atomic<std::size_t> todo{0};
    const auto open_parts = [&] {
      for (std::size_t k = todo++; k < paths.size(); k = todo++) {
        if (auto res = readers[k].open(paths[k]); !res) failed[k] = res.error();
      }
    };
    std::vector<std::thread> helpers;
    const std::size_t n_helpers = std::min<std::size_t>(paths.size(), 8) - 1;
    helpers.reserve(n_helpers);
    for (std::size_t k = 0; k < n_helpers; ++k) helpers.emplace_back(open_parts);
    open_parts();
    for (std::thread& t : helpers) t.join();
  }
  for (std::size_t k = 0; k < paths.size(); ++k) {
    if (failed[k]) {
      throw std::runtime_error("JournalSource: cannot open " + paths[k] + ": " +
                               readers[k].describe(*failed[k]));
    }
    parts_.push_back(std::move(readers[k]));
    paths_.push_back(paths[k]);
    if (k == 0) continue;
    const std::string& next_path = paths[k];
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
  if (strip_own) {
    const OwnOrderLog log = collect_own_orders(parts_);
    if (!log.live) {
      throw std::runtime_error("JournalSource: strip_own: " + path +
                               " was not recorded by a live session (its feed has no own orders)");
    }
    orders_ = log.orders.size();
    stripper_ = std::make_unique<OwnOrderStripper>(parts_);
  }
  RecordedLatencyCollector latency;
  for (JournalReader& r : parts_) {
    r.for_each([&](const EventHeader* h) {
      latency.on_event(*h);
      if ((h->flags & EventHeader::kOutbound) != 0) return;
      if (h->type == EventType::Balance && h->len >= sizeof(BalanceMsg)) {
        const auto& m = msg_cast<BalanceMsg>(h);
        if ((m.flags & BalanceMsg::kAccount) == 0) session_.balances.push_back(m);
        return;
      }
      if (h->type == EventType::AccountFlow && h->len >= sizeof(AccountFlowMsg)) {
        session_.flows.push_back(msg_cast<AccountFlowMsg>(h));
        return;
      }
      if (h->type == EventType::ParamUpdate && h->len >= sizeof(ParamUpdateMsg)) {
        session_.params.updates.push_back(msg_cast<ParamUpdateMsg>(h));
        return;
      }
      if (!is_market_data(h->type)) return;
      if (md_events_ == 0) first_ts_ = h->exch_ts.valid() ? h->exch_ts : h->recv_ts;
      ++md_events_;
    });
    r.reset();
  }
  for (const JournalParam& p : parts_.front().params())
    session_.params.table.emplace_back(std::string(p.name), p.type);
  session_.latency = latency.finish();
  read_states(parts_, session_);
  if (remap_) {
    const auto remapped = [&](std::vector<ReconcileMsg>& v) {
      std::vector<ReconcileMsg> kept;
      for (ReconcileMsg m : v) {
        const std::uint32_t j = m.hdr.instrument.value;
        if (j >= map_.size() || map_[j] == kUnmapped) continue;
        m.hdr.instrument = InstrumentId{map_[j]};
        m.hdr.venue = map_venue_[j];
        kept.push_back(m);
      }
      v = std::move(kept);
    };
    remapped(session_.reconciled);
    if (session_.restored) remapped(session_.restored->positions);
    for (sim::Checkpoint& k : session_.checkpoints) {
      remapped(k.positions);
      std::vector<StateOrder> kept;
      for (StateOrder o : k.orders) {
        if (o.instrument >= map_.size() || map_[o.instrument] == kUnmapped) continue;
        o.instrument = map_[o.instrument];
        kept.push_back(o);
      }
      k.orders = std::move(kept);
    }
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
