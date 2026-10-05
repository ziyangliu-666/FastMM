#include "fastmm/live/venue_slot.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/live/session.hpp"

#include <x86intrin.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <exception>
#include <string>

namespace fastmm::live {

namespace {

void on_order_overflow(void* ctx, const venues::EventSink&) noexcept {
  static_cast<VenueSlot*>(ctx)->order_overflows.fetch_add(1, std::memory_order_relaxed);
}

// Adaptive spin: after the last activity the network thread keeps polling for this long before it
// blocks in the reactor (for at most kNetMaxBlockMs, the cadence of Venue::poll()), so the engine's
// reaction to an event it just delivered finds the thread awake. Not on a shared CPU (net_loop).
constexpr std::int64_t kNetSpinNs = 200'000;
constexpr int kNetMaxBlockMs = 1;

const char* short_state(venues::ChannelState s) {
  switch (s) {
    case venues::ChannelState::Down:
      return "down";
    case venues::ChannelState::Connecting:
      return "conn";
    case venues::ChannelState::Live:
      return "live";
    case venues::ChannelState::Stale:
      return "stale";
  }
  return "?";
}

}  // namespace

std::size_t ring_size(std::size_t bytes) {
  return std::bit_ceil(std::max<std::size_t>(bytes, 1U << 16));
}

int make_venue_slots(const Config& cfg,
                     const venues::VenueFactoryOptions& vopts,
                     InstrumentTable& instruments,
                     const char* prog,
                     VenueSlots& slots) {
  for (std::size_t i = 0; i < cfg.venues.size(); ++i) {
    auto slot = std::make_unique<VenueSlot>();
    try {
      venues::VenueFactoryOptions vo = vopts;
      vo.dry_run = vopts.dry_run || cfg.venues[i].public_only;  // a price leader runs public-only
      vo.pool_of = cfg.venue_id(cfg.venues[i].pool_of);  // a member: orders for the primary's
      vo.pooled = !cfg.venues[i].pool_of.empty() ||
                  std::any_of(cfg.venues.begin(), cfg.venues.end(), [&](const auto& v) {
                    return v.pool_of == cfg.venues[i].name;
                  });
      slot->venue = venues::make_venue(VenueId{static_cast<std::uint8_t>(i)}, cfg.venues[i], vo);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitConfig;
    }
    if (auto r = slot->venue->load_reference_data(instruments); !r) {
      std::fprintf(stderr, "%s: %s\n", prog, r.error().c_str());
      return slot->venue->refused_account_settings() ? kExitConfig : kExitVenue;
    }
    slots.push_back(std::move(slot));
  }
  return 0;
}

net::ReactorBackend resolve_net_backend(const Config& cfg) {
  net::ReactorBackend backend = net::ReactorBackend::Epoll;
  static_cast<void>(net::parse_reactor_backend(cfg.engine.net_backend, backend));  // validated
  if (net::Reactor::resolve_backend(backend) != backend) {
    FASTMM_LOG_WARN(
        "[engine] net_backend = \"io_uring\" but io_uring is not available (kernel too "
        "old, disabled or not permitted); falling back to epoll");
    backend = net::ReactorBackend::Epoll;
  }
  return backend;
}

void wire_venue_slot(VenueSlot& s,
                     VenueId vid,
                     const Config& cfg,
                     net::ReactorBackend backend,
                     const venues::SymbolTable& symbols,
                     const InstrumentTable& instruments,
                     const Seqlocked<TscCalibration>* tsc) {
  s.reactor = std::make_unique<net::Reactor>(backend);
  s.md_ring = std::make_unique<MsgRing>(ring_size(cfg.engine.md_ring_bytes));
  s.order_ring = std::make_unique<MsgRing>(ring_size(cfg.engine.order_ring_bytes));
  s.outbound = std::make_unique<MsgRing>(ring_size(cfg.engine.order_ring_bytes));
  s.md_sink.attach(s.md_ring.get(), venues::SinkPolicy::Drop);
  s.order_sink.attach(s.order_ring.get(), venues::SinkPolicy::Spin);
  s.order_sink.set_overflow_callback(&on_order_overflow, &s);
  s.venue->attach(symbols, instruments, s.md_sink, s.order_sink, s.outbound.get());
  s.venue->set_tsc_calibration_source(tsc);
  // A pool member trades its primary's instruments (the connector takes them for its order path
  // and opens no market data of its own).
  const VenueId owner = cfg.pool_plan().primary(vid);
  std::vector<InstrumentId> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == owner) mine.push_back(inst.id);
  }
  s.venue->subscribe(mine);
}

// The flag is published after the messages (release; the network thread acquires it). The network
// thread polls it on every loop iteration, so the eventfd is written only while an adaptive one is
// blocked in the reactor.
void wake_venue(void* ctx, VenueId v) noexcept {
  auto* w = static_cast<Wake*>(ctx);
  if (v.value >= w->slots->size()) return;
  VenueSlot& s = *(*w->slots)[v.value];
  s.wake.store(true, std::memory_order_release);
  if (!w->busy && s.net_blocked.take()) s.reactor->wake();
}

