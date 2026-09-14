#include "fastmm/live/session.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/venue_factory.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastmm::live {

namespace {

volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int sig) {
  g_signal = sig;
}

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = &on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

std::size_t ring_size(std::size_t bytes) {
  return std::bit_ceil(std::max<std::size_t>(bytes, 1U << 16));
}

// Per-venue plumbing. Heap allocated so addresses stay stable for the sinks/hooks.
struct VenueSlot {
  std::unique_ptr<venues::Venue> venue;
  std::unique_ptr<net::Reactor> reactor;
  std::unique_ptr<MsgRing> md_ring;
  std::unique_ptr<MsgRing> order_ring;
  std::unique_ptr<MsgRing> outbound;
  venues::EventSink md_sink;
  venues::EventSink order_sink;
  std::atomic<bool> wake{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> order_overflows{0};
  std::thread thread;
};

struct Wake {
  std::vector<std::unique_ptr<VenueSlot>>* slots;
};

void wake_venue(void* ctx, VenueId v) noexcept {
  auto* w = static_cast<Wake*>(ctx);
  if (v.value >= w->slots->size()) return;
  VenueSlot& s = *(*w->slots)[v.value];
  s.wake.store(true, std::memory_order_relaxed);
  s.reactor->wake();
}

void on_order_overflow(void* ctx, const venues::EventSink&) noexcept {
  static_cast<VenueSlot*>(ctx)->order_overflows.fetch_add(1, std::memory_order_relaxed);
}

void net_loop(VenueSlot& s, int cpu, std::size_t index, SpinMode spin) {
  const std::string name = "fm-net-" + std::to_string(index);
  set_thread_name(name.c_str());
  pin_to_cpu(cpu);
  Logger::instance().attach_current_thread();
  s.venue->connect(*s.reactor);
  const int wait_ms = spin == SpinMode::Busy ? 0 : 1;
  while (!s.stop.load(std::memory_order_relaxed)) {
    s.reactor->run_once(wait_ms);
    if (s.wake.exchange(false, std::memory_order_relaxed)) s.venue->on_wake();
  }
  s.venue->on_wake();  // flush cancels the engine queued during shutdown
  for (int i = 0; i < 20; ++i) s.reactor->run_once(5);
  s.venue->disconnect();
  s.reactor->run_once(0);
}

bool push_control(MsgRing& ring, ControlCommand cmd) {
  ControlMsg m{};
  init_header(m, EventType::Control);
  m.command = cmd;
  m.hdr.recv_ts = wall_now();
  return ring.try_push(&m, m.hdr.len);
}

// Long-baseline recalibration on the calling (main) thread: logs how far the previous mapping had
// drifted and any host wall-clock step, then publishes; the engine's and the venues' clocks pick it
// up themselves.
void recalibrate_tsc(TscCalibrator& calibrator,
                     Seqlocked<TscCalibration>& pub,
                     TscCalibration& last) {
  const TscRecalibration r = calibrator.update();
  if (!r.ok) {
    FASTMM_LOG_WARN("TSC recalibration skipped (no TSC mapping or baseline too short)");
    return;
  }
  if (r.host_step_ns > 1'000'000 || r.host_step_ns < -1'000'000) {
    FASTMM_LOG_WARN(
        "host wall clock stepped by {} ns relative to CLOCK_MONOTONIC_RAW within {:.1f} s; the "
        "engine clock follows it",
        r.host_step_ns,
        r.elapsed_s);
  }
  const double drift_ppm =
      r.elapsed_s > 0 ? static_cast<double>(r.drift_ns - r.host_step_ns) / r.elapsed_s / 1e3 : 0.0;
  FASTMM_LOG_INFO(
      "tsc recalibrated: drift {} ns over {:.1f} s ({:.3f} ppm excluding host steps), rate change "
      "{:.3f} ppm, {:.6f} GHz",
      r.drift_ns,
      r.elapsed_s,
      drift_ppm,
      r.rate_change_ppm,
      r.calibration.ghz);
  pub.store(r.calibration);
  last = r.calibration;
}

void log_wire_latency(std::string_view venue, const venues::VenueStatus& st, bool final) {
  const std::string_view tag = final ? std::string_view("final ") : std::string_view();
  if (st.wire_tick_to_trade.count != 0) {
    FASTMM_LOG_INFO(
        "[{}] {}order latency: wire_t2t p50={}ns p99={}ns n={} encode p50={}ns send p50={}ns n={}",
        venue,
        tag,
        st.wire_tick_to_trade.p50_ns,
        st.wire_tick_to_trade.p99_ns,
        st.wire_tick_to_trade.count,
        st.order_encode.p50_ns,
        st.order_send.p50_ns,
        st.order_send.count);
  } else if (st.order_send.count != 0) {
    FASTMM_LOG_INFO("[{}] {}order latency: encode p50={}ns send p50={}ns n={}",
                    venue,
                    tag,
                    st.order_encode.p50_ns,
                    st.order_send.p50_ns,
                    st.order_send.count);
  }
}

// Log lines carry at most kLogMaxStrBytes of a string argument, so a long breakdown is split over
// several lines with the same prefix, cut between reasons.
void log_reject_breakdown(std::string_view kind, const RejectCounts& c) {
  std::string line;
  const auto emit = [&] {
    if (!line.empty())
      FASTMM_LOG_INFO("fastmm-live: {} by reason: {}", kind, std::string_view(line));
    line.clear();
  };
  for (const auto& [reason, count] : nonzero_rejects(c)) {
    std::string item = std::string(to_string(reason)) + " " + std::to_string(count);
    if (!line.empty() && line.size() + 2 + item.size() > kLogMaxStrBytes) emit();
    line += line.empty() ? item : ", " + item;
  }
  emit();
}

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

int run_live(const Config& cfg, const LiveOptions& opts) {
  const char* prog = opts.program.c_str();
  // ---- instruments, venues, reference data (main thread, blocking) ---------------------
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
  const StrategyEntry* strategy = StrategyRegistry::instance().find(cfg.strategy.name);
  if (strategy == nullptr || !strategy->supports(TransportKind::Live)) {
    std::fprintf(stderr, "%s: unknown strategy '%s' (available:", prog, cfg.strategy.name.c_str());
    for (const StrategyEntry& e : list_strategies()) {
      if (e.supports(TransportKind::Live))
        std::fprintf(stderr, " %.*s", static_cast<int>(e.name.size()), e.name.data());
    }
    std::fprintf(stderr, ")\n");
    return kExitConfig;
  }
  // Parameter names are checked before any venue is contacted; values are checked when the engine
  // is built.
  for (const auto& [key, value] : cfg.strategy.params) {
    if (strategy->schema->find(key) == nullptr) {
      std::fprintf(stderr,
                   "%s: %.*s: unknown parameter '%s' (see --list-strategies)\n",
                   prog,
                   static_cast<int>(strategy->name.size()),
                   strategy->name.data(),
                   key.c_str());
      return kExitConfig;
    }
  }

  std::vector<std::unique_ptr<VenueSlot>> slots;
  std::uint64_t replace_venues = 0;  // bit v: venue v trades with cancel-replace (journal header)
  venues::VenueFactoryOptions vopts;
  vopts.dry_run = opts.dry_run;
  vopts.record_raw_dir = opts.record_raw_dir;
  if (!vopts.record_raw_dir.empty()) std::filesystem::create_directories(vopts.record_raw_dir);
  for (std::size_t i = 0; i < cfg.venues.size(); ++i) {
    auto slot = std::make_unique<VenueSlot>();
    try {
      slot->venue = venues::make_venue(VenueId{static_cast<std::uint8_t>(i)}, cfg.venues[i], vopts);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitConfig;
    }
    if (auto r = slot->venue->load_reference_data(instruments); !r) {
      std::fprintf(stderr, "%s: %s\n", prog, r.error().c_str());
      return kExitVenue;
    }
    slots.push_back(std::move(slot));
  }
  venues::SymbolTable symbols;
  if (!symbols.build(instruments)) {
    std::fprintf(stderr, "%s: duplicate or empty instrument symbols\n", prog);
    return kExitConfig;
  }

  // ---- rings, transport, feed -----------------------------------------------------------
  TscClock clock;
  TscCalibrator tsc_calibrator;  // long-baseline recalibration (main thread only)
  clock.set_calibration(tsc_calibrator.start());
  // Calibrations measured by the main thread; the engine refreshes `clock` from it on its own
  // thread and the venues convert their latency histograms with it.
  Seqlocked<TscCalibration> tsc_pub(clock.calibration());
  // Slew each measured offset away over one recalibration period instead of accumulating it.
  clock.attach_calibration_source(&tsc_pub,
                                  TscClock::kDefaultStepThreshold,
                                  cfg.engine.tsc_recalibrate_s > 0
                                      ? seconds(cfg.engine.tsc_recalibrate_s)
                                      : TscClock::kDefaultSlewHorizon);
  TscCalibration last_tsc = clock.calibration();  // main thread's copy of the latest publish
  LiveTransport transport;
  RingFeed feed;
  MsgRing control_ring(1U << 16);
  static_cast<void>(feed.add_ring(&control_ring));
  Wake wake_ctx{&slots};
  net::ReactorBackend net_backend = net::ReactorBackend::Epoll;
  static_cast<void>(net::parse_reactor_backend(cfg.engine.net_backend, net_backend));  // validated
  if (net::Reactor::resolve_backend(net_backend) != net_backend) {
    FASTMM_LOG_WARN(
        "[engine] net_backend = \"io_uring\" but io_uring is not available (kernel too "
        "old, disabled or not permitted); falling back to epoll");
    net_backend = net::ReactorBackend::Epoll;
  }
  for (std::size_t i = 0; i < slots.size(); ++i) {
    VenueSlot& s = *slots[i];
    const VenueId vid{static_cast<std::uint8_t>(i)};
    s.reactor = std::make_unique<net::Reactor>(net_backend);
    s.md_ring = std::make_unique<MsgRing>(ring_size(cfg.engine.md_ring_bytes));
    s.order_ring = std::make_unique<MsgRing>(ring_size(cfg.engine.order_ring_bytes));
    s.outbound = std::make_unique<MsgRing>(ring_size(cfg.engine.order_ring_bytes));
    s.md_sink.attach(s.md_ring.get(), venues::SinkPolicy::Drop);
    s.order_sink.attach(s.order_ring.get(), venues::SinkPolicy::Spin);
    s.order_sink.set_overflow_callback(&on_order_overflow, &s);
    // Orders first: order events must not wait behind a burst of market data.
    static_cast<void>(feed.add_ring(s.order_ring.get()));
    static_cast<void>(feed.add_ring(s.md_ring.get()));
    const bool replace = s.venue->caps().supports_replace && cfg.venues[i].supports_replace;
    transport.set_venue(vid, s.outbound.get(), replace);
    if (replace) replace_venues |= std::uint64_t{1} << i;
    s.venue->attach(symbols, instruments, s.md_sink, s.order_sink, s.outbound.get());
    s.venue->set_tsc_calibration_source(&tsc_pub);
    std::vector<InstrumentId> mine;
    for (const Instrument& inst : instruments) {
      if (inst.venue == vid) mine.push_back(inst.id);
    }
    s.venue->subscribe(mine);
  }
  transport.set_wake_hook(&wake_venue, &wake_ctx);

  // ---- engine + journal -------------------------------------------------------------------
  RunnerDeps deps;
  deps.engine.session_id = static_cast<std::uint64_t>(wall_now().ns);
  deps.engine.rng_seed = cfg.engine.rng_seed;
  // Best effort: SessionEpochStore reports its own error if the file cannot be written.
  if (const std::filesystem::path epoch(cfg.engine.epoch_file); epoch.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(epoch.parent_path(), ec);
    if (ec) {
      const std::string dir = epoch.parent_path().string();
      const std::string why = ec.message();
      FASTMM_LOG_WARN("cannot create session epoch directory {}: {}",
                      std::string_view(dir),
                      std::string_view(why));
    }
  }
  deps.engine.session_epoch = SessionEpochStore::next_epoch(cfg.engine.epoch_file);
  deps.engine.max_events_per_step = cfg.engine.max_events_per_step;
  deps.engine.crossed_grace = milliseconds(cfg.engine.crossed_grace_ms);
  deps.engine.latency_publish_interval = milliseconds(cfg.engine.latency_publish_ms);
  deps.engine.cpu = cfg.engine.cpu;
  deps.engine.spin_mode = cfg.spin_mode();
  deps.engine.risk = cfg.risk_limits();
  deps.engine.quotes = cfg.quote_params();
  deps.engine.quoting_enabled = !opts.dry_run;
  deps.instruments = &instruments;
  deps.params = cfg.strategy.params;

  std::unique_ptr<MsgRing> journal_ring;
  std::unique_ptr<JournalFileWriter> journal;
  const bool journaling = !opts.no_journal && (!opts.journal_path.empty() || cfg.engine.journal);
  if (journaling) {
    std::string path = opts.journal_path;
    if (path.empty()) {
      path = cfg.engine.journal_dir + "/" + cfg.engine.name + "-" +
             std::to_string(deps.engine.session_id) + ".fmj";
    }
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    journal_ring = std::make_unique<MsgRing>(ring_size(cfg.engine.journal_ring_bytes));
    // Everything a replay needs besides the events: the effective configuration (secrets
    // omitted) and the session settings that do not come from it.
    const std::string effective = cfg.effective_toml();
    JournalSessionInfo info;
    info.session_id = deps.engine.session_id;
    info.start_ts = wall_now();
    info.tsc = clock.calibration();
    info.config_hash = Config::text_hash(effective);
    info.rng_seed = deps.engine.rng_seed;
    info.strategy = strategy->name;
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = deps.engine.session_epoch;
    info.quoting_enabled = deps.engine.quoting_enabled;
    info.replace_venues = replace_venues;
    info.config_toml = effective;
    journal = std::make_unique<JournalFileWriter>(*journal_ring, path, info);
    if (!journal->ok()) {
      std::fprintf(stderr, "%s: cannot open journal %s\n", prog, path.c_str());
      return kExitRuntime;
    }
    deps.journal_ring = journal_ring.get();
    FASTMM_LOG_INFO("journal: {}", path);
  }

  LiveBackend backend{&clock, &transport, &feed};
  deps.backend = &backend;
  std::unique_ptr<IEngineRunner> runner;
  try {
    runner = StrategyRegistry::instance().make(strategy->name, TransportKind::Live, deps);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  if (runner == nullptr) {
    std::fprintf(
        stderr, "%s: cannot build a live runner for '%s'\n", prog, cfg.strategy.name.c_str());
    return kExitConfig;
  }

  // ---- threads --------------------------------------------------------------------------
  install_signal_handlers();
  if (journal) journal->start();
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const int cpu = i < cfg.engine.net_cpus.size() ? cfg.engine.net_cpus[i] : -1;
    slots[i]->thread = std::thread(net_loop, std::ref(*slots[i]), cpu, i, cfg.spin_mode());
  }
  std::thread engine_thread([&] { runner->run(); });

  FASTMM_LOG_INFO(
      "fastmm-live: session {} strategy={} venues={} instruments={} dry_run={} epoch={} net={}",
      deps.engine.session_id,
      strategy->name,
      slots.size(),
      instruments.size(),
      opts.dry_run,
      deps.engine.session_epoch,
      net::to_string(net_backend));

  // ---- live status for fastmm-top --------------------------------------------------------
  StatusWriter status;
  StatusSnapshot snap;
  if (!opts.no_status) {
    const std::string path =
        opts.status_path.empty() ? default_status_path(cfg.engine.name) : opts.status_path;
    std::string err;
    if (status.open(path, &err)) {
      FASTMM_LOG_INFO("status: {} (watch with fastmm-top --path {})", path, path);
    } else {
      FASTMM_LOG_WARN("status file {} unavailable: {}", path, err);
    }
  }
  snap.pid = static_cast<std::uint32_t>(::getpid());
  snap.session_id = deps.engine.session_id;
  snap.started_ns = wall_now().ns;
  snap.dry_run = opts.dry_run ? 1 : 0;
  set_status_name(snap.engine_name, cfg.engine.name);
  set_status_name(snap.strategy, strategy->name);
  snap.venue_count =
      static_cast<std::uint8_t>(std::min<std::size_t>(slots.size(), kStatusMaxVenues));
  const auto publish_status = [&](StatusRunState state) {
    if (!status.is_open()) return;
    snap.state = state;
    snap.updated_ns = wall_now().ns;
    const EngineLiveStats live = runner->live_stats();
    snap.events = live.stats.events;
    snap.book_updates = live.stats.book_updates;
    snap.orders_sent = live.stats.orders_sent;
    snap.cancels_sent = live.stats.cancels_sent;
    snap.replaces_sent = live.stats.replaces_sent;
    snap.fills = live.stats.fills;
    snap.risk_rejects = live.stats.risk_rejects;
    snap.venue_rejects = live.stats.venue_rejects;
    set_status_rejects(snap.risk_reject_reasons, live.stats.risk_rejects_by_reason);
    set_status_rejects(snap.venue_reject_reasons, live.stats.venue_rejects_by_reason);
    snap.kills = live.kills;
    snap.kill_flags = live.kill_flags;
    snap.realized_pnl_raw = live.stats.realized_pnl_raw;
    snap.unrealized_pnl_raw = live.stats.unrealized_pnl_raw;
    snap.fees_raw = live.stats.fees_raw;
    for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i)
      snap.latency[i] = to_status_latency(live.latency.interval[i]);
    for (std::size_t i = 0; i < snap.venue_count; ++i) {
      const venues::Venue* v = slots[i]->venue.get();
      const venues::VenueStatus st = v->status();
      StatusVenue& sv = snap.venues[i];
      set_status_name(sv.name, v->name());
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
      sv.wire_tick_to_trade = StatusLatency{st.wire_tick_to_trade.count,
                                            st.wire_tick_to_trade.p50_ns,
                                            st.wire_tick_to_trade.p99_ns,
                                            0};
    }
    status.publish(snap);
  };
  publish_status(StatusRunState::Running);

