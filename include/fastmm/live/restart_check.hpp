#pragma once
// What a restarting fastmm-live session carries over, checked against the store before it trades.
//
// Three things travel from one session to the next: the epoch file (client order ids), the kill
// file (a latched max-loss trip and the loss budget spent) and the store (positions and where each
// venue's execution replay resumes). When the files are gone and the store is not, a new epoch
// counter would reuse the ids of the sessions the store records and a forgotten trip would trade
// again; and a store whose newest session booked executions twice restores a position that counts
// them twice. Each case is refused with the exit code and the message an operator acts on
// (docs/how-to/operations/runbook.md#a-start-refuses-the-state-it-carries-over).
#include "fastmm/store/reader.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::live {

// exit_code 0: nothing to refuse.
struct RestartRefusal {
  int exit_code = 0;
  std::string message;  // for stderr, without the program name or a trailing newline
  explicit operator bool() const noexcept { return exit_code != 0; }
};

// [engine] kill_file did not exist at start and --clear-kill was not given: refused with
// kExitKilled when the store's newest session ended on a max-loss trip
// (Recovery::max_loss_tripped).
[[nodiscard]] RestartRefusal check_kill_file(const store::Recovery& prev,
                                             std::string_view kill_path);

// [engine] epoch_file does not exist: refused with kExitState when the store holds sessions with an
// epoch (Recovery::session_epochs).
[[nodiscard]] RestartRefusal check_epoch_file(const store::Recovery& prev,
                                              std::string_view epoch_path);

// An instrument whose stored position the session restores, and whether its venue reports the
// position itself before the engine sends an order (VenueCapabilities::position_snapshot, for a
// derivative).
struct RestoredInstrument {
  std::string_view venue;  // [venues.<name>]
  std::string_view symbol;
  bool venue_reports = false;
};

// The entries of Recovery::double_booked the session would trade on: restored, and not replaced by
// the venue's own report first. A store that predates venue names ("#<id>") matches by symbol.
[[nodiscard]] std::vector<store::Recovery::DoubleBooked> wrong_positions(
    const store::Recovery& prev, std::span<const RestoredInstrument> restored);

// The refusal (kExitState) for `wrong`, non-empty, naming the queries and the ways out.
[[nodiscard]] RestartRefusal position_refusal(const store::Recovery& prev,
                                              std::span<const store::Recovery::DoubleBooked> wrong,
                                              std::string_view engine);

}  // namespace fastmm::live
