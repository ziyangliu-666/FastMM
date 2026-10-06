#include "fastmm/backtest/journal_state.hpp"

#include "fastmm/core/messages.hpp"

#include <fmt/format.h>

#include <cstring>
#include <vector>

namespace fastmm::bt {

namespace {

// One state of the session being put back together from its records.
struct Assembly {
  std::uint64_t id = 0;
  StrategyStateMsg::Kind kind = StrategyStateMsg::Kind::Restored;
  std::uint8_t flags = 0;
  Timestamp at;
  std::string part[2];
  std::uint32_t total[2] = {0, 0};
  std::uint32_t got[2] = {0, 0};
  bool seen[2] = {false, false};

  void add(const StrategyStateMsg& m) {
    const std::size_t p = m.part == StrategyStateMsg::kPositionsPart ? 1 : 0;
    if (!seen[p]) {
      seen[p] = true;
      total[p] = m.total;
      part[p].assign(m.total, '\0');
    }
    if (m.offset + static_cast<std::uint64_t>(m.bytes) > part[p].size()) return;
    std::memcpy(part[p].data() + m.offset, m.data(), m.bytes);
    got[p] += m.bytes;
  }
  [[nodiscard]] bool complete() const noexcept {
    const bool state = seen[0] && got[0] == total[0];
    if (kind == StrategyStateMsg::Kind::Restored) return state;
    return state && seen[1] && got[1] == total[1];
  }
};

ReconcileMsg position_msg(InstrumentId inst, VenueId venue, Qty qty, Price avg_px, Timestamp at) {
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile, inst, venue);
  m.kind = ReconcileMsg::Kind::Position;
  m.position_qty = qty;
  m.avg_px = avg_px;
  m.hdr.recv_ts = at;
  return m;
}

}  // namespace

sim::SessionStart journal_start_state(std::span<JournalReader> parts) {
  sim::SessionStart out;
  std::optional<Assembly> restored;  // the first complete one
  std::optional<Assembly> snapshot;
  Assembly cur;
  bool in_cur = false;
  // Reconcile Position before the first order, the latest per instrument and account.
  std::vector<ReconcileMsg> reconciled;
  bool ordered = false;
  for (JournalReader& r : parts) {
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) != 0) {
        if (h->type == EventType::OutNewOrder || h->type == EventType::OutReplace) ordered = true;
        return;
      }
      if (h->type == EventType::Reconcile && !ordered) {
        const auto& m = msg_cast<ReconcileMsg>(h);
        if (m.kind != ReconcileMsg::Kind::Position) return;
        for (ReconcileMsg& x : reconciled) {
          if (x.hdr.instrument == m.hdr.instrument && x.hdr.venue == m.hdr.venue) {
            x = m;
            return;
          }
        }
        reconciled.push_back(m);
        return;
      }
      if (h->type != EventType::StrategyState || h->len < sizeof(StrategyStateMsg)) return;
      const auto& m = msg_cast<StrategyStateMsg>(h);
      if (sizeof(StrategyStateMsg) + m.bytes > h->len) return;
      if (!in_cur || cur.id != m.id) {
        cur = Assembly{};
        cur.id = m.id;
        cur.kind = m.kind;
        cur.flags = m.flags;
        cur.at = h->recv_ts;
        in_cur = true;
      }
      cur.add(m);
      if (!cur.complete()) return;
      if (cur.kind == StrategyStateMsg::Kind::Restored) {
        if (!restored) restored = cur;
      } else if (!snapshot) {
        snapshot = cur;
      }
      in_cur = false;
    });
    r.reset();
  }
  const Assembly* use = nullptr;
  if (restored) {
    use = &*restored;
  } else if (snapshot) {
    use = &*snapshot;
  }
  if (use != nullptr) {
    out.has_state = true;
    out.strategy = use->part[0];
  }
  if (use != nullptr && use->kind == StrategyStateMsg::Kind::Snapshot) {
    const std::string& p = use->part[1];
    for (std::size_t off = 0; off + sizeof(StatePosition) <= p.size();
         off += sizeof(StatePosition)) {
      StatePosition s{};
      std::memcpy(&s, p.data() + off, sizeof s);
      out.positions.push_back(position_msg(InstrumentId{s.instrument},
                                           VenueId{s.venue},
                                           Qty::from_raw(s.qty_raw),
                                           Price::from_raw(s.avg_px_raw),
                                           use->at));
    }
    out.note =
        fmt::format("journal: started from the state snapshot of {} ns ({} bytes, {} positions)",
                    use->at.ns,
                    out.strategy.size(),
                    out.positions.size());
    return out;
  }
  for (const ReconcileMsg& m : reconciled) {
    if (!m.position_qty.is_zero()) out.positions.push_back(m);
  }
  if (use != nullptr) {
    out.note = fmt::format(
        "journal: started from the restored state ({} bytes{}) and {} positions reconciled before "
        "the first order",
        out.strategy.size(),
        (use->flags & StrategyStateMsg::kAccepted) != 0 ? ", taken live" : ", refused live",
        out.positions.size());
  } else {
    out.note = fmt::format(
        "journal: no strategy state recorded; {} positions reconciled before the first order",
        out.positions.size());
  }
  return out;
}

std::optional<std::string> restored_state(JournalReader& reader) {
  std::optional<std::string> out;
  Assembly cur;
  bool done = false;
  reader.for_each([&](const EventHeader* h) {
    if (done || h->type != EventType::StrategyState || h->len < sizeof(StrategyStateMsg)) return;
    const auto& m = msg_cast<StrategyStateMsg>(h);
    if (m.kind != StrategyStateMsg::Kind::Restored || sizeof(StrategyStateMsg) + m.bytes > h->len)
      return;
    if (cur.id != m.id) {
      cur = Assembly{};
      cur.id = m.id;
      cur.kind = m.kind;
    }
    cur.add(m);
    if (cur.complete()) {
      out = cur.part[0];
      done = true;
    }
  });
  reader.reset();
  return out;
}

}  // namespace fastmm::bt
