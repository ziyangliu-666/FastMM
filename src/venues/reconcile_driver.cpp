#include "fastmm/venues/reconcile_driver.hpp"

#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/reactor.hpp"

#include <algorithm>

namespace fastmm::venues {

namespace {

constexpr std::size_t kReservedRows = 1024;

}  // namespace

ReconcileDriver::ReconcileDriver(ReconcileHooks& hooks, SentWatermark& sent)
    : hooks_(hooks), sent_(sent) {
  orders_.reserve(kReservedRows);
  positions_.reserve(kMaxInstruments);
  named_.reserve(kReservedRows);
  shadows_.reserve(kReservedRows);
}

void ReconcileDriver::attach(std::string_view name, VenueId venue, EventSink* sink) {
  name_ = std::string(name);
  venue_ = venue;
  sink_ = sink;
}

void ReconcileDriver::open(bool enabled) noexcept {
  open_ = enabled && sink_ != nullptr;
}

void ReconcileDriver::close() noexcept {
  open_ = false;
  ++generation_;  // a reply the shutdown aborts is not this driver's any more
  waiting_replay_ = false;
  in_flight_ = false;
  pending_ = Want::None;
  running_ = Want::None;
  retry_want_ = Want::None;
  retry_at_ns_ = 0;
  orders_.clear();
  positions_.clear();
}

void ReconcileDriver::request() {
  want(Want::Full);
}

void ReconcileDriver::sweep() {
  want(Want::Sweep);
}

void ReconcileDriver::want(Want w) {
  if (!open_) return;
  pending_ = std::max(pending_, w);
  // In flight: the next one starts when it is in. Waiting for the replay: the snapshot it releases
  // is asked for after this request, so it serves it.
  if (busy()) return;
  start();
}

void ReconcileDriver::start() {
  running_ = pending_;
  pending_ = Want::None;
  retry_at_ns_ = 0;  // this attempt replaces the retry; its own failure schedules the next
  retry_want_ = Want::None;
  // Latched before the replay starts: a replay that cannot send anything finishes inside
  // replay_executions() and releases the snapshot there.
  waiting_replay_ = true;
  if (hooks_.replay_executions() || !waiting_replay_) return;
  waiting_replay_ = false;
  exact_ = false;
  fetch();
}

void ReconcileDriver::replay_done(bool complete) {
  if (!waiting_replay_) return;  // a replay nobody waited for (the periodic one)
  waiting_replay_ = false;
  exact_ = complete;
  fetch();
}

void ReconcileDriver::fetch() {
  running_ = std::max(running_, pending_);
  pending_ = Want::None;
  const std::int64_t now = net::Reactor::now_ns();
  const std::uint64_t gen = ++generation_;
  in_flight_ = true;
  fetch_started_ns_ = now;
  watermark_ = running_ == Want::Sweep ? ClientOrderId{} : sent_.value(now);
  orders_.clear();
  positions_.clear();
  if (hooks_.fetch_snapshot(gen)) return;
  if (current(gen)) fail("could not be asked for");
}

void ReconcileDriver::fail(std::string_view why) {
  ++failures_;
  ++generation_;
  in_flight_ = false;
  waiting_replay_ = false;
  retry_want_ = std::max({running_, pending_, retry_want_});
  running_ = Want::None;
  pending_ = Want::None;
  orders_.clear();
  positions_.clear();
  retry_at_ns_ = net::Reactor::now_ns() + kRetryNs;
  FASTMM_LOG_WARN("{}: open-order snapshot {}; asking again in {} s", name_, why, kRetryNs / 1e9);
}

void ReconcileDriver::transport_lost() {
  if (busy()) fail("lost with its connection");
}

ReconcileMsg& ReconcileDriver::add_order(InstrumentId instrument) {
  ReconcileMsg& m = orders_.emplace_back();
  init_header(m, EventType::Reconcile, instrument, venue_);
  m.kind = ReconcileMsg::Kind::OpenOrder;
  m.hdr.recv_ts = wall_now();
  return m;
}

void ReconcileDriver::add_position(InstrumentId instrument, Qty qty, Price avg_px) {
  ReconcileMsg& m = positions_.emplace_back();
  init_header(m, EventType::Reconcile, instrument, venue_);
  m.kind = ReconcileMsg::Kind::Position;
  m.position_qty = qty;
  m.avg_px = avg_px;
  m.hdr.recv_ts = wall_now();
}

void ReconcileDriver::fetched(std::uint64_t generation, bool ok) {
  if (!current(generation)) return;
  if (!ok) {
    fail("failed");
    return;
  }
  in_flight_ = false;
  emit();
  sweep_shadows();
  orders_.clear();
  positions_.clear();
  running_ = Want::None;
  ++snapshots_;
  if (pending_ != Want::None) start();
}

void ReconcileDriver::emit() {
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), venue_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, watermark_);
  if (exact_) begin.flags |= ReconcileMsg::kExecutionsExact;
  exact_ = false;
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(sink_->push(begin.hdr));
  for (const ReconcileMsg& m : orders_) static_cast<void>(sink_->push(m.hdr));
  for (const ReconcileMsg& m : positions_) static_cast<void>(sink_->push(m.hdr));
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), venue_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(sink_->push(end.hdr));
  FASTMM_LOG_INFO("{}: reconciled {} open orders{}",
                  name_,
                  orders_.size(),
                  watermark_.valid() ? "" : " (start-up sweep)");
}

// An order's shadow is dropped when its terminal event arrives. When that event is lost with a
// connection - the REST cancel-all that follows an order-channel drop is the usual way - the
// shadow stays, and the table is fixed-size: enough of them and a new order gets no shadow, so its
// replace and cancel are refused as "original unknown". The snapshot settles it: an order the venue
// does not hold, answered before the snapshot was asked for, is over. "Answered before" is what the
// watermark says, which is a point in send order; ids compare in send order within one epoch only
// (fastmm-gateway interleaves several engines' ids), so a shadow of another epoch is left alone.
void ReconcileDriver::sweep_shadows() {
  if (!watermark_.valid()) return;  // the start-up sweep: nothing tells old from in flight
  named_.clear();
  for (const ReconcileMsg& m : orders_) {
    if (m.cl_ord_id.valid()) named_.push_back(m.cl_ord_id);
  }
  std::sort(named_.begin(), named_.end());
  shadows_.clear();
  hooks_.shadow_ids(shadows_);
  const std::uint16_t epoch = cl_ord_id_epoch(watermark_);
  std::uint64_t dropped = 0;
  for (const ClientOrderId id : shadows_) {
    if (cl_ord_id_epoch(id) != epoch || id.value > watermark_.value) continue;
    if (std::binary_search(named_.begin(), named_.end(), id)) continue;
    hooks_.drop_shadow(id);
    ++dropped;
  }
  if (dropped == 0) return;
  shadows_swept_ += dropped;
  FASTMM_LOG_INFO("{}: dropped {} order shadow(s) the venue no longer holds", name_, dropped);
}

void ReconcileDriver::on_timer(std::int64_t now_ns) {
  if (in_flight_ && now_ns - fetch_started_ns_ >= kFetchTimeoutNs) fail("got no answer");
  if (retry_at_ns_ != 0 && now_ns >= retry_at_ns_) {
    const Want w = retry_want_;
    retry_at_ns_ = 0;
    retry_want_ = Want::None;
    want(w == Want::None ? Want::Full : w);
  }
}

}  // namespace fastmm::venues
