#pragma once
// A BookSnapshot message written from an L2Book, in the connectors' format: kSnapshot set, bids
// then asks best first, first/last_update_id and venue_seq = the book's seq(), exch_ts = the time
// of its last update. An engine book that applies it and then the deltas the source book applied
// after it holds the same levels, seq and update time as the source.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fastmm {

template <std::size_t N>
[[nodiscard]] std::uint32_t book_snapshot_size(const L2Book<N>& b) noexcept {
  return BookDeltaMsg::size_for(static_cast<std::uint32_t>(b.depth(Side::Buy)),
                                static_cast<std::uint32_t>(b.depth(Side::Sell)));
}

// Writes the snapshot into `out` (8-byte aligned, book_snapshot_size(b) bytes) and returns it.
// recv_ts is the caller's; t0_cycles stays 0, so the engine records no wire latency for it.
template <std::size_t N>
BookDeltaMsg& write_book_snapshot(const L2Book<N>& b,
                                  InstrumentId inst,
                                  VenueId venue,
                                  Timestamp recv_ts,
                                  std::byte* out) noexcept {
  static_assert(N <= kMaxBookLevelsPerMsg);
  const auto nb = static_cast<std::uint32_t>(b.depth(Side::Buy));
  const auto na = static_cast<std::uint32_t>(b.depth(Side::Sell));
  std::memset(out, 0, BookDeltaMsg::size_for(nb, na));  // the padding reaches the journals
  auto& d = *reinterpret_cast<BookDeltaMsg*>(out);
  init_header(d, EventType::BookSnapshot, inst, venue, BookDeltaMsg::size_for(nb, na));
  d.hdr.flags = static_cast<std::uint8_t>(EventHeader::kSnapshot | EventHeader::kSynthetic);
  d.hdr.venue_seq = b.seq();
  d.hdr.exch_ts = b.last_update();
  d.hdr.recv_ts = recv_ts;
  d.bid_count = nb;
  d.ask_count = na;
  d.first_update_id = b.seq();
  d.last_update_id = b.seq();
  d.prev_update_id = 0;
  Level* l = d.levels();
  b.for_each_level(Side::Buy, [&](const Level& x) { *l++ = x; });
  b.for_each_level(Side::Sell, [&](const Level& x) { *l++ = x; });
  return d;
}

}  // namespace fastmm