bool net_cpu_shared(int engine_cpu, std::span<const int> net_cpus, std::size_t index) {
  if (index >= net_cpus.size() || net_cpus[index] < 0) return false;
  const int cpu = net_cpus[index];
  if (cpu == engine_cpu) return true;
  for (std::size_t i = 0; i < net_cpus.size(); ++i) {
    if (i != index && net_cpus[i] == cpu) return true;
  }
  return false;
}

// Events pushed to the engine notify its feed, which wakes the engine only while it is blocked
// (Engine::block_idle).
void net_loop(VenueSlot& s, int cpu, std::size_t index, SpinMode spin, bool shared_cpu) {
  const std::string name = "fm-net-" + std::to_string(index);
  set_thread_name(name.c_str());
  pin_to_cpu(cpu);
  Logger::instance().attach_current_thread();
  s.venue->connect(*s.reactor);
  const bool busy = spin == SpinMode::Busy;
  const std::int64_t spin_ns = shared_cpu ? 0 : kNetSpinNs;
  if (shared_cpu && !busy)
    FASTMM_LOG_INFO(
        "{}: CPU {} is shared with another pinned thread; blocks when idle, no spin", name, cpu);
  std::uint64_t pushed = 0;
  std::int64_t idle_since = 0;  // 0 while active
  bool block = false;
  while (!s.stop.load(std::memory_order_relaxed)) {
    int wait_ms = 0;
    // A posted task may switch s.blocked during run_once(): clear the flag that was set.
    SleepFlag* const blocked = s.blocked;
    if (block) {
      // The engine takes the flag after it published (wake_venue, GatewayClient::wake_venue):
      // either this recheck sees the work or the engine writes the eventfd.
      blocked->set();
      if (!s.wake.load(std::memory_order_acquire) &&
          (s.pending == nullptr || !s.pending(s.hook_ctx)) &&
          !s.stop.load(std::memory_order_acquire))
        wait_ms = kNetMaxBlockMs;
    }
    bool active = s.reactor->run_once(wait_ms) > 0;
    if (block) blocked->clear();
    s.venue->poll();
    if (s.wake.load(std::memory_order_relaxed) &&
        s.wake.exchange(false, std::memory_order_acquire)) {
      s.venue->on_wake();
      active = true;
    }
    if (s.hook != nullptr && s.hook(s.hook_ctx) != 0) active = true;
    Waker* const consumer = s.consumer;
    if (busy && consumer == nullptr) continue;
    if (const std::uint64_t p = s.md_sink.pushed() + s.order_sink.pushed(); p != pushed) {
      pushed = p;
      if (consumer != nullptr) consumer->notify();
      active = true;
    }
    if (busy) continue;
    block = false;
    if (active) {
      idle_since = 0;
    } else if (idle_since == 0) {
      idle_since = net::Reactor::now_ns();
    } else if (net::Reactor::now_ns() - idle_since >= spin_ns) {
      block = true;
    } else {
      _mm_pause();
    }
  }
  s.venue->on_wake();  // flush cancels the engine queued during shutdown
  for (int i = 0; i < 20; ++i) s.reactor->run_once(5);
  s.venue->disconnect();
  s.reactor->run_once(0);
}

void log_venue_status(const venues::Venue& v) {
  const venues::VenueStatus st = v.status();
  FASTMM_LOG_INFO(
      "[{}] md={} user={} order={} books={}/{} md_msgs={} resyncs={} malformed={} dropped={} "
      "orders={} cancels={} order_events={} shadows={} swept={} refused={} rest={}/{}err "
      "reconnects={} clock_offset_ms={}",
      v.name(),
      short_state(st.md),
      short_state(st.user),
      short_state(st.order),
      st.books_synced,
      st.books_total,
      st.md_messages,
      st.resyncs,
      st.md_malformed,
      st.md_dropped,
      st.orders_sent,
      st.cancels_sent,
      st.order_events,
      st.shadows,
      st.shadows_swept,
      st.shadows_refused,
      st.rest_requests,
      st.rest_errors,
      st.reconnects,
      st.clock_offset_ms);
  log_wire_latency(v.name(), st, false);
  log_feed(v.name(), st.feed, false);
}

