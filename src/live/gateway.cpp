// fastmm-gateway: the venue connections and their network threads, and the strategy processes
// attached over shared-memory rings (live/gateway.hpp).
#include "fastmm/live/gateway.hpp"

#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"

#include <fmt/format.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fastmm::live {

namespace {

volatile std::sig_atomic_t g_gw_signal = 0;
extern "C" void on_gw_signal(int sig) {
  g_gw_signal = sig;
}

struct GatewaySignals {
  struct sigaction old_int {};
  struct sigaction old_term {};
  GatewaySignals() {
    g_gw_signal = 0;
    struct sigaction sa {};
    sa.sa_handler = &on_gw_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
  }
  GatewaySignals(const GatewaySignals&) = delete;
  GatewaySignals& operator=(const GatewaySignals&) = delete;
  GatewaySignals(GatewaySignals&&) = delete;
  GatewaySignals& operator=(GatewaySignals&&) = delete;
  ~GatewaySignals() {
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
  }
};

// Client order ids (New or Replace) forwarded to one venue, newest last, to find where a
// reconciliation's sent watermark stands.
constexpr std::size_t kHistory = 1U << 14;
// Orders working at one venue, over every attachment.
constexpr std::size_t kOrderTable = 1U << 14;
// Pauses before an attachment's full order ring counts as an overflow (a few ms): the network
// thread serves every attachment, so it cannot wait on one for long.
constexpr std::uint32_t kOrderSpin = 100'000;
// A venue's books are resnapshotted for an attachment whose md ring dropped at most this often.
constexpr std::int64_t kResyncIntervalNs = 1'000'000'000;

struct Attachment;

// What one venue's network thread keeps about one attachment. It lives in the Attachment and is
// added to and removed from the venue's router by tasks posted to that thread; only the atomics
// are read elsewhere (the main thread's checks and logs).
struct Route {
  Attachment* att = nullptr;
  std::uint16_t epoch = 0;
  ShmRing* md = nullptr;
  ShmRing* order = nullptr;
  ShmRing* out = nullptr;
  Waker* waker = nullptr;         // the engine's, when it blocks while idle
  bool dirty = false;             // events routed here since the last notify
  bool snapshot_pending = false;  // it asked for a reconciliation that has not begun
  bool in_snapshot = false;       // it receives the reconciliation being routed
  bool md_gap = false;            // its md ring dropped: nothing more until its Resyncing went out
  std::atomic<std::uint64_t> md_dropped{0};
  std::atomic<bool> overflow{false};  // its order ring stayed full: it has to go
  // Where its own history starts (venue ms): the replay start it sent, or its attach time when it
  // restored nothing. An execution naming no live order from before that is its store's (or none
  // of its business), and the trade ids its store listed in the overlap before it are too.
  std::int64_t replay_from_ms = 0;
  const std::unordered_set<std::string>* known = nullptr;
};

// One order the gateway forwarded (or a reconciliation reported), for the account guards and for
// cancelling a detached strategy's orders.
struct GwOrder {
  static constexpr std::uint8_t kCancelSent = 1U << 0;
  InstrumentId inst{};
  std::uint16_t epoch = 0;
  std::uint8_t flags = 0;
  Price price{};
  std::int64_t notional = 0;           // raw Notional of the working quantity
  std::int64_t replaced_notional = 0;  // of the order this one replaces, restored on a reject
  std::uint64_t g = 0;                 // 1-based forward index; 1 when learned from a snapshot
  std::uint32_t gen = 0;               // the last reconciliation that reported it
  ClientOrderId replaces{};
  VenueOrderId venue_order_id{};
};

struct CancelReq {
  ClientOrderId id;
  InstrumentId inst;
  VenueOrderId venue_order_id;
};

// A venue's router: its network thread's view of the attachments and of the orders at the venue.
// Everything but the atomics belongs to that thread.
struct VenueRouter {
  VenueSlot* slot = nullptr;
  VenueId vid{};
  const InstrumentTable* insts = nullptr;
  StaticVector<Route*, gw::kMaxAttachments> routes;
  std::array<Route*, kMaxInstruments> owner{};

  std::vector<ClientOrderId> hist = std::vector<ClientOrderId>(kHistory);
  std::uint64_t forwarded = 0;
  OpenHashMap<ClientOrderId, GwOrder, kOrderTable> orders;
  std::int64_t open_notional = 0;
  TokenBucket rate;
  std::int64_t max_open_notional = 0;  // raw; 0: off

  // The reconciliation being routed.
  bool snap_bounded = false;
  bool snap_known = false;  // the sent watermark was found in hist
  std::uint64_t snap_p = 0;
  std::uint32_t gen = 0;
  bool sweep_pending = false;   // the gateway asked for one (a detach): nobody receives it
  bool quiet_md_state = false;  // inside a resync_books() the gateway called
  bool want_resync = false;
  std::int64_t last_resync_ns = 0;
  std::vector<CancelReq> cancels;  // the gateway's own, sent by the hook

  std::atomic<std::uint64_t> md_discarded{0};
  std::atomic<std::uint64_t> order_discarded{0};
  std::atomic<std::uint64_t> unrouted{0};
  std::atomic<std::uint64_t> gateway_cancels{0};
  std::atomic<std::uint64_t> refused_rate{0};
  std::atomic<std::uint64_t> refused_notional{0};
  std::atomic<std::uint64_t> refused_owner{0};
  std::atomic<std::uint64_t> untracked{0};      // the order table was full
  std::atomic<std::uint64_t> stale_replays{0};  // replayed fills older than their owner's history