  // ---- control loop -----------------------------------------------------------------------
  const std::int64_t start = steady_now().ns;
  std::int64_t next_tick = start + 1'000'000'000;
  const std::int64_t recalibrate_ns =
      static_cast<std::int64_t>(cfg.engine.tsc_recalibrate_s) * 1'000'000'000;
  std::int64_t next_recalibration = start + recalibrate_ns;
  if (recalibrate_ns > 0 && !last_tsc.use_tsc)
    FASTMM_LOG_INFO("no invariant TSC: the clock uses clock_gettime; TSC recalibration is off");
  int reason = 0;  // 1 duration, 2 signal
  std::int64_t next_status = start + 250'000'000;
  while (reason == 0) {
    sleep_for(milliseconds(50));
    if (g_signal != 0) reason = 2;
    const std::int64_t now = steady_now().ns;
    if (opts.duration_ns > 0 && now - start >= opts.duration_ns) reason = reason == 0 ? 1 : reason;
    for (auto& s : slots) {
      if (s->order_overflows.load(std::memory_order_relaxed) != 0 && reason == 0) {
        FASTMM_LOG_ERROR("order ring overflow on {}: tripping the kill switch", s->venue->name());
        reason = 3;
      }
    }
    if (now >= next_status) {
      next_status = now + 250'000'000;
      publish_status(StatusRunState::Running);
    }
    if (recalibrate_ns > 0 && last_tsc.use_tsc && now >= next_recalibration) {
      recalibrate_tsc(tsc_calibrator, tsc_pub, last_tsc);
      next_recalibration = steady_now().ns + recalibrate_ns;
    }
    if (now >= next_tick) {
      next_tick += 1'000'000'000;
      for (std::size_t i = 0; i < slots.size(); ++i) {
        venues::Venue* v = slots[i]->venue.get();
        slots[i]->reactor->post([v] { v->on_timer(net::Reactor::now_ns()); });
        const venues::VenueStatus st = v->status();
        FASTMM_LOG_INFO(
            "[{}] md={} user={} order={} books={}/{} md_msgs={} resyncs={} malformed={} dropped={} "
            "orders={} cancels={} order_events={} rest={}/{}err reconnects={} clock_offset_ms={}",
            v->name(),
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
            st.rest_requests,
            st.rest_errors,
            st.reconnects,
            st.clock_offset_ms);
        log_wire_latency(v->name(), st, false);
      }
    }
  }
  const std::int64_t shutdown_start = steady_now().ns;
  publish_status(StatusRunState::Stopping);
  FASTMM_LOG_WARN("fastmm-live: shutting down ({})",
                  reason == 1   ? std::string_view("duration elapsed")
                  : reason == 2 ? std::string_view("signal")
                                : std::string_view("order ring overflow"));

