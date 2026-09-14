#pragma once
// Per-instrument Deribit order-book synchronisation. Wraps the core BookSyncer with
// DeribitSyncTraits for the `book.{instrument_name}.{interval}` channel
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
// consecutive integers. A gap (or no snapshot within the timeout) unsubscribes and re-subscribes
// the channel, which makes the venue send a fresh snapshot, rate limited to one request per
// instrument per min_interval.
//
// Output contract (6.2): BookSnapshot before the first BookDelta; a gap emits
// ConnectionState{Resyncing, channel 0} before the next snapshot.
#include "fastmm/core/book/book_syncer.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <cstdint>

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

struct ResubscribeRequester {
  void (*fn)(void* ctx, InstrumentId id) noexcept = nullptr;
  void* ctx = nullptr;
  void operator()(InstrumentId id) const noexcept {
    if (fn != nullptr) fn(ctx, id);
  }
};

class DeribitBookSync {
 public:
  static constexpr std::int64_t kDefaultMinInterval = 2'000'000'000;       // 2 s
  static constexpr std::int64_t kDefaultSnapshotTimeout = 10'000'000'000;  // 10 s

  DeribitBookSync(InstrumentId instrument,
                  VenueId venue,
                  EventSink& sink,
                  ResubscribeRequester requester,
                  std::int64_t min_interval_ns = kDefaultMinInterval,
                  std::int64_t snapshot_timeout_ns = kDefaultSnapshotTimeout)
      : instrument_(instrument),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        min_interval_ns_(min_interval_ns),
        snapshot_timeout_ns_(snapshot_timeout_ns),
        inner_{this},
        syncer_(inner_) {}

  // Subscribed: the snapshot arrives on the stream.
  void start(std::int64_t now_ns) noexcept {
    active_ = true;
    want_resubscribe_ = false;
    waiting_since_ = now_ns;
    syncer_.start();
  }
  void stop() noexcept {
    active_ = false;
    want_resubscribe_ = false;
  }

  void on_book(const BookDeltaMsg& d, std::int64_t now_ns) noexcept {
    if (!active_) return;
    overflowed_ = false;
    const bool was_synced = syncer_.synced();
    syncer_.on_delta(d);  // snapshots are routed to on_snapshot by the core syncer
    if (overflowed_) syncer_.resync(SyncReason::BufferOverflow);
    if (syncer_.synced() && !was_synced) waiting_since_ = 0;
    if (!syncer_.synced() && waiting_since_ == 0) waiting_since_ = now_ns;
    flush(now_ns);
  }

  void resync(SyncReason reason, std::int64_t now_ns) noexcept {
    if (!active_) return;
    syncer_.resync(reason);
    waiting_since_ = now_ns;
    flush(now_ns);
  }

  // Retries deferred re-subscriptions and re-subscribes when no snapshot came in time.
  void on_timer(std::int64_t now_ns) noexcept {
    if (!active_) return;
    if (!syncer_.synced() && waiting_since_ != 0 &&
        now_ns - waiting_since_ >= snapshot_timeout_ns_) {
      want_resubscribe_ = true;
      waiting_since_ = now_ns;
    }
    flush(now_ns);
  }

  [[nodiscard]] bool synced() const noexcept { return syncer_.synced(); }
  [[nodiscard]] SyncState state() const noexcept { return syncer_.state(); }
  [[nodiscard]] std::uint32_t resync_count() const noexcept { return syncer_.resync_count(); }
  [[nodiscard]] std::uint64_t last_change_id() const noexcept { return syncer_.last_update_id(); }
  [[nodiscard]] std::uint64_t resubscribe_requests() const noexcept { return requests_; }
  [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }

 private:
  struct Inner {
    DeribitBookSync* owner;
    void on_snapshot(const BookDeltaMsg& m) noexcept { owner->forward(m); }
    void on_delta(const BookDeltaMsg& m) noexcept { owner->forward(m); }
    void on_resync(SyncReason reason) noexcept {
      owner->emit_resyncing(reason);
      owner->want_resubscribe_ = true;
    }
    void request_snapshot() noexcept {}  // DeribitSyncTraits::kNeedsRestSnapshot == false
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
  void flush(std::int64_t now_ns) noexcept {
    if (!want_resubscribe_) return;
    if (last_request_ns_ != 0 && now_ns - last_request_ns_ < min_interval_ns_) return;
    want_resubscribe_ = false;
    last_request_ns_ = now_ns;
    ++requests_;
    requester_(instrument_);
  }

  InstrumentId instrument_;
  VenueId venue_;
  EventSink& sink_;
  ResubscribeRequester requester_;
  std::int64_t min_interval_ns_;
  std::int64_t snapshot_timeout_ns_;
  std::int64_t last_request_ns_ = 0;
  std::int64_t waiting_since_ = 0;
  std::uint64_t requests_ = 0;
  bool active_ = false;
  bool want_resubscribe_ = false;
  bool overflowed_ = false;
  Inner inner_;
  BookSyncer<DeribitSyncTraits, Inner> syncer_;
};

}  // namespace fastmm::venues::deribit