  [[nodiscard]] Route* by_epoch(ClientOrderId id) const noexcept {
    const std::uint16_t e = cl_ord_id_epoch(id);
    if (e == 0) return nullptr;
    for (Route* r : routes) {
      if (r->epoch == e) return r;
    }
    return nullptr;
  }
  [[nodiscard]] Route* owner_of(InstrumentId id) const noexcept {
    return id.value < owner.size() ? owner[id.value] : nullptr;
  }
};

// ---- network thread: inbound ------------------------------------------------------------------

void push_md(Route& r, const EventHeader& h) noexcept {
  if (r.md_gap) {
    r.md_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!r.md->try_push(&h, h.len)) {
    r.md_gap = true;
    r.md_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  r.dirty = true;
}

// Order events are never dropped: a strategy that stops reading its ring is detached.
void push_order(Route& r, const EventHeader& h) noexcept {
  if (r.overflow.load(std::memory_order_relaxed)) return;
  for (std::uint32_t i = 0; !r.order->try_push(&h, h.len); ++i) {
    if (i == kOrderSpin) {
      r.overflow.store(true, std::memory_order_release);
      return;
    }
    _mm_pause();
  }
  r.dirty = true;
}

// EventSink drain hook of the md sink: every event to every attachment.
void drain_md(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  MsgRing& ring = *v.slot->md_ring;
  while (const std::byte* p = ring.try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    if (v.routes.empty()) {
      v.md_discarded.fetch_add(1, std::memory_order_relaxed);
    } else if (h.type != EventType::ConnectionState || !v.quiet_md_state) {
      for (Route* r : v.routes) push_md(*r, h);
    }
    ring.release();
  }
}

void queue_cancel(VenueRouter& v, ClientOrderId id, InstrumentId inst, const VenueOrderId& voi) {
  if (GwOrder* o = v.orders.find(id)) {
    o->flags |= GwOrder::kCancelSent;
  }
  v.cancels.push_back(CancelReq{id, inst, voi});
}

void untrack(VenueRouter& v, ClientOrderId id) noexcept {
  if (const GwOrder* o = v.orders.find(id)) {
    v.open_notional -= o->notional;
    v.orders.erase(id);
  }
}

// A reconciliation: Begin, rows, End. Each recipient gets its own rows under a Begin carrying its
// own watermark; rows of an epoch nobody holds are cancelled.
void route_reconcile(VenueRouter& v, const ReconcileMsg& m) {
  switch (m.kind) {
    case ReconcileMsg::Kind::Begin: {
      ++v.gen;
      v.snap_bounded = (m.flags & ReconcileMsg::kSentWatermark) != 0;
      v.snap_known = false;
      v.snap_p = 0;
      if (v.snap_bounded) {
        if (!m.sent_watermark.valid()) {
          v.snap_known = true;  // nothing had been taken
        } else {
          const std::uint64_t lo = v.forwarded > kHistory ? v.forwarded - kHistory : 0;
          for (std::uint64_t j = v.forwarded; j > lo; --j) {
            if (v.hist[(j - 1) % kHistory] == m.sent_watermark) {
              v.snap_known = true;
              v.snap_p = j;
              break;
            }
          }
        }
      }
      bool any = false;
      for (Route* r : v.routes) any = any || r->snapshot_pending;
      const bool all = !any && !v.sweep_pending;
      v.sweep_pending = false;
      for (Route* r : v.routes) {
        if (!all && !r->snapshot_pending) continue;
        r->snapshot_pending = false;
        r->in_snapshot = true;
        ReconcileMsg b = m;
        if (v.snap_bounded) {
          // Its last order the venue had taken when it asked; none (or not findable): nothing of
          // its can be concluded from the snapshot, which is the safe side.
          ClientOrderId wm{};
          if (v.snap_known) {
            const std::uint64_t lo = v.forwarded > kHistory ? v.forwarded - kHistory : 0;
            for (std::uint64_t j = v.snap_p; j > lo; --j) {
              const ClientOrderId id = v.hist[(j - 1) % kHistory];
              if (cl_ord_id_epoch(id) == r->epoch) {
                wm = id;
                break;
              }
            }
          }
          b.sent_watermark = wm;
        }
        push_order(*r, b.hdr);
      }
      return;
    }
    case ReconcileMsg::Kind::OpenOrder: {
      const Qty leaves = m.orig_qty - m.cum_qty;
      if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
        o->gen = v.gen;
        if (!m.venue_order_id.empty()) o->venue_order_id = m.venue_order_id;
        if (v.insts->contains(m.hdr.instrument) && leaves.is_positive()) {
          const std::int64_t n = v.insts->get(m.hdr.instrument).notional(m.price, leaves).raw;
          v.open_notional += n - o->notional;
          o->notional = n;
        }
      }
      Route* r = v.by_epoch(m.cl_ord_id);
      if (r != nullptr) {
        if (v.orders.find(m.cl_ord_id) == nullptr && v.insts->contains(m.hdr.instrument)) {
          GwOrder o;
          o.inst = m.hdr.instrument;
          o.epoch = r->epoch;
          o.price = m.price;
          o.notional = leaves.is_positive()
                           ? v.insts->get(m.hdr.instrument).notional(m.price, leaves).raw
                           : 0;
          o.g = 1;
          o.gen = v.gen;
          o.venue_order_id = m.venue_order_id;
          if (v.orders.insert(m.cl_ord_id, o).second) v.open_notional += o.notional;
        }
        if (r->in_snapshot) push_order(*r, m.hdr);
        return;
      }
      // Nobody holds its epoch: a dead session's order, or one placed outside FastMM.
      queue_cancel(v, m.cl_ord_id, m.hdr.instrument, m.venue_order_id);
      return;
    }
    case ReconcileMsg::Kind::Position:
      if (Route* o = v.owner_of(m.hdr.instrument)) {
        push_order(*o, m.hdr);
      } else {
        v.unrouted.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    case ReconcileMsg::Kind::End: {
      for (Route* r : v.routes) {
        if (!r->in_snapshot) continue;
        r->in_snapshot = false;
        push_order(*r, m.hdr);
      }
      // Orders the snapshot no longer holds and that had reached the venue when it was asked.
      if (!v.snap_bounded || v.snap_known) {
        std::vector<ClientOrderId> gone;
        v.orders.for_each_key([&](ClientOrderId id) {
          const GwOrder* o = v.orders.find(id);
          if (o->gen != v.gen && (!v.snap_bounded || o->g <= v.snap_p)) gone.push_back(id);
        });
        for (const ClientOrderId id : gone) untrack(v, id);
      }
      return;
    }
  }
}

void route_fill(VenueRouter& v, const OrderFillMsg& m) {
  const bool replayed = (m.flags & OrderFillMsg::kReplayed) != 0;
  if (!replayed) {
    if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
      if (m.leaves_qty.is_zero()) {
        untrack(v, m.cl_ord_id);
      } else if (v.insts->contains(o->inst)) {
        const std::int64_t n = v.insts->get(o->inst).notional(o->price, m.leaves_qty).raw;
        v.open_notional += n - o->notional;
        o->notional = n;
      }
    }
  }
  // Streamed or replayed alike: to the order's epoch, else to the instrument's owner. A replay one
  // attachment's attach started also names the others' executions; each books only those its
  // OMS has not (it dedupes by venue execution id, instrument and side), and among them is a fill
  // its private stream missed.
  Route* r = v.by_epoch(m.cl_ord_id);
  if (r == nullptr) {
    r = v.owner_of(m.hdr.instrument);
    // Naming no live order, it goes to the owner only if the owner's store cannot hold it
    // already: a replay another attach started can reach back before this owner's history began,
    // and its engine never saw those executions, so it would book them a second time. (A
    // connector that gives no trade time, exch_ts 0, is not filtered.)
    if (r != nullptr && replayed && m.hdr.exch_ts.ns > 0 &&
        (m.hdr.exch_ts.ns / 1'000'000 < r->replay_from_ms ||
         (r->known != nullptr && r->known->contains(std::string(m.exec_id.view()))))) {
      v.stale_replays.fetch_add(1, std::memory_order_relaxed);
      FASTMM_LOG_INFO(
          "gateway: replayed execution {} ({} ms) is older than its owner's history ({} ms) or "
          "in its store: not routed",
          m.exec_id.view(),
          m.hdr.exch_ts.ns / 1'000'000,
          r->replay_from_ms);
      return;
    }
  }
  if (r != nullptr) {
    push_order(*r, m.hdr);
  } else {
    v.unrouted.fetch_add(1, std::memory_order_relaxed);
  }
}

// An event about one order goes to the attachment its epoch names; a dead session's is dropped.
template <class M>
void route_by_id(VenueRouter& v, const M& m) {
  if (Route* r = v.by_epoch(m.cl_ord_id)) {
    push_order(*r, m.hdr);
  } else {
    v.unrouted.fetch_add(1, std::memory_order_relaxed);
  }
}

void route_order(VenueRouter& v, const EventHeader& h) {
  switch (h.type) {
    case EventType::OrderAck: {
      const auto& m = msg_cast<OrderAckMsg>(&h);
      bool cancel_sent = false;
      if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
        o->venue_order_id = m.venue_order_id;
        cancel_sent = (o->flags & GwOrder::kCancelSent) != 0;
        if (o->replaces.valid()) {
          untrack(v, o->replaces);
          if (GwOrder* again = v.orders.find(m.cl_ord_id)) again->replaces = ClientOrderId{};
        }
      }
      if (Route* r = v.by_epoch(m.cl_ord_id)) {
        push_order(*r, h);
      } else if (!cancel_sent) {
        // Sent by a strategy that has gone since: it rests unmanaged, so it goes.
        queue_cancel(v, m.cl_ord_id, h.instrument, m.venue_order_id);
      }
      return;
    }
    case EventType::OrderReject: {
      const auto& m = msg_cast<OrderRejectMsg>(&h);
      if (const GwOrder* o = v.orders.find(m.cl_ord_id)) {
        if (o->replaces.valid()) {
          if (GwOrder* orig = v.orders.find(o->replaces)) {
            orig->notional = o->replaced_notional;
            v.open_notional += o->replaced_notional;
          }
        }
      }
      untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderCancelAck: {
      const auto& m = msg_cast<OrderCancelAckMsg>(&h);
      untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderCancelReject: {
      const auto& m = msg_cast<OrderCancelRejectMsg>(&h);
      if (m.reason == RejectReason::VenueUnknownOrder) untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderExpired: {
      const auto& m = msg_cast<OrderExpiredMsg>(&h);
      untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderFill:
      route_fill(v, msg_cast<OrderFillMsg>(&h));
      return;
    case EventType::PositionUpdate:
      if (Route* o = v.owner_of(h.instrument)) {
        push_order(*o, h);
      } else {
        v.unrouted.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    case EventType::Reconcile:
      route_reconcile(v, msg_cast<ReconcileMsg>(&h));
      return;
    default:
      // Connection states, a venue kill, latency samples: every attachment.
      for (Route* r : v.routes) push_order(*r, h);
      return;
  }
}

// EventSink drain hook of the order sink.
void drain_orders(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  MsgRing& ring = *v.slot->order_ring;
  while (const std::byte* p = ring.try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    // With nothing attached the gateway still keeps its order table and cancels dead sessions'.
    if (v.routes.empty()) v.order_discarded.fetch_add(1, std::memory_order_relaxed);
    route_order(v, h);
    ring.release();
  }
}

// ---- network thread: outbound -----------------------------------------------------------------

void refuse(VenueRouter& v, Route& r, const EventHeader& h, ClientOrderId id, RejectReason why) {
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject, h.instrument, h.venue);
  m.cl_ord_id = id;
  m.reason = why;
  m.text.assign("fastmm-gateway");
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  push_order(r, m.hdr);
  switch (why) {
    case RejectReason::GatewayRateLimit:
      v.refused_rate.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayOpenNotional:
      v.refused_notional.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      v.refused_owner.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

// The account guards: the sender owns the instrument, the venue's order rate, the notional
// working at the venue. `replaced` is the working notional of the order a replace takes over.
RejectReason check_order(VenueRouter& v,
                         const Route& r,
                         InstrumentId inst,
                         Price px,
                         Qty qty,
                         std::int64_t replaced,
                         std::int64_t* notional) {
  if (v.owner_of(inst) != &r || !v.insts->contains(inst)) return RejectReason::GatewayNotOwner;
  *notional = v.insts->get(inst).notional(px, qty).raw;
  if (v.max_open_notional > 0 && v.open_notional - replaced + *notional > v.max_open_notional)
    return RejectReason::GatewayOpenNotional;
  if (!v.rate.try_take(steady_now())) return RejectReason::GatewayRateLimit;
  return RejectReason::None;
}

void note_forwarded(VenueRouter& v, ClientOrderId id) noexcept {
  v.hist[v.forwarded % kHistory] = id;
  ++v.forwarded;
}

void track(VenueRouter& v, ClientOrderId id, const GwOrder& o) noexcept {
  if (v.orders.insert(id, o).second) {
    v.open_notional += o.notional;
  } else {
    v.untracked.fetch_add(1, std::memory_order_relaxed);
  }
}

// Moves one attachment's outbound messages into the venue's ring. Returns how many.
std::size_t forward(VenueRouter& v, Route& r) {
  MsgRing& dst = *v.slot->outbound;
  std::size_t n = 0;
  while (const std::byte* p = r.out->try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    std::byte* slot = dst.try_reserve(h.len);
    if (slot == nullptr) break;  // the venue catches up first
    bool send = true;
    switch (h.type) {
      case EventType::OutNewOrder: {
        const auto& m = msg_cast<OutNewOrderMsg>(&h);
        std::int64_t notional = 0;
        if (const RejectReason why = check_order(v, r, h.instrument, m.price, m.qty, 0, &notional);
            why != RejectReason::None) {
          refuse(v, r, h, m.cl_ord_id, why);
          send = false;
          break;
        }
        GwOrder o;
        o.inst = h.instrument;
        o.epoch = r.epoch;
        o.price = m.price;
        o.notional = notional;
        note_forwarded(v, m.cl_ord_id);
        o.g = v.forwarded;
        track(v, m.cl_ord_id, o);
        break;
      }
      case EventType::OutReplace: {
        const auto& m = msg_cast<OutReplaceMsg>(&h);
        GwOrder* orig = v.orders.find(m.orig_cl_ord_id);
        const std::int64_t replaced = orig != nullptr ? orig->notional : 0;
        std::int64_t notional = 0;
        if (const RejectReason why =
                check_order(v, r, h.instrument, m.price, m.qty, replaced, &notional);
            why != RejectReason::None) {
          refuse(v, r, h, m.cl_ord_id, why);
          send = false;
          break;
        }
        if (orig != nullptr) {
          v.open_notional -= orig->notional;
          orig->notional = 0;
        }
        GwOrder o;
        o.inst = h.instrument;
        o.epoch = r.epoch;
        o.price = m.price;
        o.notional = notional;
        o.replaces = m.orig_cl_ord_id;
        o.replaced_notional = replaced;
        o.venue_order_id = m.venue_order_id;
        note_forwarded(v, m.cl_ord_id);
        o.g = v.forwarded;
        track(v, m.cl_ord_id, o);
        break;
      }
      case EventType::Control:
        if (msg_cast<ControlMsg>(&h).command == ControlCommand::Reconcile)
          r.snapshot_pending = true;
        break;
      default:
        break;  // cancels always go
    }
    if (send) {
      std::memcpy(slot, p, h.len);
      dst.commit();
    }
    r.out->release();
    ++n;
  }
  return n;
}

// The gateway's own cancels (a detached strategy's orders, a dead session's rows and acks).
std::size_t send_cancels(VenueRouter& v) {
  if (v.cancels.empty()) return 0;
  std::vector<CancelReq> todo;
  todo.swap(v.cancels);
  std::size_t sent = 0;
  for (const CancelReq& c : todo) {
    if (sent == todo.size()) break;
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, c.inst, v.vid);
    m.cl_ord_id = c.id;
    m.venue_order_id = c.venue_order_id;
    if (!v.slot->outbound->try_push(&m, m.hdr.len)) {
      v.cancels.push_back(c);  // next round
      continue;
    }
    ++sent;
  }
  v.gateway_cancels.fetch_add(sent, std::memory_order_relaxed);
  return sent;
}

// An attachment whose md ring dropped gets a Resyncing state of its own once its ring has room
// (its engine clears the venue's books and pulls their quotes), then the books are snapshotted.
std::size_t recover_md(VenueRouter& v) {
  std::size_t n = 0;
  for (Route* r : v.routes) {
    if (!r->md_gap || r->md->bytes_used_approx() > r->md->capacity() / 2) continue;
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, InstrumentId::invalid(), v.vid);
    m.state = ConnState::Resyncing;
    m.channel = 0;
    m.hdr.recv_ts = wall_now();
    if (!r->md->try_push(&m, m.hdr.len)) continue;
    r->md_gap = false;
    r->dirty = true;
    v.want_resync = true;
    ++n;
  }
  if (v.want_resync) {
    const std::int64_t now = steady_now().ns;
    if (now - v.last_resync_ns >= kResyncIntervalNs) {
      v.want_resync = false;
      v.last_resync_ns = now;
      // The states it emits are not routed: only the attachments that lost data resync.
      v.quiet_md_state = true;
      v.slot->venue->resync_books();
      v.quiet_md_state = false;
      ++n;
    }
  }
  return n;
}

// VenueSlot::Hook, after every reactor iteration: the gateway's cancels, the attachments' orders
// through the account guards into the venue, md recovery, then the engines' wake-ups.
std::size_t gateway_hook(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  std::size_t moved = send_cancels(v);
  for (Route* r : v.routes) moved += forward(v, *r);
  if (moved != 0) v.slot->venue->on_wake();
  std::size_t n = moved;
  n += recover_md(v);
  for (Route* r : v.routes) {
    if (!r->dirty) continue;
    r->dirty = false;
    if (r->waker != nullptr) r->waker->notify();
    ++n;
  }
  return n;
}

// VenueSlot::Pending: the recheck of an adaptive network thread about to block. An engine takes
// the thread's flag in the gateway's wake page after it pushed an order
// (GatewayClient::wake_venue).
bool gateway_pending(void* ctx) noexcept {
  const auto& v = *static_cast<const VenueRouter*>(ctx);
  if (!v.cancels.empty()) return true;
  for (const Route* r : v.routes) {
    if (!r->out->empty_approx()) return true;
  }
  return false;
}

// ---- main thread ------------------------------------------------------------------------------

// Runs fn(i) on every venue's network thread and waits until all have run. The tasks reference
// the caller's frame, so this waits for as long as it takes (a wedged thread is logged).
void on_net_threads(VenueSlots& slots, const std::function<void(std::size_t)>& fn) {
  std::atomic<std::size_t> done{0};
  for (std::size_t i = 0; i < slots.size(); ++i) {
    slots[i]->reactor->post([&fn, &done, i] {
      fn(i);
      done.fetch_add(1, std::memory_order_release);
    });
    slots[i]->reactor->wake();
  }
  std::int64_t next_warn = steady_now().ns + 5'000'000'000;
  while (done.load(std::memory_order_acquire) != slots.size()) {
    sleep_for(microseconds(50));
    if (steady_now().ns >= next_warn) {
      next_warn += 5'000'000'000;
      FASTMM_LOG_ERROR("gateway: a network thread has not run a posted task for 5 s");
    }
  }
}

template <std::size_t N>
void to_field(char (&f)[N], std::string_view s) {
  std::memset(f, 0, N);
  std::memcpy(f, s.data(), std::min(s.size(), N - 1));
}

template <std::size_t N>
std::string from_field(const char (&f)[N]) {
  return std::string(f, ::strnlen(f, N));
}

void send_error(int fd, std::string_view why) {
  gw::AttachReply r{};
  r.hdr =
      gw::Header{gw::kMagic, gw::kVersion, gw::MsgType::AttachReply, sizeof(gw::AttachReply), 0};
  r.status = 1;
  r.gateway_pid = static_cast<std::uint32_t>(::getpid());
  to_field(r.error, why);
  static_cast<void>(::send(fd, &r, sizeof r, MSG_NOSIGNAL));
}

// One attached strategy.
struct Attachment {
  int fd = -1;
  std::uint32_t id = 0;
  std::string engine;
  std::uint32_t pid = 0;
  std::int64_t since_ns = 0;
  std::uint16_t epoch = 0;
  std::vector<InstrumentId> owned;
  struct Rings {
    std::unique_ptr<ShmRing> md;
    std::unique_ptr<ShmRing> order;
    std::unique_ptr<ShmRing> outbound;
    std::array<std::string, 3> paths;
  };
  std::vector<Rings> rings;         // per venue
  std::unique_ptr<Route[]> routes;  // per venue
  std::vector<std::uint64_t> md_dropped_logged;
  std::unordered_set<std::string> known;  // the trade ids its store listed (Route::known)
  std::int64_t replay_from_ms = 0;        // Route::replay_from_ms
  // The wake page (live/gateway.hpp) and this process's Waker over the engine's flag in it.
  gw::WakePage* page = nullptr;
  std::unique_ptr<Waker> engine_waker;

  [[nodiscard]] std::string who() const {
    return engine + " (pid " + std::to_string(pid) + ", attachment " + std::to_string(id) +
           ", epoch " + std::to_string(epoch) + ")";
  }
};

void remove_rings(const std::vector<Attachment::Rings>& rings) {
  std::error_code ec;
  for (const Attachment::Rings& r : rings)
    for (const std::string& p : r.paths)
      if (!p.empty()) std::filesystem::remove(p, ec);
}

class Gateway {
 public:
  Gateway(const Config& cfg,
          const GatewayOptions& opts,
          VenueSlots& slots,
          InstrumentTable& insts,
          gw::WakePage* net_page,
          int net_page_fd)
      : cfg_(cfg), opts_(opts), slots_(slots), instruments_(insts), net_page_fd_(net_page_fd) {
    std::int64_t max_notional = 0;
    if (!cfg.gateway.max_open_notional.empty()) {
      if (const auto n = Notional::from_decimal(cfg.gateway.max_open_notional))
        max_notional = n->raw;
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
      auto v = std::make_unique<VenueRouter>();
      v->slot = slots[i].get();
      v->vid = VenueId{static_cast<std::uint8_t>(i)};
      v->insts = &insts;
      v->rate.configure(static_cast<std::uint32_t>(cfg.gateway.orders_per_sec),
                        static_cast<std::uint32_t>(cfg.gateway.burst),
                        steady_now());
      v->max_open_notional = max_notional;
      VenueSlot& s = *slots[i];
      s.hook = &gateway_hook;
      s.pending = &gateway_pending;
      s.hook_ctx = v.get();
      s.blocked = &net_page->net[i].flag;
      s.consumer = nullptr;  // the hook notifies each attachment's engine
      s.md_sink.set_drain_hook(&drain_md, v.get(), /*on_commit=*/true);
      s.order_sink.set_drain_hook(&drain_orders, v.get(), /*on_commit=*/true);
      routers_.push_back(std::move(v));
    }
  }

  [[nodiscard]] std::size_t attachments() const noexcept { return atts_.size(); }

  // pollfds for the attachments' connections, in atts_ order.
  void poll_fds(std::vector<pollfd>& fds) const {
    for (const auto& a : atts_) fds.push_back(pollfd{a->fd, POLLIN | POLLRDHUP, 0});
  }
  // The connection of atts_[k] has something to say (it closed, as a rule).
  void on_readable(std::size_t k) {
    Attachment& a = *atts_[k];
    char b[64];
    const ssize_t n = ::recv(a.fd, b, sizeof b, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
      detach(a, n == 0 ? "its connection closed" : std::strerror(errno));
    } else if (n > 0) {
      FASTMM_LOG_WARN("gateway: ignored {} byte(s) from {}", n, std::string_view(a.who()));
    }
  }

  // A new connection on the listener. Answers it and keeps it as an attachment, or refuses it.
  void on_connection(int fd) {
    // The request follows the connect at once; a client that sends nothing is dropped.
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 2000) <= 0) {
      FASTMM_LOG_WARN("gateway: a connection sent no attach request");
      ::close(fd);
      return;
    }
    std::vector<std::byte> buf(gw::kMaxRequest);
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    gw::AttachRequest req{};
    if (n < static_cast<ssize_t>(sizeof req)) {
      send_error(fd, "short or missing attach request");
      ::close(fd);
      return;
    }
    std::memcpy(&req, buf.data(), sizeof req);
    if (req.hdr.magic != gw::kMagic || req.hdr.type != gw::MsgType::AttachRequest) {
      send_error(fd, "not an attach request");
      ::close(fd);
      return;
    }
    if (req.hdr.version != gw::kVersion) {
      send_error(fd,
                 "gateway protocol version " + std::to_string(gw::kVersion) + ", request has " +
                     std::to_string(req.hdr.version));
      ::close(fd);
      return;
    }
    if (req.known_count > gw::kMaxKnownExecIds || req.claim_count > kMaxInstruments ||
        static_cast<std::size_t>(n) != sizeof req + req.known_count * sizeof(gw::ExecId) +
                                           req.claim_count * sizeof(gw::InstrumentClaim)) {
      send_error(fd, "malformed attach request");
      ::close(fd);
      return;
    }
    const std::string engine = from_field(req.engine);
    const auto refuse_attach = [&](const std::string& why) {
      FASTMM_LOG_WARN("gateway: refused {} (pid {}): {}",
                      std::string_view(engine),
                      req.pid,
                      std::string_view(why));
      send_error(fd, why);
      ::close(fd);
    };
    if (atts_.size() >= gw::kMaxAttachments) {
      refuse_attach("the gateway has " + std::to_string(gw::kMaxAttachments) +
                    " strategies attached already");
      return;
    }
    std::vector<std::string> known;
    known.reserve(req.known_count);
    for (std::uint32_t i = 0; i < req.known_count; ++i) {
      gw::ExecId id{};
      std::memcpy(&id, buf.data() + sizeof req + i * sizeof id, sizeof id);
      known.push_back(from_field(id.id));
    }
    // The instruments it trades: each in the gateway's table and owned by no one else.
    std::vector<InstrumentId> owned;
    const std::byte* claims = buf.data() + sizeof req + req.known_count * sizeof(gw::ExecId);
    for (std::uint32_t i = 0; i < req.claim_count; ++i) {
      gw::InstrumentClaim cl{};
      std::memcpy(&cl, claims + i * sizeof cl, sizeof cl);
      const std::string venue = from_field(cl.venue);
      const std::string symbol = from_field(cl.symbol);
      const VenueId vid = cfg_.venue_id(venue);
      const Instrument* inst = vid.valid() ? instruments_.find(vid, symbol) : nullptr;
      if (inst == nullptr) {
        refuse_attach(fmt::format(
            "instrument {} on venue '{}' is not in the gateway's configuration", symbol, venue));
        return;
      }
      if (const Attachment* other = owner_[inst->id.value]) {
        refuse_attach(fmt::format(
            "instrument {} on venue '{}' is traded by {}", symbol, venue, other->who()));
        return;
      }
      if (std::find(owned.begin(), owned.end(), inst->id) == owned.end()) owned.push_back(inst->id);
    }
    if (owned.empty()) {
      refuse_attach("the attach request names no instrument to trade");
      return;
    }
    attach(fd, req, engine, known, std::move(owned));
  }

  // The attachment's connection closed (or misbehaved): the strategy is gone.
  void detach(Attachment& a, std::string_view why) {
    const std::int64_t t0 = steady_now().ns;
    std::vector<std::size_t> cancelled(slots_.size(), 0);
    // Out of the routers first, so nothing of its reaches the wire any more; then its orders go.
    on_net_threads(slots_, [&](std::size_t i) {
      VenueRouter& v = *routers_[i];
      Route* r = &a.routes[i];
      for (std::size_t k = 0; k < v.routes.size(); ++k) {
        if (v.routes[k] == r) {
          v.routes.erase_at(k);
          break;
        }
      }
      for (Route*& o : v.owner) {
        if (o == r) o = nullptr;
      }
      if (opts_.dry_run) return;
      std::vector<ClientOrderId> mine;
      v.orders.for_each_key([&](ClientOrderId id) {
        const GwOrder* o = v.orders.find(id);
        if (o->epoch == a.epoch && (o->flags & GwOrder::kCancelSent) == 0) mine.push_back(id);
      });
      for (const ClientOrderId id : mine) {
        const GwOrder o = *v.orders.find(id);
        queue_cancel(v, id, o.inst, o.venue_order_id);
      }
      cancelled[i] = mine.size();
      if (send_cancels(v) != 0) v.slot->venue->on_wake();
      // And ask the venue what it holds: a row of this epoch that the gateway did not know is
      // cancelled when the snapshot comes back.
      v.sweep_pending = true;
      v.slot->venue->request_open_orders();
    });
    gw::unmap_wake_page(a.page);
    a.page = nullptr;
    a.engine_waker.reset();
    remove_rings(a.rings);
    a.rings.clear();
    ::close(a.fd);
    a.fd = -1;
    for (const InstrumentId id : a.owned) owner_[id.value] = nullptr;
    std::size_t total = 0;
    for (const std::size_t c : cancelled) total += c;
    const std::int64_t ms = (steady_now().ns - t0) / 1'000'000;
    const double held_s = static_cast<double>(steady_now().ns - a.since_ns) / 1e9;
    FASTMM_LOG_WARN(
        "gateway: {} detached after {:.1f} s ({}): {} order(s) of its epoch cancelled, sent in {} "
        "ms; its {} instrument(s) are free",
        std::string_view(a.who()),
        held_s,
        why,
        total,
        ms,
        a.owned.size());
    for (std::size_t k = 0; k < atts_.size(); ++k) {
      if (atts_[k].get() == &a) {
        atts_.erase(atts_.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
    }
  }

  void detach_all(std::string_view why) {
    while (!atts_.empty()) detach(*atts_.back(), why);
  }

  // An attachment stopped reading its order ring: the venue could not hand it an order event.
  void check_overflows() {
    for (std::size_t k = 0; k < atts_.size(); ++k) {
      Attachment& a = *atts_[k];
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!a.routes[i].overflow.load(std::memory_order_acquire)) continue;
        FASTMM_LOG_ERROR("gateway: [{}] the order ring of {} is full",
                         slots_[i]->venue->name(),
                         std::string_view(a.who()));
        detach(a, "its order ring overflowed");
        return;  // atts_ changed
      }
    }
  }

  // Once a second: what was dropped, refused or had nowhere to go.
  void log_counters() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      const VenueRouter& v = *routers_[i];
      const std::uint64_t md = v.md_discarded.load(std::memory_order_relaxed);
      const std::uint64_t order = v.order_discarded.load(std::memory_order_relaxed);
      const std::uint64_t unrouted = v.unrouted.load(std::memory_order_relaxed);
      const std::uint64_t cancels = v.gateway_cancels.load(std::memory_order_relaxed);
      const std::uint64_t rate = v.refused_rate.load(std::memory_order_relaxed);
      const std::uint64_t notional = v.refused_notional.load(std::memory_order_relaxed);
      const std::uint64_t owner = v.refused_owner.load(std::memory_order_relaxed);
      const std::uint64_t untracked = v.untracked.load(std::memory_order_relaxed);
      const std::uint64_t stale = v.stale_replays.load(std::memory_order_relaxed);
      Logged& l = logged_[i];
      if (md != l.md || order != l.order || unrouted != l.unrouted || cancels != l.cancels ||
          rate != l.rate || notional != l.notional || owner != l.owner ||
          untracked != l.untracked || stale != l.stale) {
        FASTMM_LOG_INFO(
            "gateway: [{}] discarded with nothing attached: md={} order={}; order events for no "
            "attachment: {}; gateway cancels: {}; refused: rate={} open_notional={} not_owner={}; "
            "untracked: {}; replayed fills older than their owner's history: {}",
            slots_[i]->venue->name(),
            md,
            order,
            unrouted,
            cancels,
            rate,
            notional,
            owner,
            untracked,
            stale);
        l = Logged{md, order, unrouted, cancels, rate, notional, owner, untracked, stale};
      }
      for (const auto& a : atts_) {
        const std::uint64_t d = a->routes[i].md_dropped.load(std::memory_order_relaxed);
        if (d == a->md_dropped_logged[i]) continue;
        FASTMM_LOG_WARN("gateway: [{}] {} could not keep up: {} market-data event(s) dropped",
                        slots_[i]->venue->name(),
                        std::string_view(a->who()),
                        d);
        a->md_dropped_logged[i] = d;
      }
    }
  }

