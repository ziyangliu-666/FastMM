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
//
// Every resync is logged with its reason and the update ids that caused it, and the book's return
// with how long it was away (the engine pulls quotes for that long).
//
// On a new stream the snapshot is requested once the first event is buffered, as the venue's
// procedure says (buffer the stream's events, then get a depth snapshot), not when the connection
// opens. Requested at once, the snapshot could come back before the stream's first event and be
// older than it: USDⓈ-M's first event arrives some 200 ms after the connection opens, and its
// update range then started after the snapshot's lastUpdateId. With nothing buffered the snapshot
// was applied, and that first event, which does not bracket it, resynced the book (the next
// snapshot 2 s later, by the rate limit) on many starts. A book that does not change sends no
// event: kStreamWait after start() without one, on_timer() requests the snapshot anyway.
#include "fastmm/core/log.hpp"
#include "fastmm/venues/book_sync.hpp"

#include <string_view>

namespace fastmm::venues::binance {

using SnapshotRequester = InstrumentCallback;

// Traits: BinanceSpotSyncTraits (Spot, U/u chaining) or BinanceFuturesSyncTraits (USDⓈ-M, pu
// chaining; see binance_usdm_md_feed.hpp).
template <class Traits>
class BasicBinanceDepthSync {
 public:
  static constexpr std::int64_t kDefaultMinInterval = 2'000'000'000;  // 2 s
  static constexpr std::int64_t kStreamWait = 500'000'000;            // 0.5 s

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

  // Begin synchronisation (stream connected / subscribed). The snapshot request goes out with the
  // stream's first event, or from on_timer() kStreamWait after this without one, subject to the
  // rate limit; on_timer() retries when it was deferred.
  void start(std::int64_t now_ns) noexcept {
    now_ = now_ns;
    stream_wait_since_ = now_ns;
    awaiting_stream_ = true;
    syncer_.start();
    flush_request(now_ns);
  }
  // Stream lost: the book is unusable until start() again.
  void stop() noexcept {
    stopped_ = true;
    awaiting_stream_ = false;
    want_request_ = false;
    pending_request_ = false;
    // BookSyncer has no explicit stop; a resync on the next start() rebuilds it.
  }

  void on_delta(const BookDeltaMsg& d, std::int64_t now_ns) noexcept {
    now_ = now_ns;
    overflowed_ = false;
    cause_ = &d;
    syncer_.on_delta(d);
    awaiting_stream_ = false;
    if (overflowed_) syncer_.resync(SyncReason::Explicit);
    cause_ = nullptr;
    note_synced();
    flush_request(now_ns);
  }
  // REST snapshot arrived (already decoded to a kSnapshot BookDeltaMsg).
  void on_snapshot(const BookDeltaMsg& snap, std::int64_t now_ns) noexcept {
    now_ = now_ns;
    pending_request_ = false;
    want_request_ = false;  // this one answers it; the syncer asks again if it is too old
    overflowed_ = false;
    snapshot_id_ = snap.last_update_id;
    syncer_.on_snapshot(snap);
    if (overflowed_) syncer_.resync(SyncReason::Explicit);
    snapshot_id_ = 0;
    note_synced();
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

  // Names for the log: the venue (the connector's config name) and the venue's symbol. Both must
  // outlive the sync.
  void set_log_names(std::string_view venue, std::string_view symbol) noexcept {
    venue_name_ = venue;
    symbol_ = symbol;
  }

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
    log_resync(reason);
    if (away_since_ == 0) away_since_ = now_ != 0 ? now_ : 1;
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, instrument_, venue_);
    m.state = ConnState::Resyncing;
    m.channel = 0;
    m.reason_code = static_cast<std::int32_t>(reason);
    m.hdr.recv_ts = wall_now();
    static_cast<void>(sink_.push(m.hdr));
  }
  void log_resync(SyncReason reason) const noexcept {
    if (cause_ != nullptr) {
      FASTMM_LOG_WARN("{}: {} book resync ({}): book at update {}, next update U={} u={} pu={}",
                      venue_name_,
                      symbol_,
                      to_string(reason),
                      syncer_.last_update_id(),
                      cause_->first_update_id,
                      cause_->last_update_id,
                      cause_->prev_update_id);
    } else if (snapshot_id_ != 0) {
      FASTMM_LOG_WARN("{}: {} book resync ({}): snapshot lastUpdateId={}",
                      venue_name_,
                      symbol_,
                      to_string(reason),
                      snapshot_id_);
    } else {
      FASTMM_LOG_WARN("{}: {} book resync ({})", venue_name_, symbol_, to_string(reason));
    }
  }
  // After a resync: the book is back, and was away (the engine without it) for this long.
  void note_synced() noexcept {
    if (away_since_ == 0 || !syncer_.synced()) return;
    FASTMM_LOG_INFO("{}: {} book synced again after {} ms ({} snapshot requests so far)",
                    venue_name_,
                    symbol_,
                    (now_ - away_since_) / 1'000'000,
                    requests_);
    away_since_ = 0;
  }

  // Issues the deferred request if the rate limit allows it.
  void flush_request(std::int64_t now_ns) noexcept {
    if (stopped_) stopped_ = false;
    // A requester that fails synchronously calls on_snapshot_failed from inside requester_(); the
    // retry is left to the timer, or a zero interval would recurse until the stack runs out.
    if (!want_request_ || pending_request_ || requesting_) return;
    if (awaiting_stream_) {
      if (now_ns - stream_wait_since_ < kStreamWait) return;
      awaiting_stream_ = false;
    }
    if (last_request_ns_ != 0 && now_ns - last_request_ns_ < min_interval_ns_) {
      ++deferred_;
      return;
    }
    want_request_ = false;
    pending_request_ = true;
    last_request_ns_ = now_ns;
    ++requests_;
    requesting_ = true;
    requester_(instrument_);
    requesting_ = false;
  }

  InstrumentId instrument_;
  VenueId venue_;
  EventSink& sink_;
  SnapshotRequester requester_;
  std::int64_t min_interval_ns_;
  std::int64_t now_ = 0;
  std::int64_t away_since_ = 0;          // since the last resync, until the book is synced again
  std::int64_t stream_wait_since_ = 0;   // start(): the snapshot request waits for the stream
  const BookDeltaMsg* cause_ = nullptr;  // the delta being handled, for the log
  std::uint64_t snapshot_id_ = 0;        // the snapshot being handled, for the log
  std::string_view venue_name_ = "binance";
  std::string_view symbol_;
  std::int64_t last_request_ns_ = 0;
  std::uint64_t requests_ = 0;
  std::uint64_t deferred_ = 0;
  bool want_request_ = false;
  bool pending_request_ = false;
  bool requesting_ = false;
  bool overflowed_ = false;
  bool stopped_ = false;
  bool awaiting_stream_ = false;  // a new stream has delivered no event yet (see the top)
  Inner inner_;
  BookSyncer<Traits, Inner> syncer_;
};

using BinanceDepthSync = BasicBinanceDepthSync<BinanceSpotSyncTraits>;

}  // namespace fastmm::venues::binance
