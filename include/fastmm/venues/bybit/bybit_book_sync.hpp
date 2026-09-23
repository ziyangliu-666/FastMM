#pragma once
// Per-instrument Bybit v5 order-book synchronisation (6.5). The stream itself delivers a
// `snapshot` after subscribing and `delta`s afterwards
// (https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook), so this is
// venues::StreamBookSync (book_sync.hpp) with BybitSyncTraits: `u` must increase, and
// "u=1 indicates a restart snapshot" (docs), which the traits treat as a reset.
//
// Sequencing (checked against the orderbook page, 2026-09-14): the docs promise neither that u
// grows by exactly one per delta nor any other gap rule; they only say that a new snapshot
// (including u=1 after a service restart, and the level-1 spot snapshot resent after 3 s
// without changes) resets the local book, and that a smaller seq means older data. Recorded
// testnet deltas (tests/fixtures/bybit/raw_md_stream.jsonl) did step by one, but the traits
// require only a strict increase, so an id Bybit legitimately skips never forces a resync.
#include "fastmm/venues/book_sync.hpp"

namespace fastmm::venues::bybit {

using ResubscribeRequester = InstrumentCallback;
using BybitBookSync = StreamBookSync<BybitSyncTraits>;

}  // namespace fastmm::venues::bybit
