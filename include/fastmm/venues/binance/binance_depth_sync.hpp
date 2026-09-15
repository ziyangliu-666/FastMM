#pragma once
// Per-instrument Binance Spot order-book synchronisation (6.4). Wraps the core
// BookSyncer<BinanceSpotSyncTraits> ("How to manage a local order book correctly",
// web-socket-streams.md): buffer depthUpdate events, fetch GET /api/v3/depth (limit 1000 =
// weight 50, rest-api.md "Order book" weight table), drop events with u <= lastUpdateId,
// require U <= lastUpdateId+1 <= u for the first applied event and U == prev_u + 1 after
// that; any gap discards the book and restarts.
//
// Output contract (6.2): BookSnapshot before the first BookDelta; a gap emits
// ConnectionState{Resyncing, channel 0} then a fresh snapshot. Snapshot requests go through
// an injected callback and are rate limited to one per instrument per min_interval (2 s by
// default) so a flapping stream cannot burn REST weight. A full market-data ring drops the
// delta and forces a resync (6.7).
#include "fastmm/core/book/book_syncer.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <cstdint>

namespace fastmm::venues::binance {

struct SnapshotRequester {
  void (*fn)(void* ctx, InstrumentId id) noexcept = nullptr;
  void* ctx = nullptr;
  void operator()(InstrumentId id) const noexcept {
    if (fn != nullptr) fn(ctx, id);
  }
};

// Traits: BinanceSpotSyncTraits (Spot, U/u chaining) or BinanceFuturesSyncTraits (USDⓈ-M, pu
// chaining; see binance_usdm_md_feed.hpp).
template <class Traits>
class BasicBinanceDepthSync {
 public:
  static constexpr std::int64_t kDefaultMinInterval = 2'000'000'000;  // 2 s

  BasicBinanceDepthSync(InstrumentId instrument,
                        VenueId venue,
                        EventSink& sink,
                        SnapshotRequester requester,
                        std::int64_t min_interval_ns = kDefaultMinInterval,
                        std::size_t buffer_bytes = BookSyncer<Traits, int>::kDefaultBufferBytes)
      : instrument_(instrument),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        min_interval_ns_(min_interval_ns),
        inner_{this},
        syncer_(inner_, buffer_bytes) {}

  // Begin synchronisation (stream connected / subscribed). Issues the snapshot request
  // subject to the rate limit; on_timer() retries when it was deferred.
  void start(std::int64_t now_ns) noexcept {
    now_ = now_ns;
    syncer_.start();
    flush_request(now_ns);
  }
  // Stream lost: the book is unusable until start() again.
  void stop() noexcept {
    stopped_ = true;
    want_request_ = false;
    pending_request_ = false;
    // BookSyncer has no explicit stop; a resync on the next start() rebuilds it.
  }

  void on_delta(const BookDeltaMsg& d, std::int64_t now_ns) noexcept {
    now_ = now_ns;
    overflowed_ = false;
    syncer_.on_delta(d);
    if (overflowed_) syncer_.resync(SyncReason::Explicit);
    flush_request(now_ns);
  }
  // REST snapshot arrived (already decoded to a kSnapshot BookDeltaMsg).
  void on_snapshot(const BookDeltaMsg& snap, std::int64_t now_ns) noexcept {
    now_ = now_ns;
    pending_request_ = false;
    overflowed_ = false;
    syncer_.on_snapshot(snap);
    if (overflowed_) syncer_.resync(SyncReason::Explicit);
    flush_request(now_ns);
  }
  // REST snapshot failed (HTTP error / timeout): retry after the interval.
  void on_snapshot_failed(std::int64_t now_ns) noexcept {
    now_ = now_ns;
    pending_request_ = false;
    want_request_ = true;
    flush_request(now_ns);
  }
  void resync(SyncReason reason, std::int64_t now_ns) noexcept {
    now_ = now_ns;
    syncer_.resync(reason);
    flush_request(now_ns);
  }
  void on_timer(std::int64_t now_ns) noexcept {
    now_ = now_ns;
    flush_request(now_ns);
  }

  [[nodiscard]] bool synced() const noexcept { return syncer_.synced(); }
  [[nodiscard]] SyncState state() const noexcept { return syncer_.state(); }
  [[nodiscard]] std::uint32_t resync_count() const noexcept { return syncer_.resync_count(); }
  [[nodiscard]] std::uint64_t last_update_id() const noexcept { return syncer_.last_update_id(); }
  [[nodiscard]] std::uint64_t snapshot_requests() const noexcept { return requests_; }
  [[nodiscard]] std::uint64_t deferred_requests() const noexcept { return deferred_; }
  [[nodiscard]] bool request_pending() const noexcept { return pending_request_; }
  [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }

 private:
  struct Inner {
    BasicBinanceDepthSync* owner;
    void on_snapshot(const BookDeltaMsg& m) noexcept { owner->forward(m); }
    void on_delta(const BookDeltaMsg& m) noexcept { owner->forward(m); }
    void on_resync(SyncReason reason) noexcept { owner->emit_resyncing(reason); }
    void request_snapshot() noexcept { owner->want_request_ = true; }
  };

  void forward(const BookDeltaMsg& m) noexcept {
    if (!sink_.push(m.hdr)) overflowed_ = true;
  }
  void emit_resyncing(SyncReason reason) noexcept {
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, instrument_, venue_);
    m.state = ConnState::Resyncing;
    m.channel = 0;
    m.reason_code = static_cast<std::int32_t>(reason);
    m.hdr.recv_ts = wall_now();
    static_cast<void>(sink_.push(m.hdr));
  }
  // Issues the deferred request if the rate limit allows it.
  void flush_request(std::int64_t now_ns) noexcept {
    if (stopped_) stopped_ = false;
    if (!want_request_ || pending_request_) return;
    if (last_request_ns_ != 0 && now_ns - last_request_ns_ < min_interval_ns_) {
      ++deferred_;
      return;
    }
    want_request_ = false;
    pending_request_ = true;
    last_request_ns_ = now_ns;
    ++requests_;
    requester_(instrument_);
  }

  InstrumentId instrument_;
  VenueId venue_;
  EventSink& sink_;
  SnapshotRequester requester_;
  std::int64_t min_interval_ns_;
  std::int64_t now_ = 0;
  std::int64_t last_request_ns_ = 0;
  std::uint64_t requests_ = 0;
  std::uint64_t deferred_ = 0;
  bool want_request_ = false;
  bool pending_request_ = false;
  bool overflowed_ = false;
  bool stopped_ = false;
  Inner inner_;
  BookSyncer<Traits, Inner> syncer_;
};

using BinanceDepthSync = BasicBinanceDepthSync<BinanceSpotSyncTraits>;

}  // namespace fastmm::venues::binance
