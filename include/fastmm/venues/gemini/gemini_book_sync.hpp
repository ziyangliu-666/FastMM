#pragma once
// Gemini order-book synchronisation for `{symbol}@depth@100ms` on wss://ws.gemini.com with the
// connection parameter `snapshot=-1` (https://developer.gemini.com/websocket/introduction.md and
// the AsyncAPI spec https://developer.gemini.com/specs/asyncapi/websocket.yaml, DepthUpdate, read
// 2026-09-30):
//
//   "the FIRST frame after subscribing is the snapshot (b/a hold absolute levels, and U..u
//   identifies the update range covered by the frame); every frame after it is an incremental
//   diff. There is no separate snapshot message and no lastUpdateId field. Apply frames in order,
//   using u as the last applied update ID; if a frame's U skips ahead of the last applied u,
//   discard the book and resubscribe to resync."
//
// The ids are Gemini's event ids, sparse, and consecutive frames overlap: a diff's U equals the
// u of the frame before it (production, 2026-09-30; the SDK's order book states the same rule:
// stale u <= last, applied U <= last < u, gap U > last). The snapshot frame carries U == u.
//
// Since a snapshot looks like any other frame, the feed marks it (GeminiMdFeed): the first
// depthUpdate of a symbol after the connection opens, and after a resubscription the first one
// after the unsubscribe's reply. Frames of the old subscription can arrive until that reply; they
// are dropped (the syncer is Buffering, and this traits class buffers nothing).
//
// BookDeltaMsg mapping (GeminiMdParser): first_update_id = U, last_update_id = u,
// prev_update_id = U.
#include "fastmm/venues/book_sync.hpp"

#include <cstdint>

namespace fastmm::venues::gemini {

struct GeminiSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t last) noexcept {
    return d.last_update_id <= last;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.first_update_id <= snap && d.last_update_id > snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t last) noexcept {
    return d.first_update_id <= last && d.last_update_id > last;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

using GeminiBookSync = StreamBookSync<GeminiSyncTraits>;

}  // namespace fastmm::venues::gemini
