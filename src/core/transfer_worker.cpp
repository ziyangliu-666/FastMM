#include "fastmm/core/transfer_worker.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/session_state.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace fastmm {

namespace {

constexpr std::string_view kLedgerHead = "fastmm-transfers 1";

}  // namespace

std::string lane_client_id(VenueId primary, std::uint64_t seq) {
  FixedString<31> s;
  lane_client_id(s, primary, seq);
  return std::string(s.view());
}

std::size_t TransferWorker::pool_of(VenueId primary) const noexcept {
  for (std::size_t i = 0; i < pools_.size(); ++i) {
    if (pools_[i].primary == primary) return i;
  }
  return pools_.size();
}

// ---- ledger -------------------------------------------------------------------------------------
//
//   fastmm-transfers 1
//   <seq> <client id> <from account> <to account> <asset> <amount raw> <created ms> <answered 0|1>

std::string TransferWorker::serialize(std::size_t i) const {
  const LanePool& p = pools_[i];
  std::string out(kLedgerHead);
  out += '\n';
  const auto name = [&](VenueId v) -> std::string_view {
    for (std::size_t k = 0; k < p.members.size(); ++k) {
      if (p.members[k] == v) return p.names[k];
    }
    return "?";
  };
  for (const Entry& e : state_[i].entries) {
    fmt::format_to(std::back_inserter(out),
                   "{} {} {} {} {} {} {} {}\n",
                   e.seq,
                   e.req.client_id,
                   name(e.req.from),
                   name(e.req.to),
                   e.req.asset,
                   e.req.amount.raw,
                   e.req.created_ms,
                   e.answered ? 1 : 0);
  }
  return out;
}

Result<std::vector<TransferWorker::Entry>, std::string> TransferWorker::parse(
    const LanePool& p, std::string_view text) {
  std::vector<Entry> out;
  std::istringstream in{std::string(text)};
  std::string line;
  if (!std::getline(in, line)) return out;
  if (line != kLedgerHead) return fail(fmt::format("not a transfer ledger: '{}'", line));
  const auto id_of = [&](const std::string& name) -> VenueId {
    for (std::size_t k = 0; k < p.members.size(); ++k) {
      if (p.names[k] == name) return p.members[k];
    }
    return VenueId{};
  };
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    Entry e;
    std::string from;
    std::string to;
    std::int64_t raw = 0;
    int answered = 0;
    if (!(ls >> e.seq >> e.req.client_id >> from >> to >> e.req.asset >> raw >> e.req.created_ms >>
          answered))
      return fail(fmt::format("transfer ledger: cannot read '{}'", line));
    e.req.from = id_of(from);
    e.req.to = id_of(to);
    if (!e.req.from.valid() || !e.req.to.valid())
      return fail(fmt::format("transfer ledger: '{}' names an account outside the pool", line));
    e.req.amount = Notional::from_raw(raw);
    e.answered = answered != 0;
    out.push_back(std::move(e));
  }
  return out;
}

Result<void, std::string> TransferWorker::open() {
  for (std::size_t i = 0; i < pools_.size(); ++i) {
    const LanePool& p = pools_[i];
    state_[i] = PoolState{};
    if (p.cfg.state_file.empty()) continue;
    std::ifstream f(p.cfg.state_file);
    if (!f.good()) continue;  // no ledger: nothing in flight
    std::stringstream ss;
    ss << f.rdbuf();
    auto r = parse(p, ss.str());
    if (!r) return fail(p.cfg.state_file + ": " + r.error());
    state_[i].entries = std::move(*r);
    if (!state_[i].entries.empty()) {
      FASTMM_LOG_WARN(
          "[transfers {}] {} transfer(s) of an earlier process in flight: held again "
          "and asked for",
          p.names[0],
          state_[i].entries.size());
    }
  }
  count();
  return {};
}

void TransferWorker::persist(std::size_t p) {
  const LanePool& pool = pools_[p];
  if (pool.cfg.state_file.empty()) return;
  if (auto r = write_file_atomic(pool.cfg.state_file, serialize(p)); !r) {
    ++stats_.errors;
    FASTMM_LOG_ERROR("[transfers {}] ledger {}: {}", pool.names[0], pool.cfg.state_file, r.error());
  }
}

void TransferWorker::count() {
  std::uint32_t n = 0;
  for (const PoolState& s : state_) n += static_cast<std::uint32_t>(s.entries.size());
  stats_.in_flight = n;
}

void TransferWorker::record(std::size_t p,
                            TreasuryEventKind kind,
                            const Entry& e,
                            const TransferResult* r) const {
  TreasuryEvent ev{kind, &e.req, r, e.seq, true, true};
  pools_[p].port->record(ev);
}

// ---- step ---------------------------------------------------------------------------------------

