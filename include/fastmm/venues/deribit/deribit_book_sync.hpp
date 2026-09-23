#pragma once
// Per-instrument Deribit order-book synchronisation: venues::StreamBookSync (book_sync.hpp)
// with DeribitSyncTraits for the `book.{instrument_name}.{interval}` channel
// (https://docs.deribit.com/subscriptions/orderbook/bookinstrument_nameinterval):
//
//   "The first notification contains a full snapshot of the book ... Subsequent notifications
//    contain only incremental changes ... Each notification includes a change_id. Every message
//    except the first also contains prev_change_id. If prev_change_id equals the change_id of the
//    previous message, it indicates that no messages were missed."
//
// So a change applies iff prev_change_id == the last applied change_id; anything else is a gap,
// except a change whose change_id is not newer (a duplicate), which is dropped. Recorded testnet
// frames (tests/fixtures/deribit/book_*.json) chain exactly this way; change_ids are not
// consecutive integers.
#include "fastmm/venues/book_sync.hpp"

namespace fastmm::venues::deribit {

// BookDeltaMsg mapping (DeribitMdParser): first_update_id = last_update_id = change_id,
// prev_update_id = prev_change_id (0 on the snapshot).
struct DeribitSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t prev) noexcept {
    return d.last_update_id <= prev;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.prev_update_id == snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev) noexcept {
    return d.prev_update_id == prev;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

using ResubscribeRequester = InstrumentCallback;

// Deribit calls the sequence number a change id; the syncer calls it an update id.
class DeribitBookSync : public StreamBookSync<DeribitSyncTraits> {
 public:
  using StreamBookSync::StreamBookSync;
  [[nodiscard]] std::uint64_t last_change_id() const noexcept { return last_update_id(); }
};

}  // namespace fastmm::venues::deribit