void log_wire_latency(std::string_view venue, const venues::VenueStatus& st, bool final) {
  const std::string_view tag = final ? std::string_view("final ") : std::string_view();
  // The session's percentiles, then the last minute's and the last hour's: a long session's
  // totals hide a recent tail.
  const venues::WireLatencyWindow& m = st.wire_1m;
  const venues::WireLatencyWindow& h = st.wire_1h;
  if (st.wire_tick_to_trade.count != 0) {
    FASTMM_LOG_INFO(
        "[{}] {}order latency: wire_t2t p50={}ns p99={}ns n={} encode p50={}ns send p50={}ns n={}; "
        "last 1m: wire_t2t p50={}ns p99={}ns n={} send p99={}ns; "
        "last 1h: wire_t2t p50={}ns p99={}ns n={} send p99={}ns",
        venue,
        tag,
        st.wire_tick_to_trade.p50_ns,
        st.wire_tick_to_trade.p99_ns,
        st.wire_tick_to_trade.count,
        st.order_encode.p50_ns,
        st.order_send.p50_ns,
        st.order_send.count,
        m.tick_to_trade.p50_ns,
        m.tick_to_trade.p99_ns,
        m.tick_to_trade.count,
        m.send.p99_ns,
        h.tick_to_trade.p50_ns,
        h.tick_to_trade.p99_ns,
        h.tick_to_trade.count,
        h.send.p99_ns);
  } else if (st.order_send.count != 0) {
    FASTMM_LOG_INFO(
        "[{}] {}order latency: encode p50={}ns send p50={}ns n={}; last 1m: send p50={}ns "
        "p99={}ns n={}; last 1h: send p50={}ns p99={}ns n={}",
        venue,
        tag,
        st.order_encode.p50_ns,
        st.order_send.p50_ns,
        st.order_send.count,
        m.send.p50_ns,
        m.send.p99_ns,
        m.send.count,
        h.send.p50_ns,
        h.send.p99_ns,
        h.send.count);
  }
}

void log_feed(std::string_view venue, const venues::VenueFeedStatus& f, bool final) {
  if (f.state == venues::FeedState::None) return;
  FASTMM_LOG_INFO(
      "[{}] {}feed={} packets={} a={} b={} gaps={} recovered={} lost={} snapshots={} "
      "overflows={} book_errors={} kernel_to_t0 p50={}ns p99={}ns",
      venue,
      final ? std::string_view("final ") : std::string_view(),
      to_string(f.state),
      f.packets,
      f.line_packets[0],
      f.line_packets[1],
      f.gaps,
      f.recovered,
      f.unrecovered,
      f.snapshot_recoveries,
      f.recovery_overflows,
      f.book_errors,
      f.kernel_to_t0.p50_ns,
      f.kernel_to_t0.p99_ns);
}

namespace {

StatusLatency wire_status_latency(const venues::WireLatencyStats& w) noexcept {
  return StatusLatency{w.count, w.p50_ns, w.p99_ns, w.p999_ns, w.max_ns};
}

void copy_feed_status(const venues::VenueFeedStatus& f, StatusFeed& out) noexcept {
  out.state = static_cast<std::uint8_t>(f.state);
  out.backend = f.backend;
  out.xdp_mode = f.xdp_mode;
  out.packets = f.packets;
  out.bytes = f.bytes;
  for (std::size_t l = 0; l < 2; ++l) {
    out.line_packets[l] = f.line_packets[l];
    out.line_duplicates[l] = f.line_duplicates[l];
    out.line_skew_mean_ns[l] = f.line_skew_mean_ns[l];
    out.line_skew_max_ns[l] = f.line_skew_max_ns[l];
  }
  out.gaps = f.gaps;
  out.recovered = f.recovered;
  out.unrecovered = f.unrecovered;
  out.snapshot_recoveries = f.snapshot_recoveries;
  out.recovery_overflows = f.recovery_overflows;
  out.reorder_high_water = f.reorder_high_water;
  out.requests = f.requests;
  out.malformed = f.malformed;
  out.book_errors = f.book_errors;
  out.kernel_to_t0 = wire_status_latency(f.kernel_to_t0);
  out.xdp_rx_dropped = f.xdp_rx_dropped;
  out.xdp_rx_invalid_descs = f.xdp_rx_invalid_descs;
  out.xdp_rx_ring_full = f.xdp_rx_ring_full;
  out.xdp_fill_ring_empty = f.xdp_fill_ring_empty;
  out.xdp_fallback = f.xdp_fallback;
}

}  // namespace

void fill_status_venue(const venues::VenueStatus& st, StatusVenue& sv) noexcept {
  sv.md = static_cast<std::uint8_t>(st.md);
  sv.user = static_cast<std::uint8_t>(st.user);
  sv.order = static_cast<std::uint8_t>(st.order);
  sv.books_synced = st.books_synced;
  sv.books_total = st.books_total;
  sv.md_messages = st.md_messages;
  sv.resyncs = st.resyncs;
  sv.orders_sent = st.orders_sent;
  sv.cancels_sent = st.cancels_sent;
  sv.replaces_sent = st.replaces_sent;
  sv.order_events = st.order_events;
  sv.reconnects = st.reconnects;
  sv.rest_errors = st.rest_errors;
  sv.rate_limit_cooldowns = st.rate_limit_cooldowns;
  sv.clock_offset_ms = st.clock_offset_ms;
  sv.wire_tick_to_trade = wire_status_latency(st.wire_tick_to_trade);
  sv.wire_tick_to_trade_1m = wire_status_latency(st.wire_1m.tick_to_trade);
  sv.wire_tick_to_trade_1h = wire_status_latency(st.wire_1h.tick_to_trade);
  copy_feed_status(st.feed, sv.feed);
}

}  // namespace fastmm::live