void TransferWorker::step(std::int64_t now_ns, const Pull& pull) {
  for (std::size_t p = 0; p < pools_.size(); ++p) {
    PoolState& s = state_[p];
    if (s.reheld) continue;
    s.reheld = true;
    for (Entry& e : s.entries) {
      record(p, TreasuryEventKind::Reserve, e, nullptr);
      e.next_poll_ns = now_ns;
    }
  }
  LaneIntent in;
  while (pull && pull(in)) {
    const std::size_t p = pool_of(in.primary);
    if (p == pools_.size()) continue;
    if (in.kind == LaneIntent::Kind::Send) {
      send(p, in, now_ns);
      continue;
    }
    std::vector<Entry>& es = state_[p].entries;
    for (std::size_t k = 0; k < es.size(); ++k) {
      if (es[k].seq == in.seq) {
        ++stats_.done;
        drop(p, k);
        break;
      }
    }
  }
  for (std::size_t p = 0; p < pools_.size(); ++p) {
    std::vector<Entry>& es = state_[p].entries;
    for (std::size_t k = 0; k < es.size();) {
      const std::size_t before = es.size();
      if (es[k].next_poll_ns != 0 && now_ns >= es[k].next_poll_ns) poll(p, k, now_ns);
      if (es.size() == before) ++k;
    }
  }
  count();
}

void TransferWorker::send(std::size_t p, const LaneIntent& in, std::int64_t now_ns) {
  const LanePool& pool = pools_[p];
  Entry e;
  e.seq = in.seq;
  e.req.client_id = lane_client_id(in.primary, in.seq);
  e.req.from = in.from;
  e.req.to = in.to;
  e.req.asset = std::string(in.asset.view());
  e.req.amount = Notional::from_raw(in.amount);
  e.req.created_ms = now_ns / 1'000'000;
  std::vector<Entry>& es = state_[p].entries;
  es.push_back(e);
  persist(p);  // written before it is sent: a crash in between is asked for, never sent twice
  const TransferResult r = pool.port->submit(e.req);
  ++stats_.sent;
  Entry& cur = es.back();
  switch (r.state) {
    case TransferState::Failed:
      ++stats_.failed;
      FASTMM_LOG_WARN("[transfers {}] {} {} {} -> {} refused: {}",
                      pool.names[0],
                      e.req.client_id,
                      e.req.asset,
                      e.req.from.value,
                      e.req.to.value,
                      r.detail);
      record(p, TreasuryEventKind::Failed, cur, &r);
      drop(p, es.size() - 1);
      return;
    case TransferState::Done:
      record(p, TreasuryEventKind::Sent, cur, &r);
      record(p, TreasuryEventKind::Done, cur, &r);
      ++stats_.done;
      drop(p, es.size() - 1);
      return;
    case TransferState::Unknown:
    case TransferState::NotFound:
      ++stats_.unknown;
      FASTMM_LOG_WARN("[transfers {}] {} no answer ({}): asking for it in {} s",
                      pool.names[0],
                      e.req.client_id,
                      r.detail,
                      pool.cfg.status_interval.ns / 1'000'000'000);
      {
        TransferResult u = r;
        u.state = TransferState::Unknown;
        record(p, TreasuryEventKind::Sent, cur, &u);
      }
      cur.next_poll_ns = now_ns + pool.cfg.status_interval.ns;
      persist(p);
      return;
    case TransferState::Pending:
      record(p, TreasuryEventKind::Sent, cur, &r);
      cur.answered = true;
      cur.next_poll_ns = now_ns + pool.cfg.status_interval.ns;
      persist(p);
      return;
  }
}

void TransferWorker::poll(std::size_t p, std::size_t k, std::int64_t now_ns) {
  const LanePool& pool = pools_[p];
  Entry& e = state_[p].entries[k];
  const TransferResult r = pool.port->status(e.req);
  ++stats_.queries;
  const std::int64_t age_ns = now_ns - (e.req.created_ms * 1'000'000);
  switch (r.state) {
    case TransferState::Done:
      record(p, TreasuryEventKind::Done, e, &r);
      ++stats_.done;
      drop(p, k);
      return;
    case TransferState::Failed:
      record(p, TreasuryEventKind::Failed, e, &r);
      ++stats_.failed;
      drop(p, k);
      return;
    case TransferState::NotFound:
      if (age_ns >= pool.cfg.timeout.ns) {
        TransferResult f = r;
        f.state = TransferState::Failed;
        f.detail = "the venue has no record of it";
        FASTMM_LOG_WARN("[transfers {}] {}: {} after {} s: failed",
                        pool.names[0],
                        e.req.client_id,
                        f.detail,
                        age_ns / 1'000'000'000);
        record(p, TreasuryEventKind::Failed, e, &f);
        ++stats_.failed;
        drop(p, k);
        return;
      }
      break;
    case TransferState::Pending:
    case TransferState::Unknown:
      break;
  }
  e.next_poll_ns = now_ns + pool.cfg.status_interval.ns;
}

void TransferWorker::drop(std::size_t p, std::size_t k) {
  std::vector<Entry>& es = state_[p].entries;
  es.erase(es.begin() + static_cast<std::ptrdiff_t>(k));
  persist(p);
}

std::int64_t TransferWorker::next_due_ns() const noexcept {
  std::int64_t next = std::numeric_limits<std::int64_t>::max();
  for (const PoolState& s : state_) {
    if (!s.reheld && !s.entries.empty()) return 0;
    for (const Entry& e : s.entries) {
      if (e.next_poll_ns != 0) next = std::min(next, e.next_poll_ns);
    }
  }
  return next;
}

}  // namespace fastmm
