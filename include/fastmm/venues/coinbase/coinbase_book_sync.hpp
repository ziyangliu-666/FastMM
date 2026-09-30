#pragma once
// Order-book synchronisation shared by the two Coinbase feeds (the Exchange's level2 channels and
// Advanced Trade's level2 channel). Both send the book as a snapshot on the stream, then updates
// that carry no sequence number of their own: each feed numbers the book messages of a product
// itself (snapshot n, update k: prev k - 1, last k) and detects loss by other means (trade ids on
// the Exchange, the connection's sequence_num on Advanced Trade), resyncing the book when it
// finds some. Under that numbering a message applies when it follows the last one applied.
#include "fastmm/venues/book_sync.hpp"

#include <cstdint>

namespace fastmm::venues::coinbase {

struct CoinbaseSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg&, std::uint64_t) noexcept { return false; }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.prev_update_id == snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev) noexcept {
    return d.prev_update_id == prev;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

using CoinbaseBookSync = StreamBookSync<CoinbaseSyncTraits>;
using ResubscribeRequester = InstrumentCallback;

// Numbers a book message of one product (see above).
inline void number_book_message(BookDeltaMsg& d, std::uint64_t& counter) noexcept {
  if (d.is_snapshot()) {
    d.first_update_id = d.last_update_id = ++counter;
    d.prev_update_id = 0;
  } else {
    d.prev_update_id = counter;
    d.first_update_id = d.last_update_id = ++counter;
  }
  d.hdr.venue_seq = d.last_update_id;
}

}  // namespace fastmm::venues::coinbase