  // Kill switch: the engine pulls quotes and queues cancels; independently every venue
  // cancels all open orders over its own REST connection (6.7).
  if (!push_control(control_ring, ControlCommand::TripKill))
    FASTMM_LOG_ERROR("control ring full: kill switch message dropped");
  bool cancel_ok = true;
  if (!opts.dry_run) {
    for (auto& s : slots) cancel_ok = s->venue->cancel_all() && cancel_ok;
  }
  // Let queued cancels reach the wire, then stop the engine and the net threads.
  sleep_for(milliseconds(200));
  runner->stop();
  engine_thread.join();
  for (auto& s : slots) {
    s->stop.store(true);
    s->reactor->wake();
  }
  for (auto& s : slots) s->thread.join();
  if (journal) journal->stop();

  const RunnerStats rs = runner->stats();
  // Numeric arguments: string arguments are capped at kLogMaxStrBytes, which used to cut the PnL.
  FASTMM_LOG_INFO(
      "fastmm-live: events={} book_updates={} orders={} cancels={} replaces={} fills={} "
      "risk_rejects={} venue_rejects={}",
      rs.events,
      rs.book_updates,
      rs.orders_sent,
      rs.cancels_sent,
      rs.replaces_sent,
      rs.fills,
      rs.risk_rejects,
      rs.venue_rejects);
  log_reject_breakdown("risk_rejects", rs.risk_rejects_by_reason);
  log_reject_breakdown("venue_rejects", rs.venue_rejects_by_reason);
  FASTMM_LOG_INFO(
      "fastmm-live: realized_pnl={} unrealized_pnl={} fees={} tick_to_trade p50={} ns p99={} ns",
      Notional::from_raw(rs.realized_pnl_raw),
      Notional::from_raw(rs.unrealized_pnl_raw),
      Notional::from_raw(rs.fees_raw),
      rs.tick_to_trade_p50_ns,
      rs.tick_to_trade_p99_ns);
  for (auto& s : slots) {
    const venues::VenueStatus st = s->venue->status();
    FASTMM_LOG_INFO(
        "[{}] final: books={}/{} md_msgs={} resyncs={} orders={} cancels={} order_events={} "
        "reconnects={}",
        s->venue->name(),
        st.books_synced,
        st.books_total,
        st.md_messages,
        st.resyncs,
        st.orders_sent,
        st.cancels_sent,
        st.order_events,
        st.reconnects);
    log_wire_latency(s->venue->name(), st, true);
  }
  // The engine thread has stopped, so its clock can be read here.
  FASTMM_LOG_INFO("fastmm-live: tsc clock re-anchors={} steps={} last_offset_ns={}",
                  clock.reanchors(),
                  clock.steps(),
                  clock.last_offset_ns());
  const std::int64_t shutdown_ms = (steady_now().ns - shutdown_start) / 1'000'000;
  FASTMM_LOG_INFO(
      "fastmm-live: shutdown took {} ms (cancel_all {})", shutdown_ms, cancel_ok ? "ok" : "FAILED");
  publish_status(StatusRunState::Stopped);  // the file stays: monitors show the final numbers
  if (!cancel_ok) return kExitRuntime;
  return reason == 3 ? kExitRuntime : kExitOk;
}

}  // namespace fastmm::live
