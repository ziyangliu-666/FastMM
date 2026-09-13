#include "fastmm/core/book/book_syncer.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <new>
#include <string>
#include <vector>

using namespace fastmm;

namespace {
struct RecordingSink {
  std::vector<std::string> events;
  int snapshot_requests = 0;
  void on_snapshot(const BookDeltaMsg& m) {
    events.push_back("snap:" + std::to_string(m.last_update_id));
  }
  void on_delta(const BookDeltaMsg& m) {
    events.push_back("delta:" + std::to_string(m.first_update_id) + "-" +
                     std::to_string(m.last_update_id));
  }
  void on_resync(SyncReason r) {
    events.push_back("resync:" + std::to_string(static_cast<int>(r)));
  }
  void request_snapshot() {
    ++snapshot_requests;
    events.push_back("request");
  }
};

// BookDeltaMsg must never live by value: hdr.len (128 for zero levels) is longer than the 96-byte
// struct, and the syncer copies hdr.len bytes. Each test message owns a correctly sized buffer.
struct MsgBuf {
  alignas(64) std::byte bytes[BookDeltaMsg::size_for(0, 0)]{};
  // NOLINTNEXTLINE(google-explicit-constructor): lets tests pass delta(...) straight to the syncer
  operator const BookDeltaMsg&() const noexcept {
    return *reinterpret_cast<const BookDeltaMsg*>(bytes);
  }
};

MsgBuf delta(std::uint64_t U, std::uint64_t u, std::uint64_t pu = 0, bool snapshot = false) {
  MsgBuf b;
  auto* d = new (b.bytes) BookDeltaMsg{};
  init_header(*d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              BookDeltaMsg::size_for(0, 0));
  d->first_update_id = U;
  d->last_update_id = u;
  d->prev_update_id = pu;
  if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
  return b;
}
MsgBuf snapshot(std::uint64_t L) {
  return delta(0, L, 0, true);
}
}  // namespace

TEST_CASE("core.book_syncer: binance spot happy path with buffering") {
  RecordingSink sink;
  BookSyncer<BinanceSpotSyncTraits, RecordingSink> s(sink);
  CHECK(s.state() == SyncState::Idle);
  s.on_delta(delta(1, 2));  // ignored while idle
  s.start();
  CHECK(s.state() == SyncState::Buffering);
  CHECK(sink.snapshot_requests == 1);
  s.on_delta(delta(5, 7));
  s.on_delta(delta(8, 10));
  s.on_delta(delta(11, 12));
  CHECK(s.buffered() == 3);
  s.on_snapshot(snapshot(9));  // L=9: drop 5-7 (u<=9), first must satisfy U<=10<=u -> 8-10 ok
  CHECK(s.state() == SyncState::Synced);
  CHECK(s.last_update_id() == 12);
  CHECK(sink.events == std::vector<std::string>{"request", "snap:9", "delta:8-10", "delta:11-12"});
  s.on_delta(delta(13, 15));
  CHECK(sink.events.back() == "delta:13-15");
  s.on_delta(delta(13, 15));  // duplicate/stale -> dropped silently
  CHECK(sink.events.back() == "delta:13-15");
  CHECK(sink.events.size() == 5);
  // gap -> resync -> new request
  s.on_delta(delta(17, 18));
  CHECK(s.state() == SyncState::Buffering);
  CHECK(s.resync_count() == 1);
  CHECK(sink.events[5] == "resync:1");
  CHECK(sink.events[6] == "request");
  CHECK(sink.snapshot_requests == 2);
  // deltas during resync buffer again; snapshot too old -> re-request
  s.on_delta(delta(30, 31));
  s.on_snapshot(snapshot(20));  // first buffered U=30 > L+1=21 -> too old
  CHECK(s.state() == SyncState::Buffering);
  CHECK(sink.snapshot_requests == 3);
  CHECK(s.buffered() == 1);
  s.on_delta(delta(32, 33));
  s.on_snapshot(snapshot(30));  // U=30 <= 31 <= 31 ok, then 32-33
  CHECK(s.state() == SyncState::Synced);
  CHECK(s.last_update_id() == 33);
}

TEST_CASE("core.book_syncer: binance spot buffer overflow forces resync") {
  RecordingSink sink;
  BookSyncer<BinanceSpotSyncTraits, RecordingSink> s(sink,
                                                     4096);  // tiny arena: 32 x 128-byte deltas
  s.start();
  for (std::uint64_t i = 0; i < 40; ++i) s.on_delta(delta(i * 2, i * 2 + 1));
  CHECK(s.resync_count() >= 1);
  CHECK(s.state() == SyncState::Buffering);
  CHECK(sink.snapshot_requests >= 2);
  // gap inside the buffered stream is detected at replay time
  RecordingSink sink2;
  BookSyncer<BinanceSpotSyncTraits, RecordingSink> s2(sink2);
  s2.start();
  s2.on_delta(delta(10, 11));
  s2.on_delta(delta(13, 14));  // gap (12 missing)
  s2.on_snapshot(snapshot(10));
  CHECK(s2.state() == SyncState::Buffering);
  CHECK(s2.resync_count() == 1);
  CHECK(sink2.events ==
        std::vector<std::string>{"request", "snap:10", "delta:10-11", "resync:1", "request"});
}

TEST_CASE("core.book_syncer: binance futures uses pu chaining") {
  RecordingSink sink;
  BookSyncer<BinanceFuturesSyncTraits, RecordingSink> s(sink);
  s.start();
  s.on_delta(delta(90, 95, 80));
  s.on_delta(delta(96, 100, 95));
  s.on_delta(delta(101, 104, 100));
  s.on_snapshot(snapshot(97));  // drop u<97: none (95<97 dropped); first: U=96<=97<=100 ok
  CHECK(s.state() == SyncState::Synced);
  CHECK(sink.events ==
        std::vector<std::string>{"request", "snap:97", "delta:96-100", "delta:101-104"});
  s.on_delta(delta(105, 106, 104));
  CHECK(s.synced());
  s.on_delta(delta(107, 108, 106));
  CHECK(s.synced());
  s.on_delta(delta(110, 111, 109));  // pu != prev_u -> gap
  CHECK_FALSE(s.synced());
  CHECK(s.resync_count() == 1);
}

TEST_CASE("core.book_syncer: bybit in-stream snapshot, strictly increasing u, u==1 marker") {
  RecordingSink sink;
  BookSyncer<BybitSyncTraits, RecordingSink> s(sink);
  s.start();
  CHECK(sink.snapshot_requests == 0);  // no REST snapshot
  s.on_delta(delta(0, 5));             // delta before snapshot: dropped (no buffering)
  CHECK(s.state() == SyncState::Buffering);
  s.on_delta(snapshot(10));
  CHECK(s.state() == SyncState::Synced);
  s.on_delta(delta(0, 11));
  s.on_delta(delta(0, 15));  // gaps in u are fine as long as increasing
  CHECK(s.last_update_id() == 15);
  s.on_delta(delta(0, 15));  // duplicate: dropped
  s.on_delta(delta(0, 14));  // out of order: stale, dropped
  CHECK(sink.events == std::vector<std::string>{"snap:10", "delta:0-11", "delta:0-15"});
  s.on_delta(delta(0, 1));  // u == 1 -> venue reset -> resubscribe
  CHECK(s.state() == SyncState::Buffering);
  CHECK(s.resync_count() == 1);
  CHECK(sink.events.back() == "resync:5");
  s.on_delta(snapshot(2));
  CHECK(s.synced());
  s.resync(SyncReason::Explicit);
  CHECK(s.state() == SyncState::Buffering);
}