 private:
  struct Logged {
    std::uint64_t md = 0;
    std::uint64_t order = 0;
    std::uint64_t unrouted = 0;
    std::uint64_t cancels = 0;
    std::uint64_t rate = 0;
    std::uint64_t notional = 0;
    std::uint64_t owner = 0;
    std::uint64_t untracked = 0;
    std::uint64_t stale = 0;
  };

  // A session epoch no live attachment has, from the gateway's epoch file (fail closed).
  [[nodiscard]] Result<std::uint16_t, std::string> next_epoch() const {
    if (const std::filesystem::path p(cfg_.engine.epoch_file); p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    for (std::size_t tries = 0; tries <= gw::kMaxAttachments; ++tries) {
      bool wrapped = false;
      auto e = SessionEpochStore::next_epoch(cfg_.engine.epoch_file, &wrapped);
      if (!e) return e;
      if (wrapped)
        FASTMM_LOG_WARN(
            "gateway: the session epoch has cycled past 65535: client order ids of sessions that "
            "long ago can repeat");
      bool taken = false;
      for (const auto& a : atts_) taken = taken || a->epoch == *e;
      if (!taken) return *e;
    }
    return fail(std::string("every session epoch tried is held by a live attachment"));
  }

  void attach(int fd,
              const gw::AttachRequest& req,
              const std::string& engine,
              const std::vector<std::string>& known,
              std::vector<InstrumentId> owned) {
    const auto epoch = next_epoch();
    if (!epoch) {
      FASTMM_LOG_ERROR("gateway: cannot give {} a session epoch: {}",
                       std::string_view(engine),
                       std::string_view(epoch.error()));
      send_error(fd, "cannot assign a session epoch: " + epoch.error());
      ::close(fd);
      return;
    }
    const std::uint32_t id = ++next_id_;
    auto ap = std::make_unique<Attachment>();
    Attachment& a = *ap;
    a.id = id;
    a.engine = engine;
    a.pid = req.pid;
    a.epoch = *epoch;
    a.owned = std::move(owned);
    a.replay_from_ms =
        (req.flags & gw::kResumeExecutions) != 0 ? req.exec_since_ms : wall_now().ns / 1'000'000;
    a.known.insert(known.begin(), known.end());
    // Closed once the reply is sent (or on failure); the mappings keep the page.
    struct Fd {
      int fd;
      ~Fd() {
        if (fd >= 0) ::close(fd);
      }
    } page_fd{gw::create_wake_page()};
    if (page_fd.fd >= 0) a.page = gw::map_wake_page(page_fd.fd);
    if (a.page == nullptr) {
      const std::string why = std::strerror(errno);
      FASTMM_LOG_ERROR(
          "gateway: cannot create the wake page of attachment {}: {}", id, std::string_view(why));
      send_error(fd, "cannot create the wake page: " + why);
      ::close(fd);
      return;
    }
    a.engine_waker = std::make_unique<Waker>();
    a.engine_waker->share(&a.page->engine.flag);
    const bool strategy_blocks = (req.flags & gw::kStrategyBlocks) != 0;
    const std::string stem = "/dev/shm/fastmm-gw-" + cfg_.engine.name + "-" +
                             std::to_string(::getpid()) + "-" + std::to_string(id) + "-";
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      Attachment::Rings r;
      const std::string base = stem + std::string(slots_[i]->venue->name());
      r.paths = {base + ".md", base + ".ord", base + ".out"};
      auto md = ShmRing::create(r.paths[0], ring_size(cfg_.engine.md_ring_bytes));
      auto order = ShmRing::create(r.paths[1], ring_size(cfg_.engine.order_ring_bytes));
      auto out = ShmRing::create(r.paths[2], ring_size(cfg_.engine.order_ring_bytes));
      if (!md || !order || !out) {
        const std::string why = !md ? md.error() : !order ? order.error() : out.error();
        FASTMM_LOG_ERROR(
            "gateway: cannot create the rings of attachment {}: {}", id, std::string_view(why));
        send_error(fd, "cannot create the rings: " + why);
        ::close(fd);
        remove_rings(a.rings);
        std::error_code ec;
        for (const std::string& p : r.paths) std::filesystem::remove(p, ec);
        gw::unmap_wake_page(a.page);
        return;
      }
      r.md = std::make_unique<ShmRing>(std::move(*md));
      r.order = std::make_unique<ShmRing>(std::move(*order));
      r.outbound = std::make_unique<ShmRing>(std::move(*out));
      a.rings.push_back(std::move(r));
    }
    a.routes = std::make_unique<Route[]>(slots_.size());
    a.md_dropped_logged.assign(slots_.size(), 0);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      Route& r = a.routes[i];
      r.att = &a;
      r.epoch = a.epoch;
      r.md = a.rings[i].md.get();
      r.order = a.rings[i].order.get();
      r.out = a.rings[i].outbound.get();
      r.waker = strategy_blocks ? a.engine_waker.get() : nullptr;
      r.replay_from_ms = a.replay_from_ms;
      r.known = &a.known;
    }
    for (const InstrumentId inst : a.owned) owner_[inst.value] = &a;
    const bool resume = (req.flags & gw::kResumeExecutions) != 0;
    // On each network thread, between two of its callbacks: from here on the venue's events reach
    // the strategy's rings, starting with fresh books and the account's truth (executions, then
    // the open orders), so the strategy's OMS starts from what the venue holds.
    on_net_threads(slots_, [&](std::size_t i) {
      VenueRouter& v = *routers_[i];
      VenueSlot& s = *slots_[i];
      Route& r = a.routes[i];
      static_cast<void>(v.routes.push_back(&r));
      for (const InstrumentId inst : a.owned) {
        if (instruments_.get(inst).venue == v.vid) v.owner[inst.value] = &r;
      }
      r.snapshot_pending = true;
      // Its books start empty and fill from these snapshots; the others' books are replaced by
      // the same, so the Resyncing states the venue emits here reach nobody.
      v.quiet_md_state = true;
      s.venue->resync_books();
      v.quiet_md_state = false;
      if (resume && executions(i)) {
        s.venue->resume_executions(req.exec_since_ms, known);
        static_cast<void>(s.venue->request_executions(req.exec_since_ms));
      }
      s.venue->request_open_orders();
    });

