#pragma once
// The states a recorded session wrote into its journal, for a backtest or a replay over it.
//
// The engine writes StrategyStateMsg records (core/messages.hpp): the bytes it handed to the
// strategy's restore() at the start (Kind::Restored), and its checkpoints (Kind::Snapshot): every
// [engine] checkpoint_s, and at a warm standby's take-over, the strategy's state(), the positions
// and (format v4) the open orders. One state's records are written back to back; StateAssembler
// puts each one together as a reader walks the journal, so a journal of many checkpoints is read
// once.
//
// read_states() fills a RecordedSession's restored state, checkpoints and the Reconcile Position
// messages received before the first order. restored_state() is the Restored bytes alone, what a
// replay hands to restore() to repeat the session; take_over_states() the checkpoint written at
// each ControlCommand::TakeOver, what a replay stages for it (Engine::stage_strategy_state).
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/recorded_session.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fastmm::bt {

// One state as the journal recorded it.
struct RecordedState {
  std::uint64_t id = 0;
  StrategyStateMsg::Kind kind = StrategyStateMsg::Kind::Restored;
  std::uint8_t flags = 0;
  Timestamp at;
  bool take_over = false;  // its records follow a TakeOver control record
  std::string strategy;
  std::vector<StatePosition> positions;
  std::vector<StateOrder> orders;
};

class StateAssembler {
 public:
  // Takes the journal's records in order. Returns the state the record completed (the records of
  // a state end at the first record that is not one of them), valid until the next call; null
  // when it completed none.
  const RecordedState* add(const EventHeader& h);
  // The state still being read at the end of the journal, if complete.
  const RecordedState* finish();

 private:
  bool close();
  struct Part {
    std::string bytes;
    std::uint32_t total = 0;
    std::uint32_t got = 0;
    bool seen = false;
  };
  RecordedState cur_;
  RecordedState done_;
  Part parts_[3];
  bool open_ = false;
  bool after_take_over_ = false;
};

// The restored state, the checkpoints and the positions reconciled before the first order of the
// parts, in order (resets each reader).
void read_states(std::span<JournalReader> parts, sim::RecordedSession& out);

// The bytes the session's strategy was handed to restore() at the start; none when the journal
// does not record them. Resets the reader.
[[nodiscard]] std::optional<std::string> restored_state(JournalReader& reader);

// The strategy bytes of the checkpoint written at each take-over, in order. Resets the reader.
[[nodiscard]] std::vector<std::string> take_over_states(JournalReader& reader);

}  // namespace fastmm::bt
