#pragma once
// ReconcileDriver: the open-order snapshot every exchange connector sends the engine
// (ReconcileMsg Begin / OpenOrder* / Position* / End), written once. It owns when a snapshot is
// taken and what it says; the connector owns how the venue is asked and how its rows read.
//
// What the driver does:
//   * one snapshot at a time. A request while one is being fetched is served by another one
//     straight after it; a request while the execution replay before it runs is served by the
//     snapshot that replay releases (the watermark is taken when the snapshot is asked for, so
//     it covers that request too).
//   * the execution replay first (ReconcileHooks::replay_executions), and kExecutionsExact on the
//     Begin when it was complete.
//   * the sent watermark (SentWatermark::value) taken when the snapshot is asked for, after the
//     replay: every order the venue answered before that moment is in the snapshot or over, and
//     an order sent and answered while the replay ran is judged by it too. Taken when the
//     reconciliation was requested instead, those orders would wait for the next snapshot.
//   * the start-up sweep (sweep()): the first snapshot of a session carries an empty watermark,
//     so it says nothing about this session's own orders (one sent before the channel came up can
//     still be in flight) and only finds the orders nobody holds, which the engine cancels.
//   * a failed snapshot is asked again after kRetryNs; one that never answers is abandoned after
//     kFetchTimeoutNs; the generation moves on at every abandon, so a late reply is ignored.
//   * the shadow sweep: an order the connector keeps a shadow for that the snapshot does not name
//     and that was answered before the snapshot was asked for is over; its terminal event was lost
//     (typically with a connection). Without it the fixed-size shadow tables leak.
//   * nothing while closed: disconnect() closes the driver first, so a reply aborted by the
//     shutdown cannot start another request.
//
// Reactor thread only. Nothing here runs per order or per market-data message; the row buffers
// are reserved up front.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues {

// What a connector provides. fetch_snapshot() is asynchronous: rows go into the driver's buffers
// (ReconcileDriver::add_order / add_position) and the fetch ends with
// ReconcileDriver::fetched(generation, ok) exactly once, unless the driver moved on
// (ReconcileDriver::current() is false for its generation, and the rows must not be touched).
class ReconcileHooks {
 public:
  // Asks the venue for its open orders (and positions) for `generation`. False: nothing could be
  // sent (no transport now); the driver retries.
  virtual bool fetch_snapshot(std::uint64_t generation) = 0;
  // Starts the execution replay, or joins the one running. True: the snapshot waits for
  // ReconcileDriver::replay_done(). False: none can run, the snapshot goes now, not exact.
  virtual bool replay_executions() { return false; }
  // The client order ids the connector keeps an order shadow for, and dropping one.
  virtual void shadow_ids(std::vector<ClientOrderId>& out) = 0;
  virtual void drop_shadow(ClientOrderId id) = 0;

 protected:
  ~ReconcileHooks() = default;
};

class ReconcileDriver {
 public:
  static constexpr std::int64_t kRetryNs = 5'000'000'000;
  static constexpr std::int64_t kFetchTimeoutNs = 60'000'000'000;

  ReconcileDriver(ReconcileHooks& hooks, SentWatermark& sent);

  void attach(std::string_view name, VenueId venue, EventSink* sink);
  // connect(): requests are served from now on, when `enabled` (not a dry run, keys usable).
  void open(bool enabled) noexcept;
  // disconnect(): everything in progress is dropped and nothing is asked until open().
  void close() noexcept;

  // A reconciliation (reconnect, the engine's ControlCommand::Reconcile, an order whose fate is
  // unknown).
  void request();
  // The start-up sweep: a snapshot with an empty watermark. Any other request pending with it
  // turns it into an ordinary one, which finds the orders nobody holds as well.
  void sweep();
  // The replay replay_executions() started or joined is over; `complete` when it covered
  // everything since its start.
  void replay_done(bool complete);
  // The transport the fetch in progress went out on is gone: it is abandoned and asked again.
  void transport_lost();

  // Rows of the snapshot being fetched; valid between fetch_snapshot() and fetched().
  [[nodiscard]] bool current(std::uint64_t generation) const noexcept {
    return in_flight_ && generation == generation_;
  }
  // An OpenOrder row with its header set; the connector fills the rest.
  ReconcileMsg& add_order(InstrumentId instrument);
  void add_position(InstrumentId instrument, Qty qty, Price avg_px);
  // The fetch for `generation` is over: `ok` emits the snapshot, otherwise it is asked again after
  // kRetryNs. Ignored for a generation the driver moved on from.
  void fetched(std::uint64_t generation, bool ok);

  // The housekeeping timer: retries and fetch timeouts.
  void on_timer(std::int64_t now_ns);

  // A snapshot is being fetched or waits for its replay (position checks hold off meanwhile).
  [[nodiscard]] bool busy() const noexcept { return in_flight_ || waiting_replay_; }
  [[nodiscard]] bool retry_pending() const noexcept { return retry_at_ns_ != 0; }
  [[nodiscard]] std::uint64_t snapshots() const noexcept { return snapshots_; }
  [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }
  [[nodiscard]] std::uint64_t shadows_swept() const noexcept { return shadows_swept_; }

 private:
  // What is wanted, weakest first: a merge keeps the stronger.
  enum class Want : std::uint8_t { None = 0, Sweep = 1, Full = 2 };

  void want(Want w);
  void start();
  void fetch();
  void fail(std::string_view why);
  void emit();
  void sweep_shadows();

  ReconcileHooks& hooks_;
  SentWatermark& sent_;
  std::string name_;
  VenueId venue_{};
  EventSink* sink_ = nullptr;
  bool open_ = false;
  bool waiting_replay_ = false;   // replay_executions() started; the snapshot waits for it
  bool in_flight_ = false;        // fetch_snapshot() sent, fetched() not yet
  bool exact_ = false;            // kExecutionsExact for the snapshot in progress
  Want pending_ = Want::None;     // asked for, not started yet
  Want running_ = Want::None;     // the one in progress (replay or fetch)
  Want retry_want_ = Want::None;  // what the retry asks for
  std::uint64_t generation_ = 0;
  std::int64_t fetch_started_ns_ = 0;
  std::int64_t retry_at_ns_ = 0;  // 0: none
  ClientOrderId watermark_{};
  std::uint64_t snapshots_ = 0;
  std::uint64_t failures_ = 0;
  std::uint64_t shadows_swept_ = 0;
  std::vector<ReconcileMsg> orders_;
  std::vector<ReconcileMsg> positions_;
  std::vector<ClientOrderId> named_;    // the snapshot's client order ids, sorted
  std::vector<ClientOrderId> shadows_;  // shadow_ids() scratch
};

}  // namespace fastmm::venues
