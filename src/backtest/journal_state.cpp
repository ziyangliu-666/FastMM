#include "fastmm/backtest/journal_state.hpp"

#include "fastmm/core/messages.hpp"

#include <cstring>
#include <vector>

namespace fastmm::bt {

namespace {

ReconcileMsg position_msg(const StatePosition& s, Timestamp at) {
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile, InstrumentId{s.instrument}, VenueId{s.venue});
  m.kind = ReconcileMsg::Kind::Position;
  m.position_qty = Qty::from_raw(s.qty_raw);
  m.avg_px = Price::from_raw(s.avg_px_raw);
  m.hdr.recv_ts = at;
  return m;
}

template <class T>
std::vector<T> entries(const std::string& bytes) {
  std::vector<T> out(bytes.size() / sizeof(T));
  if (!out.empty()) std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
  return out;
}

sim::Checkpoint checkpoint_of(const RecordedState& s) {
  sim::Checkpoint c;
  c.at = s.at;
  c.accepted = (s.flags & StrategyStateMsg::kAccepted) != 0;
  c.strategy = s.strategy;
  for (const StatePosition& p : s.positions) c.positions.push_back(position_msg(p, s.at));
  c.orders = s.orders;
  return c;
}

}  // namespace

const RecordedState* StateAssembler::add(const EventHeader& h) {
  const bool state = h.type == EventType::StrategyState && h.len >= sizeof(StrategyStateMsg) &&
                     sizeof(StrategyStateMsg) + msg_cast<StrategyStateMsg>(&h).bytes <= h.len &&
                     msg_cast<StrategyStateMsg>(&h).part <= StrategyStateMsg::kOrdersPart;
  const RecordedState* completed = nullptr;
  if (open_ && (!state || msg_cast<StrategyStateMsg>(&h).id != cur_.id)) {
    if (close()) completed = &done_;
  }
  if (!state) {
    after_take_over_ = h.type == EventType::Control && (h.flags & EventHeader::kOutbound) == 0 &&
                       msg_cast<ControlMsg>(&h).command == ControlCommand::TakeOver;
    return completed;
  }
  const auto& m = msg_cast<StrategyStateMsg>(&h);
  if (!open_) {
    open_ = true;
    cur_ = RecordedState{};
    cur_.id = m.id;
    cur_.kind = m.kind;
    cur_.flags = m.flags;
    cur_.at = h.recv_ts;
    cur_.take_over = after_take_over_;
    for (Part& p : parts_) p = Part{};
  }
  after_take_over_ = false;
  Part& p = parts_[m.part];
  if (!p.seen) {
    p.seen = true;
    p.total = m.total;
    p.bytes.assign(m.total, '\0');
  }
  if (m.offset + static_cast<std::uint64_t>(m.bytes) <= p.bytes.size()) {
    std::memcpy(p.bytes.data() + m.offset, m.data(), m.bytes);
    p.got += m.bytes;
  }
  return completed;
}

const RecordedState* StateAssembler::finish() {
  return open_ && close() ? &done_ : nullptr;
}

bool StateAssembler::close() {
  open_ = false;
  const auto whole = [](const Part& p) { return p.seen && p.got == p.total; };
  const Part& state = parts_[StrategyStateMsg::kStatePart];
  const Part& positions = parts_[StrategyStateMsg::kPositionsPart];
  const Part& orders = parts_[StrategyStateMsg::kOrdersPart];
  if (!whole(state)) return false;
  if (cur_.kind == StrategyStateMsg::Kind::Snapshot &&
      (!whole(positions) || (orders.seen && !whole(orders))))
    return false;
  done_ = std::move(cur_);
  done_.strategy = state.bytes;
  done_.positions = entries<StatePosition>(positions.bytes);
  done_.orders = entries<StateOrder>(orders.bytes);
  return true;
}

void read_states(std::span<JournalReader> parts, sim::RecordedSession& out) {
  StateAssembler a;
  bool ordered = false;
  const auto take = [&](const RecordedState* s) {
    if (s == nullptr) return;
    if (s->kind == StrategyStateMsg::Kind::Restored) {
      if (!out.restored) out.restored = checkpoint_of(*s);
    } else {
      out.checkpoints.push_back(checkpoint_of(*s));
    }
  };
  for (JournalReader& r : parts) {
    r.for_each([&](const EventHeader* h) {
      take(a.add(*h));
      if ((h->flags & EventHeader::kOutbound) != 0) {
        if (h->type == EventType::OutNewOrder || h->type == EventType::OutReplace) ordered = true;
        return;
      }
      if (h->type != EventType::Reconcile || ordered) return;
      const auto& m = msg_cast<ReconcileMsg>(h);
      if (m.kind != ReconcileMsg::Kind::Position) return;
      for (ReconcileMsg& x : out.reconciled) {
        if (x.hdr.instrument == m.hdr.instrument && x.hdr.venue == m.hdr.venue) {
          x = m;
          return;
        }
      }
      out.reconciled.push_back(m);
    });
    r.reset();
  }
  take(a.finish());
}

std::optional<std::string> restored_state(JournalReader& reader) {
  std::optional<std::string> out;
  StateAssembler a;
  const auto take = [&](const RecordedState* s) {
    if (!out && s != nullptr && s->kind == StrategyStateMsg::Kind::Restored) out = s->strategy;
  };
  reader.for_each([&](const EventHeader* h) { take(a.add(*h)); });
  take(a.finish());
  reader.reset();
  return out;
}

std::vector<std::string> take_over_states(JournalReader& reader) {
  std::vector<std::string> out;
  StateAssembler a;
  const auto take = [&](const RecordedState* s) {
    if (s != nullptr && s->take_over) out.push_back(s->strategy);
  };
  reader.for_each([&](const EventHeader* h) { take(a.add(*h)); });
  take(a.finish());
  reader.reset();
  return out;
}

}  // namespace fastmm::bt