    // The reply: header, venues, instruments.
    std::vector<std::byte> out(sizeof(gw::AttachReply) + slots_.size() * sizeof(gw::VenueInfo) +
                               instruments_.size() * sizeof(Instrument));
    gw::AttachReply rep{};
    rep.hdr = gw::Header{gw::kMagic,
                         gw::kVersion,
                         gw::MsgType::AttachReply,
                         static_cast<std::uint32_t>(out.size()),
                         0};
    rep.status = 0;
    rep.attach_id = id;
    rep.venue_count = static_cast<std::uint32_t>(slots_.size());
    rep.instrument_count = static_cast<std::uint32_t>(instruments_.size());
    rep.instrument_bytes = sizeof(Instrument);
    rep.gateway_pid = static_cast<std::uint32_t>(::getpid());
    rep.session_epoch = a.epoch;
    if (cfg_.spin_mode() == SpinMode::Adaptive) rep.flags |= gw::kGatewayBlocks;
    std::memcpy(out.data(), &rep, sizeof rep);
    std::byte* p = out.data() + sizeof rep;
    for (std::size_t i = 0; i < slots_.size(); ++i, p += sizeof(gw::VenueInfo)) {
      gw::VenueInfo vi{};
      vi.id = static_cast<std::uint8_t>(i);
      if (slots_[i]->venue->caps().supports_replace && cfg_.venues[i].supports_replace)
        vi.flags |= gw::kVenueReplace;
      if (executions(i)) vi.flags |= gw::kVenueExecutions;
      to_field(vi.name, slots_[i]->venue->name());
      to_field(vi.md_path, a.rings[i].paths[0]);
      to_field(vi.order_path, a.rings[i].paths[1]);
      to_field(vi.outbound_path, a.rings[i].paths[2]);
      std::memcpy(p, &vi, sizeof vi);
    }
    for (const Instrument& inst : instruments_) {
      std::memcpy(p, &inst, sizeof inst);
      p += sizeof inst;
    }
    a.fd = fd;
    a.since_ns = steady_now().ns;
    atts_.push_back(std::move(ap));
    std::vector<int> fds{page_fd.fd, net_page_fd_};
    for (const auto& s : slots_) fds.push_back(s->reactor->wake_fd());
    if (gw::send_with_fds(fd, out.data(), out.size(), fds) != static_cast<ssize_t>(out.size())) {
      FASTMM_LOG_ERROR("gateway: cannot answer {} (pid {}): {}",
                       std::string_view(engine),
                       req.pid,
                       std::strerror(errno));
      detach(a, "the attach reply could not be sent");
      return;
    }
    std::string symbols;
    for (const InstrumentId inst : a.owned) {
      if (!symbols.empty()) symbols += ",";
      symbols += std::string(instruments_.get(inst).symbol.view());
    }
    FASTMM_LOG_WARN(
        "gateway: {} attached: {} venue(s), trades {}, {} strategies attached, reconciling{}",
        std::string_view(a.who()),
        slots_.size(),
        std::string_view(symbols),
        atts_.size(),
        resume ? std::string_view(" from its store's last fill") : std::string_view());
  }

  [[nodiscard]] bool executions(std::size_t i) const {
    const venues::VenueEntry* e = venues::VenueRegistry::instance().find(cfg_.venues[i].kind);
    return e != nullptr && e->caps.executions;
  }

  const Config& cfg_;
  const GatewayOptions& opts_;
  VenueSlots& slots_;
  const InstrumentTable& instruments_;
  int net_page_fd_;
  std::vector<std::unique_ptr<VenueRouter>> routers_;
  std::vector<std::unique_ptr<Attachment>> atts_;
  std::array<const Attachment*, kMaxInstruments> owner_{};
  std::array<Logged, 8> logged_{};
  std::uint32_t next_id_ = 0;
};

}  // namespace

