#pragma once
// MdSource: the simulator's view of a market-data stream (journal / csv / arrays / synthetic
// recording). next() yields normalized events (BookDelta/BookSnapshot/Trade/BookTicker) in
// non-decreasing hdr.recv_ts order; the pointer stays valid until the next call. The
// concrete sources live in fastmm::bt (backtest); the interface lives here so SimDriver
// can consume them without depending on the backtest library.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::sim {

struct SimAccountConfig;

// Largest event a source builds itself: a full L2Book<256> snapshot. A journal yields its records
// as recorded, up to kMaxMsgBytes (kMaxBookLevelsPerMsg a side), so whatever copies an event it
// was handed into an EventBuf checks hdr.len first.
inline constexpr std::uint32_t kMaxSourceEventBytes = BookDeltaMsg::size_for(256, 256);

struct alignas(64) EventBuf {
  std::byte bytes[kMaxSourceEventBytes];
  [[nodiscard]] const EventHeader& hdr() const noexcept {
    return *reinterpret_cast<const EventHeader*>(bytes);
  }
  [[nodiscard]] EventHeader& hdr() noexcept { return *reinterpret_cast<EventHeader*>(bytes); }
  template <class M>
  [[nodiscard]] M& as() noexcept {
    return *reinterpret_cast<M*>(bytes);
  }
  template <class M>
  [[nodiscard]] const M& as() const noexcept {
    return *reinterpret_cast<const M*>(bytes);
  }
};

// What a recorded session started from (backtest [backtest] initial_state = "journal"): the
// bytes for the strategy's restore(), and Reconcile Position messages that set the engine's
// positions as the session had them. `note` says where they came from.
struct SessionStart {
  bool has_state = false;  // `strategy` holds a recorded state (possibly empty)
  std::string strategy;
  std::vector<ReconcileMsg> positions;
  std::string note;
};

// A recorded session's parameter updates (backtest [backtest] params_from_journal): each at the
// time the session received it (hdr.recv_ts), its fields by the session's parameter table, the
// name and ParamType of each recorded index (empty: the indices are the strategy's own).
struct RecordedParams {
  std::vector<std::pair<std::string, std::uint8_t>> table;
  std::vector<ParamUpdateMsg> updates;
};

class MdSource {
 public:
  virtual ~MdSource() = default;
  // nullptr at end of data.
  virtual const EventHeader* next() = 0;
  // Rewind to the first event (sweeps run one cursor per worker).
  virtual void reset() = 0;
  // The first balance snapshot the source recorded for each venue (sim_account.hpp); null for a
  // source that cannot carry balances (anything but a journal).
  [[nodiscard]] virtual const std::vector<SimAccountConfig>* balance_snapshots() const {
    return nullptr;
  }
  // The state the recorded session started from; null for a source that cannot carry one
  // (anything but a journal).
  [[nodiscard]] virtual const SessionStart* session_start() const { return nullptr; }
  // The parameter updates the recorded session received; null for a source without them.
  [[nodiscard]] virtual const RecordedParams* recorded_params() const { return nullptr; }
  // Time of the first event if known (invalid Timestamp otherwise); lets the driver start
  // the virtual clock at the data.
  [[nodiscard]] virtual Timestamp start_ts() const { return Timestamp{}; }
  // One line about what the source did to the data, for the run's report (empty: nothing).
  [[nodiscard]] virtual std::string note() const { return {}; }
};

}  // namespace fastmm::sim
