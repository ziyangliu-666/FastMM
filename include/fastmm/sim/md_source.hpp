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

struct RecordedSession;

// Largest event a source builds itself: a full L2Book<256> snapshot. A journal yields its records
// as recorded, up to kMaxMsgBytes (kMaxBookLevelsPerMsg a side), so whatever copies an event it
// was handed holds kMaxMsgBytes, not an EventBuf.
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

class MdSource {
 public:
  virtual ~MdSource() = default;
  // nullptr at end of data.
  virtual const EventHeader* next() = 0;
  // Rewind to the first event (sweeps run one cursor per worker).
  virtual void reset() = 0;
  // What the recorded session's journal says about the session itself (recorded_session.hpp); null
  // for a source that is not a session's journal.
  [[nodiscard]] virtual const RecordedSession* recorded() const { return nullptr; }
  // Time of the first event if known (invalid Timestamp otherwise); lets the driver start
  // the virtual clock at the data.
  [[nodiscard]] virtual Timestamp start_ts() const { return Timestamp{}; }
  // One line about what the source did to the data, for the run's report (empty: nothing).
  [[nodiscard]] virtual std::string note() const { return {}; }
  // The source yields every order of its books (OrderAddL3 and the rest), from the start of the
  // run: what fill_model = "l3_queue" needs.
  [[nodiscard]] virtual bool order_by_order() const { return false; }
};

}  // namespace fastmm::sim
