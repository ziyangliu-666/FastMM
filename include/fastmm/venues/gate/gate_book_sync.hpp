#pragma once
// Per-instrument Gate futures order-book synchronisation on the `futures.obu` channel (Order Book
// V2, futures WS docs "Order Book V2 API", read 2026-10-03): the first push after subscribing is a
// full snapshot (`full: true`, depth id `u`), then increments carry `U` and `u`; an increment
// applies when `U` equals the local depth id + 1, and anything else means "unsubscribe and
// resubscribe to retrieve the initial depth snapshot". The server may send a full snapshot again
// at any time; the parser marks those kSnapshot and the core syncer treats them as a new base. So
// this is venues::StreamBookSync (book_sync.hpp) with these traits and no REST snapshot.
#include "fastmm/venues/book_sync.hpp"

namespace fastmm::venues::gate {

struct GateObuSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.last_update_id <= snap;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.first_update_id == snap + 1;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev_u) noexcept {
    return d.first_update_id == prev_u + 1;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

using ResubscribeRequester = InstrumentCallback;
using GateBookSync = StreamBookSync<GateObuSyncTraits>;

}  // namespace fastmm::venues::gate