std::string default_gateway_path(const Config& cfg) {
  return cfg.engine.journal_dir + "/" + cfg.engine.name + ".gw";
}

int run_gateway(const Config& cfg, const GatewayOptions& opts) {
  const char* prog = opts.program.c_str();
  InstrumentTable instruments;
  try {
    instruments = load_instruments(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  if (instruments.size() == 0) {
    std::fprintf(stderr, "%s: no [[instruments]] configured\n", prog);
    return kExitConfig;
  }
  if (cfg.venues.empty() || cfg.venues.size() > 8) {
    std::fprintf(stderr, "%s: a gateway runs 1 to 8 venues\n", prog);
    return kExitConfig;
  }
  if (cfg.gateway.orders_per_sec < 0 || cfg.gateway.burst < 0) {
    std::fprintf(stderr, "%s: [gateway] orders_per_sec and burst cannot be negative\n", prog);
    return kExitConfig;
  }
  if (!cfg.gateway.max_open_notional.empty()) {
    const auto n = Notional::from_decimal(cfg.gateway.max_open_notional);
    if (!n || n->raw < 0) {
      std::fprintf(stderr,
                   "%s: [gateway] max_open_notional: '%s' is not a non-negative decimal\n",
                   prog,
                   cfg.gateway.max_open_notional.c_str());
      return kExitConfig;
    }
  }
  VenueSlots slots;
  venues::VenueFactoryOptions vopts;
  vopts.dry_run = opts.dry_run;
  vopts.busy_poll = cfg.spin_mode() == SpinMode::Busy;
  if (const int rc = make_venue_slots(cfg, vopts, instruments, prog, slots); rc != 0) return rc;
  venues::SymbolTable symbols;
  if (!symbols.build(instruments)) {
    std::fprintf(stderr, "%s: duplicate or empty instrument symbols\n", prog);
    return kExitConfig;
  }
  TscCalibrator tsc_calibrator;
  const Seqlocked<TscCalibration> tsc_pub(tsc_calibrator.start());
  const net::ReactorBackend net_backend = resolve_net_backend(cfg);
  for (std::size_t i = 0; i < slots.size(); ++i)
    wire_venue_slot(*slots[i],
                    VenueId{static_cast<std::uint8_t>(i)},
                    cfg,
                    net_backend,
                    symbols,
                    instruments,
                    &tsc_pub);

  // The network threads' sleeping flags, in a page every attachment maps.
  const int net_page_fd = gw::create_wake_page();
  gw::WakePage* const net_page = net_page_fd >= 0 ? gw::map_wake_page(net_page_fd) : nullptr;
  if (net_page == nullptr) {
    std::fprintf(stderr, "%s: cannot create the wake page: %s\n", prog, std::strerror(errno));
    return kExitConfig;
  }
  struct PageGuard {
    int fd;
    gw::WakePage* page;
    ~PageGuard() {
      gw::unmap_wake_page(page);
      ::close(fd);
    }
  } page_guard{net_page_fd, net_page};

  const std::string path = opts.socket_path.empty() ? default_gateway_path(cfg) : opts.socket_path;
  std::string err;
  const int listen_fd = listen_seqpacket(path, &err);
  if (listen_fd < 0) {
    std::fprintf(stderr, "%s: gateway socket %s: %s\n", prog, path.c_str(), err.c_str());
    return kExitConfig;
  }

  Gateway gateway(cfg, opts, slots, instruments, net_page, net_page_fd);
  const GatewaySignals signals;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const int cpu = i < cfg.engine.net_cpus.size() ? cfg.engine.net_cpus[i] : -1;
    slots[i]->thread = std::thread(net_loop, std::ref(*slots[i]), cpu, i, cfg.spin_mode());
  }
  FASTMM_LOG_INFO(
      "fastmm-gateway: {} venue(s), {} instrument(s), dry_run={}, net={}, orders_per_sec={}, "
      "max_open_notional={}; attach with fastmm-live --gateway {}",
      slots.size(),
      instruments.size(),
      opts.dry_run,
      net::to_string(net_backend),
      cfg.gateway.orders_per_sec,
      cfg.gateway.max_open_notional.empty() ? std::string_view("off")
                                            : std::string_view(cfg.gateway.max_open_notional),
      path);

  const std::int64_t start = steady_now().ns;
  std::int64_t next_tick = start + 1'000'000'000;
  std::vector<pollfd> fds;
  while (g_gw_signal == 0) {
    const std::int64_t now = steady_now().ns;
    if (opts.duration_ns > 0 && now - start >= opts.duration_ns) break;
    fds.clear();
    fds.push_back(pollfd{listen_fd, POLLIN, 0});
    gateway.poll_fds(fds);
    // Wakes at once when a strategy's connection closes; otherwise at least every 50 ms for the
    // signals, the overflow check and the once-a-second housekeeping.
    const int pr = ::poll(fds.data(), fds.size(), 50);
    if (pr > 0) {
      // Back to front: a detach removes its attachment, and the others keep their index.
      for (std::size_t k = fds.size() - 1; k >= 1; --k) {
        if (fds[k].revents != 0) gateway.on_readable(k - 1);
      }
      if ((fds[0].revents & POLLIN) != 0) {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) gateway.on_connection(fd);
      }
    }
    gateway.check_overflows();
    if (steady_now().ns >= next_tick) {
      next_tick += 1'000'000'000;
      for (auto& s : slots) {
        venues::Venue* v = s->venue.get();
        s->reactor->post([v] { v->on_timer(net::Reactor::now_ns()); });
        log_venue_status(*v);
      }
      gateway.log_counters();
    }
  }
  FASTMM_LOG_WARN("fastmm-gateway: shutting down ({})",
                  g_gw_signal != 0 ? std::string_view("signal") : std::string_view("duration"));
  ::close(listen_fd);
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  gateway.detach_all("the gateway is shutting down");
  bool cancel_ok = true;
  if (!opts.dry_run) {
    for (auto& s : slots) cancel_ok = s->venue->cancel_all() && cancel_ok;
  }
  for (auto& s : slots) {
    s->stop.store(true);
    s->reactor->wake();
  }
  for (auto& s : slots) {
    if (s->thread.joinable()) s->thread.join();
  }
  gateway.log_counters();
  for (auto& s : slots) {
    const venues::VenueStatus st = s->venue->status();
    FASTMM_LOG_INFO("[{}] final: md_msgs={} orders={} cancels={} order_events={} reconnects={}",
                    s->venue->name(),
                    st.md_messages,
                    st.orders_sent,
                    st.cancels_sent,
                    st.order_events,
                    st.reconnects);
    log_wire_latency(s->venue->name(), st, true);
  }
  FASTMM_LOG_INFO("fastmm-gateway: cancel_all {}", cancel_ok ? "ok" : "FAILED");
  return cancel_ok ? kExitOk : kExitRuntime;
}

}  // namespace fastmm::live
