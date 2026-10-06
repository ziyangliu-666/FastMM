#pragma once
// The state a recorded session started from, for a backtest or a replay over its journal.
//
// The engine writes StrategyStateMsg records (core/messages.hpp): the bytes it handed to the
// strategy's restore() at the start (Kind::Restored), and with [strategy]
// state_snapshot_interval_s the strategy's state() and the positions every so often
// (Kind::Snapshot). journal_start_state() takes the first restored state of the parts, else their
// first snapshot. The positions are the snapshot's; with the restored state, or with no state
// record at all, they are the Reconcile Position messages the session received before its first
// order: what it restored from its store and what the venues reported at the start. A journal
// without any of these yields nothing (has_state false, no positions).
//
// restored_state() is the Restored bytes alone: what a replay hands to restore() to repeat the
// session exactly.
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/md_source.hpp"

#include <optional>
#include <span>
#include <string>

namespace fastmm::bt {

[[nodiscard]] sim::SessionStart journal_start_state(std::span<JournalReader> parts);

// The bytes the session's strategy was handed to restore() at the start; none when the journal
// does not record them. Resets the reader.
[[nodiscard]] std::optional<std::string> restored_state(JournalReader& reader);

}  // namespace fastmm::bt
