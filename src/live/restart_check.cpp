#include "fastmm/live/restart_check.hpp"

#include "fastmm/live/session.hpp"

#include <fmt/format.h>

namespace fastmm::live {

RestartRefusal check_kill_file(const store::Recovery& prev, std::string_view kill_path) {
  if (!prev.found || !prev.max_loss_tripped) return {};
  return {kExitKilled,
          fmt::format("[engine] kill_file {} is missing, and the store's newest session ({}, "
                      "started {}) ended on a max_loss trip. Check the positions, then start with "
                      "--clear-kill; that arms the whole [risk] max_loss budget again.",
                      kill_path,
                      prev.session_id,
                      prev.started_utc)};
}

RestartRefusal check_epoch_file(const store::Recovery& prev, std::string_view epoch_path) {
  if (!prev.found || prev.session_epochs.empty()) return {};
  const unsigned newest = prev.session_epochs.front();
  return {kExitState,
          fmt::format("[engine] epoch_file {} is missing, and the store holds sessions of this "
                      "engine up to epoch {}: a new counter starts at 1 and reuses their client "
                      "order ids. Restore the file, or write the newest epoch of every engine "
                      "that shares it (this engine's is {}): echo {} > {}",
                      epoch_path,
                      newest,
                      newest,
                      newest,
                      epoch_path)};
}

std::vector<store::Recovery::DoubleBooked> wrong_positions(
    const store::Recovery& prev, std::span<const RestoredInstrument> restored) {
  std::vector<store::Recovery::DoubleBooked> out;
  for (const store::Recovery::DoubleBooked& d : prev.double_booked) {
    const bool unnamed = d.venue.starts_with('#');
    for (const RestoredInstrument& r : restored) {
      if (r.symbol != d.symbol || (!unnamed && r.venue != d.venue) || r.venue_reports) continue;
      out.push_back(d);
      break;
    }
  }
  return out;
}

RestartRefusal position_refusal(const store::Recovery& prev,
                                std::span<const store::Recovery::DoubleBooked> wrong,
                                std::string_view engine) {
  std::string what;
  for (const store::Recovery::DoubleBooked& d : wrong) {
    if (!what.empty()) what += ", ";
    what += fmt::format("{} {} {}", d.executions, d.venue, d.symbol);
  }
  return {kExitState,
          fmt::format("the store's newest session ({}) booked executions more than once ({}; "
                      "fastmm-pnl duplicates --engine {}): the position it left counts them twice, "
                      "and the venue does not report its own before this session would trade. "
                      "Check the account, then start with --accept-position to trade from the "
                      "stored position, or with [engine] restore_position = false to start flat.",
                      prev.session_id,
                      what,
                      engine)};
}

}  // namespace fastmm::live
