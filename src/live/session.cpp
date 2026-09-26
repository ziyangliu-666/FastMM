#include "fastmm/live/session.hpp"

#include "fastmm/config/env_subst.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/gateway.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/live/shutdown_guard.hpp"
#include "fastmm/live/thread_affinity.hpp"
#include "fastmm/live/venue_slot.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/store_thread.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/version.hpp"

#include <fmt/format.h>

#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastmm::live {

namespace {

volatile std::sig_atomic_t g_signal = 0;
volatile std::sig_atomic_t g_hup = 0;  // SIGHUP: clear the kill switch and resume quoting
std::atomic<std::int64_t> g_first_signal_ns{0};
std::atomic<ShutdownWatchdog*> g_watchdog{nullptr};
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
extern "C" void on_signal(int sig) {
  if (sig == SIGHUP) {
    g_hup = 1;
    return;
  }
  const std::int64_t now = monotonic_ns();
  const std::int64_t first = g_first_signal_ns.load(std::memory_order_relaxed);
  if (g_signal != 0 && second_signal_forces_exit(first, now))
    force_exit("second stop signal while shutting down: exiting without waiting for cancel_all");
  if (first == 0) g_first_signal_ns.store(now, std::memory_order_relaxed);
  if (ShutdownWatchdog* w = g_watchdog.load(std::memory_order_acquire)) w->stop_requested(now);
  g_signal = sig;
}

struct sigaction g_old_int {};
struct sigaction g_old_term {};
struct sigaction g_old_hup {};
std::atomic<bool> g_handlers_installed{false};

void install_signal_handlers() {
  // Each session starts without a pending stop: a process can run several sessions (tests).
  g_signal = 0;
  g_hup = 0;
  g_first_signal_ns.store(0);
  struct sigaction sa {};
  sa.sa_handler = &on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, &g_old_int);
  sigaction(SIGTERM, &sa, &g_old_term);
  sigaction(SIGHUP, &sa, &g_old_hup);
  g_handlers_installed.store(true);
}

// Restores the previous handlers when the session returns (a Python process gets its
// KeyboardInterrupt back).
struct SignalGuard {
  SignalGuard() { install_signal_handlers(); }
  SignalGuard(const SignalGuard&) = delete;
  SignalGuard& operator=(const SignalGuard&) = delete;
  SignalGuard(SignalGuard&&) = delete;
  SignalGuard& operator=(SignalGuard&&) = delete;
  ~SignalGuard() { restore_signal_handlers(); }
};

std::string cpu_list(const std::vector<int>& cpus) {
  std::string s;
  for (const int c : cpus) {
    if (!s.empty()) s += ',';
    s += std::to_string(c);
  }
  return s;
}

// Run-to-completion ([engine] threading = "single"): the engine thread runs the venue's network
// loop between engine steps (inline_poll). Market data reaches the engine as soon as the venue
// commits it to the md ring (inline_drain, the md sink's drain hook), and the engine's orders go
// straight to Venue::send_now (send_direct): no event crosses a thread between the packet read and
// the order write. The rings stay as same-thread FIFOs: order events, control messages and events
// the venue emits while the engine is running (a refused order) wait for the next step.
struct Inline {
  VenueSlot* slot = nullptr;
  IEngineRunner* runner = nullptr;
  bool active = false;  // between the engine's start and finish
  std::size_t drained = 0;
  std::atomic<bool> unsupported{false};
};

void inline_drain(void* ctx) noexcept {
  auto* in = static_cast<Inline*>(ctx);
  if (in->active) in->drained += in->runner->drain();
}

std::size_t inline_poll(void* ctx) noexcept {
  auto* in = static_cast<Inline*>(ctx);
  in->active = true;  // the engine has started
  in->drained = 0;
  VenueSlot& s = *in->slot;
  const int io = s.reactor->run_once(0);
  s.venue->poll();
  return static_cast<std::size_t>(io > 0 ? io : 0) + in->drained;
}

// Venue::send_now writes every message or refuses it through the order sink.
std::size_t send_direct(void* ctx, std::span<const EventHeader* const> batch) noexcept {
  static_cast<venues::Venue*>(ctx)->send_now(batch);
  return batch.size();
}

void inline_loop(Inline& in, int cpu) {
  VenueSlot& s = *in.slot;
  set_thread_name("fm-engine");
  pin_to_cpu(cpu);
  Logger::instance().attach_current_thread();
  s.venue->connect(*s.reactor);
  if (!in.runner->run_inline(&inline_poll, &in)) {
    FASTMM_LOG_ERROR("the strategy's runner cannot run inline ([engine] threading = \"single\")");
    in.unsupported.store(true);
  }
  // The engine has finished: nothing reaches it any more. Let the cancels it sent go out.
  in.active = false;
  s.md_sink.set_drain_hook(nullptr, nullptr, false);
  s.order_sink.set_drain_hook(nullptr, nullptr, false);
  for (int i = 0; i < 20; ++i) s.reactor->run_once(5);
  s.venue->disconnect();
  s.reactor->run_once(0);
}

bool push_control(MsgRing& ring, ControlCommand cmd, VenueId venue = VenueId::invalid()) {
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId{}, venue);
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

std::string host_name() {
  char buf[HOST_NAME_MAX + 1] = {};
  if (::gethostname(buf, sizeof buf - 1) != 0) return {};
  return buf;
}

// What the previous session of this engine left behind. Logged before this one starts, so an
// operator sees the open orders, the last position and the cumulative PnL without a query.
//
// It is what FastMM recorded, not what the venue holds: FastMM does not fetch execution history
// from a venue at start-up, so a fill that happened while the process was down is missing until
// the venue's reconciliation snapshot arrives (docs/reference/storage.md).
std::optional<store::Recovery> log_previous_session(const std::string& backend_name,
                                                    const Config& cfg) {
  auto reader = store::StoreRegistry::instance().make_reader(backend_name);
  if (reader == nullptr) return std::nullopt;
  store::BackendOptions opts;
  opts.config = &cfg.storage;
  opts.engine_name = cfg.engine.name;
  opts.default_dir = cfg.engine.journal_dir;
  opts.read_only = true;
  if (auto r = reader->open(opts); !r) return std::nullopt;  // no store yet: the first session
  store::QueryFilter f;
  f.engine = cfg.engine.name;
  auto rec = reader->recovery(f);
  if (!rec || !rec->found) return std::nullopt;
  const store::Recovery& p = *rec;
  FASTMM_LOG_WARN(
      "previous session {} ({}) started {} and {}: realized {} unrealized {} fees {} net {} over "
      "{} fill(s)",
      p.session_id,
      p.strategy,
      p.started_utc,
      p.stopped_utc.empty() ? std::string("never recorded a shutdown") : "stopped " + p.stopped_utc,
      p.realized,
      p.unrealized,
      p.fees,
      p.net,
      p.fills);
  // A clean shutdown asks for the kill switch itself, so only an unrequested one is news.
  if (p.kill_latched || (p.kill_reason != "None" && p.kill_reason != "Requested"))
    FASTMM_LOG_WARN("previous session kill switch: {}{}",
                    std::string_view(p.kill_reason),
                    p.kill_latched ? " (latched)" : "");
  if (!p.journal_complete)
    FASTMM_LOG_ERROR(
        "the previous session's journal was not closed: its tail is missing and a replay of it "
        "will refuse to run");
  if (p.records_dropped != 0)
    FASTMM_LOG_WARN("the previous session dropped {} store record(s): its rows are incomplete",
                    p.records_dropped);
  for (const std::string& line : p.positions)
    FASTMM_LOG_WARN("previous position: {}", std::string_view(line));
  for (const std::string& line : p.open_orders)
    FASTMM_LOG_ERROR(
        "the previous session still had order {} open at its last record; the venue may still "
        "hold it",
        std::string_view(line));
  FASTMM_LOG_INFO("previous session: fastmm-pnl recover --engine {}", cfg.engine.name);
  return *rec;
}

// Where one venue's first execution replay of a restored session starts: the store's resume point
// for it, found by the venue's name (by its id in a store that predates the names), else the
// engine-clock fallback. `instruments` is null before a gateway attach, which takes no trade ids.
struct ResumePlan {
  std::int64_t since_ms = 0;  // venue time, inclusive; 0: no replay
  std::vector<std::string> known;
  std::vector<std::pair<InstrumentId, std::int64_t>> next_ids;  // Venue::resume_trade_ids
};

ResumePlan resume_plan(const store::Recovery& prev,
                       std::string_view venue,
                       VenueId vid,
                       const InstrumentTable* instruments) {
  const auto same = [&](const std::string& name, std::uint8_t id) {
    return name.empty() ? id == vid.value : name == venue;
  };
  ResumePlan p;
  const store::Recovery::VenueResume* r = nullptr;
  for (const store::Recovery::VenueResume& v : prev.venue_resume) {
    if (same(v.venue, v.venue_id)) r = &v;
  }
  if (r != nullptr) {
    p.since_ms = r->since_ms;
    p.known = r->known_exec_ids;
    if (r->shrunk)
      FASTMM_LOG_WARN(
          "{}: more than {} stored executions in the {} ms before the previous session's last "
          "fill; its execution replay starts {} ms after that fill rather than {} ms before",
          venue,
          store::Recovery::kMaxKnownExecIds,
          store::Recovery::kResumeOverlapMs,
          r->since_ms - r->last_fill_ms,
          store::Recovery::kResumeOverlapMs);
  } else {
    p.since_ms = prev.fallback_since_ms;
    p.known = prev.fallback_exec_ids;
    if (p.since_ms > 0)
      FASTMM_LOG_INFO(
          "{}: no stored fill of this venue carries the venue's time; its execution replay starts "
          "from the engine clock",
          venue);
  }
  if (instruments != nullptr) {
    for (const store::Recovery::TradeIdMark& m : prev.last_trade_ids) {
      if (!same(m.venue, m.venue_id)) continue;
      for (const Instrument& in : *instruments) {
        if (in.venue == vid && in.symbol.view() == m.symbol)
          p.next_ids.emplace_back(in.id, m.last_id + 1);
      }
    }
  }
  return p;
}

// Carries the previous session's positions over, on venues that can replay what happened while
// nothing was running. The position enters the engine as a reconciliation on the venue's order
// ring, so the journal records it and a replay starts from the same place; then the venue's
// execution replay starts where the store's record ends and books every execution the store does
// not have - fills of orders that were still resting when the process died, and trades made on the
// account outside FastMM - with their real prices and fees. A venue that cannot replay executions
// starts flat, as before: a stored position with nothing to bring it up to date could be wrong.
//
// With fastmm-gateway the messages go on the control ring instead (this process does not produce
// into the gateway's order ring) and the execution replay's start travels in the attach request.
struct RestoreVenue {
  std::string_view name;
  bool can_replay = false;  // VenueCapabilities::executions
};

template <class Push, class Resume>
void restore_positions(const store::Recovery& prev,
                       const InstrumentTable& instruments,
                       std::span<const RestoreVenue> venues,
                       Push&& push,
                       Resume&& resume) {
  if (prev.position_state.empty() && prev.last_fill_ns == 0) return;
  for (std::size_t i = 0; i < venues.size(); ++i) {
    const bool can_replay = venues[i].can_replay;
    const VenueId vid{static_cast<std::uint8_t>(i)};
    for (const store::Recovery::PositionState& p : prev.position_state) {
      if (p.qty_raw == 0) continue;
      const Instrument* inst = nullptr;
      for (const Instrument& in : instruments) {
        if (in.venue == vid && in.symbol.view() == p.symbol) inst = &in;
      }
      if (inst == nullptr) continue;
      if (!can_replay) {
        FASTMM_LOG_WARN(
            "{}: not restoring the previous position {} {} - this venue cannot replay what "
            "happened "
            "while nothing was running, so the session starts flat",
            venues[i].name,
            p.symbol,
            Qty::from_raw(p.qty_raw));
        continue;
      }
      ReconcileMsg m{};
      init_header(m, EventType::Reconcile, inst->id, vid);
      m.kind = ReconcileMsg::Kind::Position;
      m.position_qty = Qty::from_raw(p.qty_raw);
      m.avg_px = Price::from_raw(p.avg_px_raw);
      push(i, m);
      FASTMM_LOG_INFO("{}: restored position {} {} @ {} from the previous session",
                      venues[i].name,
                      p.symbol,
                      m.position_qty,
                      m.avg_px);
    }
    if (!can_replay) continue;
    if (ResumePlan plan = resume_plan(prev, venues[i].name, vid, &instruments); plan.since_ms > 0)
      resume(i, plan);
  }
}

}  // namespace

void restore_signal_handlers() noexcept {
  if (!g_handlers_installed.exchange(false)) return;
  sigaction(SIGINT, &g_old_int, nullptr);
  sigaction(SIGTERM, &g_old_term, nullptr);
  sigaction(SIGHUP, &g_old_hup, nullptr);
}

bool resolve_venue_env(Config& cfg, bool dry_run, const char* prog) {
  venues::register_builtin_venues();
  for (VenueSection& v : cfg.venues) {
    auto resolve = [&](const char* key, std::string& value, bool secret) {
      if (!has_env_reference(value)) return true;
      auto r = substitute_env(value);
      if (r) {
        value = *r;
        return true;
      }
      if (secret && dry_run) {
        value.clear();
        return true;
      }
      if (secret) {
        std::fprintf(
            stderr,
            "%s: venue '%s' needs API keys: environment variable %s is not set "
            "(venues.%s.%s). Export it, or run with --dry-run for public market data only.\n",
            prog,
            v.name.c_str(),
            r.error().c_str(),
            v.name.c_str(),
            key);
      } else {
        std::fprintf(stderr,
                     "%s: venues.%s.%s: environment variable %s is not set\n",
                     prog,
                     v.name.c_str(),
                     key,
                     r.error().c_str());
      }
      return false;
    };
    if (!resolve("ws_url", v.ws_url, false) || !resolve("ws_api_url", v.ws_api_url, false) ||
        !resolve("rest_url", v.rest_url, false) || !resolve("ca_file", v.ca_file, false) ||
        !resolve("api_key", v.api_key, true) || !resolve("api_secret", v.api_secret, true))
      return false;
    for (auto& [k, val] : v.extra) {
      if (!resolve(k.c_str(), val, false)) return false;
    }
    auto extra = [&](const char* key) {
      const auto it = v.extra.find(key);
      return it == v.extra.end() ? std::string_view{} : std::string_view(it->second);
    };
    // Ed25519 keys have no secret (the private key comes from private_key_file / _env), and
    // Binance SBE market data needs the API key even in a dry run.
    const bool ed25519 = extra("key_type") == "ed25519";
    const bool sbe_md = extra("md_format") == "sbe";
    // The venue declares whether it needs credentials at all (venues/registry.hpp); nasdaq_itch,
    // for one, takes market data without keys and logs OUCH in with ouch_username / ouch_password.
    const venues::VenueEntry* entry = venues::VenueRegistry::instance().find(v.kind);
    const bool needs_keys = entry == nullptr || entry->caps.credentials;
    if (dry_run) {
      if (!sbe_md) v.api_key.clear();
      v.api_secret.clear();
    } else if (needs_keys && (v.api_key.empty() || (v.api_secret.empty() && !ed25519))) {
      std::fprintf(stderr,
                   "%s: venue '%s' has no api_key/api_secret. Set them via ${ENV} references, or "
                   "run with --dry-run for public market data only.\n",
                   prog,
                   v.name.c_str());
      return false;
    }
  }
  return true;
}

int run_live(const Config& cfg, const LiveOptions& opts) {
  const char* prog = opts.program.c_str();
  const LiveStrategy* const custom = opts.strategy;
  // ---- instruments, venues, reference data (main thread, blocking) ---------------------
  // Attached to fastmm-gateway: the venues, their reference data and the instrument table are the
  // gateway's.
  const bool via_gateway = !opts.gateway_path.empty();
  InstrumentTable instruments;
  if (!via_gateway) {
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
  }
  const StrategyEntry* const strategy =
      custom != nullptr ? nullptr : StrategyRegistry::instance().find(cfg.strategy.name);
  if (custom != nullptr && !custom->make) {
    std::fprintf(
        stderr, "%s: the strategy '%s' has no runner factory\n", prog, custom->name.c_str());
    return kExitConfig;
  }
  if (custom == nullptr && (strategy == nullptr || !strategy->supports(TransportKind::Live))) {
    std::fprintf(stderr, "%s: unknown strategy '%s' (available:", prog, cfg.strategy.name.c_str());
    for (const StrategyEntry& e : list_strategies()) {
      if (e.supports(TransportKind::Live))
        std::fprintf(stderr, " %.*s", static_cast<int>(e.name.size()), e.name.data());
    }
    std::fprintf(stderr, ")\n");
    return kExitConfig;
  }
  const std::string_view strategy_name =
      custom != nullptr ? std::string_view(custom->name) : strategy->name;
  const ParamSchema* const param_schema = custom != nullptr ? custom->params : strategy->schema;
  // Parameter names are checked before any venue is contacted; values are checked when the engine
  // is built. A LiveStrategy checks its own.
  for (const auto& [key, value] : cfg.strategy.params) {
    if (custom == nullptr && strategy->schema->find(key) == nullptr) {
      std::fprintf(stderr,
                   "%s: %.*s: unknown parameter '%s' (see --list-strategies)\n",
                   prog,
                   static_cast<int>(strategy->name.size()),
                   strategy->name.data(),
                   key.c_str());
      return kExitConfig;
    }
  }

  VenueSlots slots;
  std::unique_ptr<GatewayClient> gateway;
  std::vector<InstrumentId> not_mine;  // attached: the instruments other strategies trade
  std::vector<std::string> venue_names;
  std::uint64_t replace_venues = 0;  // bit v: venue v trades with cancel-replace (journal header)
  // What the previous session left behind, read before the venues attach so its positions can be
  // carried over (restore_positions). Read-only: the store itself is opened further down.
  std::optional<store::Recovery> previous;
  const auto read_previous = [&] {
    if (const std::string b = store::configured_backend(cfg.storage); b != store::kNoBackend) {
      store::register_builtin_backends();
      previous = log_previous_session(b, cfg);
    }
  };
  if (via_gateway) {
    if (cfg.single_threaded()) {
      std::fprintf(stderr,
                   "%s: [engine] threading = \"single\" runs the venue in this process; it "
                   "cannot attach to a gateway\n",
                   prog);
      return kExitConfig;
    }
    read_previous();
    GatewayAttachRequest req;
    req.engine = cfg.engine.name;
    req.blocks = cfg.spin_mode() == SpinMode::Adaptive;
    // What this strategy trades: its own [[instruments]]. The gateway's table holds every
    // instrument of the account; the others stay disabled here.
    for (const InstrumentSection& in : cfg.instruments) {
      if (in.enabled) req.instruments.emplace_back(in.venue, in.symbol);
    }
    if (req.instruments.empty()) {
      std::fprintf(stderr,
                   "%s: no enabled [[instruments]]: a strategy attached to a gateway names the "
                   "instruments it trades there\n",
                   prog);
      return kExitConfig;
    }
    if (previous && cfg.engine.restore_position) {
      // Each venue's execution replay starts where the store's record of it ends, in its own
      // clock. The trade-id start (Venue::resume_trade_ids) stays in-process: the gateway's venue
      // is shared, and it filters another attachment's replay for this one by time and ids.
      if (previous->last_fill_ns > 0 || !previous->position_state.empty()) {
        for (const auto& [venue, symbol] : req.instruments) {
          bool listed = false;
          for (const GatewayAttachRequest::Resume& r : req.resume)
            listed = listed || r.venue == venue;
          if (listed) continue;
          ResumePlan plan = resume_plan(*previous, venue, VenueId{}, nullptr);
          if (plan.since_ms > 0)
            req.resume.push_back({venue, plan.since_ms, std::move(plan.known)});
        }
      }
      // The gateway's account starts from what this strategy restores (restore_positions below).
      for (const auto& [venue, symbol] : req.instruments) {
        for (const store::Recovery::PositionState& p : previous->position_state) {
          if (p.symbol == symbol && p.qty_raw != 0)
            req.positions.push_back(
                {venue, symbol, Qty::from_raw(p.qty_raw), Price::from_raw(p.avg_px_raw)});
        }
      }
    }
    std::string err;
    gateway = GatewayClient::attach(opts.gateway_path, req, &err);
    if (gateway == nullptr) {
      std::fprintf(stderr, "%s: gateway %s: %s\n", prog, opts.gateway_path.c_str(), err.c_str());
      return kExitVenue;
    }
    instruments = gateway->instruments();
    for (const GatewayVenue& v : gateway->venues()) venue_names.push_back(v.name);
    // Another strategy's instruments: their market data arrives here, their quotes are pulled
    // below and risk refuses any order on them (InstrumentDisabled), as the gateway would.
    std::size_t mine = 0;
    for (std::uint32_t k = 0; k < instruments.size(); ++k) {
      Instrument& inst = instruments.get(InstrumentId{k});
      const std::string_view venue = venue_names[inst.venue.value];
      bool claimed = false;
      for (const auto& [v, sym] : req.instruments)
        claimed = claimed || (v == venue && sym == inst.symbol.view());
      if (claimed) {
        ++mine;
      } else {
        inst.flags = static_cast<std::uint8_t>(inst.flags & ~Instrument::kEnabled);
        not_mine.push_back(inst.id);
      }
    }
    FASTMM_LOG_INFO(
        "gateway: attached to {} (attachment {}, session epoch {}): {} venue(s), {} of {} "
        "instrument(s) traded here, with the gateway's reference data",
        opts.gateway_path,
        gateway->attach_id(),
        gateway->session_epoch(),
        venue_names.size(),
        mine,
        instruments.size());
    if (instruments.size() == 0) {
      std::fprintf(stderr, "%s: the gateway has no instruments\n", prog);
      return kExitConfig;
    }
  } else {
    venues::VenueFactoryOptions vopts;
    vopts.dry_run = opts.dry_run;
    vopts.record_raw_dir = opts.record_raw_dir;
    vopts.busy_poll = cfg.spin_mode() == SpinMode::Busy;
    if (!vopts.record_raw_dir.empty()) std::filesystem::create_directories(vopts.record_raw_dir);
    if (const int rc = make_venue_slots(cfg, vopts, instruments, prog, slots); rc != 0) return rc;
    for (const auto& s : slots) venue_names.emplace_back(s->venue->name());
  }
  // ---- fee rates ---------------------------------------------------------------------------
  // [[instruments]] maker_bps / taker_bps, else [venues.<x>.fees]; a connector with fetch_fees
  // replaces them with the account's own. Those are written into the configuration the journal and
  // the store embed (`recorded`), so a replay reports and charges the rates this session used.
  FeeTable fees = fee_table(cfg, &instruments, &venue_names);
  std::optional<Config> fetched_fees;
  for (const auto& s : slots) {
    auto r = s->venue->account_fees(instruments);
    if (!r) {
      std::fprintf(stderr, "%s: %s\n", prog, r.error().c_str());
      return kExitVenue;
    }
    for (const venues::VenueFee& f : *r) {
      const Instrument& inst = instruments.get(f.instrument);
      const std::string_view venue = venue_names[inst.venue.value];
      const FeeRates configured = fees.schedule(f.instrument);
      if (configured == f.rates) {
        FASTMM_LOG_INFO("{}: {} account fees maker {} bps, taker {} bps, as configured",
                        venue,
                        inst.symbol.view(),
                        f.rates.maker_bps(),
                        f.rates.taker_bps());
      } else {
        FASTMM_LOG_WARN(
            "{}: {} account fees maker {} bps, taker {} bps; configured {} / {} bps: using the "
            "account's",
            venue,
            inst.symbol.view(),
            f.rates.maker_bps(),
            f.rates.taker_bps(),
            configured.maker_bps(),
            configured.taker_bps());
      }
      fees.set_instrument(f.instrument, f.rates);
      if (!fetched_fees) fetched_fees = cfg;
      for (InstrumentSection& is : fetched_fees->instruments) {
        if (is.venue == venue && is.symbol == inst.symbol.view()) {
          is.maker_bps = f.rates.maker_bps();
          is.taker_bps = f.rates.taker_bps();
        }
      }
    }
  }
  const Config& recorded = fetched_fees ? *fetched_fees : cfg;

  // [accounting] converts the PnL totals, [risk] max_loss and the exposure caps to one reporting
  // currency. Without it every total is one currency-less Notional. The venues' reference data has
  // been loaded, so kInverse (and with it each instrument's settlement currency) is known here.
  FxPlan fx_plan;
  if (cfg.accounting.configured()) {
    std::string warning;
    auto plan = session_fx_plan(instruments,
                                cfg.accounting,
                                venue_names,
                                /*all_instruments=*/false,
                                cfg.risk_limits().reads_totals(),
                                &warning);
    if (!plan) {
      std::fprintf(stderr,
                   "%s: [accounting]: %s. [risk] max_loss, max_gross_notional and "
                   "max_net_notional are in %s.\n",
                   prog,
                   plan.error().c_str(),
                   cfg.accounting.reporting_currency.c_str());
      return kExitConfig;
    }
    if (!warning.empty()) FASTMM_LOG_WARN("[accounting]: {}", warning);
    fx_plan = *plan;
  }
  if (fx_plan.active()) {
    for (std::size_t c = 1; c < fx_plan.count; ++c) {
      const FxSource& src = fx_plan.sources[c];
      if (!src.instrument.valid()) {
        FASTMM_LOG_WARN("accounting: {} has no source; its PnL is not in the {} totals",
                        fx_plan.names[c],
                        fx_plan.reporting());
        continue;
      }
      const Instrument& si = instruments.get(src.instrument);
      FASTMM_LOG_INFO("accounting: {} converts to {} at the mid of {}:{}{}",
                      fx_plan.names[c],
                      fx_plan.reporting(),
                      venue_names[si.venue.value],
                      si.symbol,
                      src.invert ? " (inverted)" : "");
    }
  } else if (const SettlementMix mix = instruments.settlement_mix(); mix.mixed()) {
    const std::string a(mix.first->settlement_ccy());
    const std::string b(mix.other->settlement_ccy());
    const std::string sa(mix.first->symbol.view());
    const std::string sb(mix.other->symbol.view());
    if (cfg.risk_limits().max_loss.is_positive()) {
      std::fprintf(stderr,
                   "%s: instruments settle in different currencies (%s in %s, %s in %s) and "
                   "[risk] max_loss is one number in one currency. Set [accounting] "
                   "reporting_currency and an [accounting.fx] source per other currency, split "
                   "them into one session per settlement currency, or unset max_loss.\n",
                   prog,
                   sa.c_str(),
                   a.empty() ? "?" : a.c_str(),
                   sb.c_str(),
                   b.empty() ? "?" : b.c_str());
      return kExitConfig;
    }
    FASTMM_LOG_WARN(
        "instruments settle in different currencies ({} in {}, {} in {}): the PnL totals in the "
        "logs and the status file add unrelated numbers ([accounting] converts them)",
        mix.first->symbol,
        a.empty() ? std::string_view("?") : std::string_view(a),
        mix.other->symbol,
        b.empty() ? std::string_view("?") : std::string_view(b));
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
  // Attached and adaptive: the engine sleeps on a futex in the gateway's wake page, which the
  // gateway's network threads and this process's producers all notify.
  if (gateway && cfg.spin_mode() == SpinMode::Adaptive) feed.waker().share(gateway->engine_flag());
  MsgRing control_ring(1U << 16);
  static_cast<void>(feed.add_ring(&control_ring));
  if (custom != nullptr) {
    for (MsgRing* const ring : custom->inputs) {
      if (!feed.add_ring(ring)) {
        std::fprintf(stderr, "%s: too many engine input rings\n", prog);
        return kExitConfig;
      }
    }
  }
  // Destroyed before `feed`, on every return: the caller's producers stop waking it first.
  struct WakerGuard {
    const live::LiveStrategy* s;
    ~WakerGuard() {
      if (s != nullptr && s->set_waker) s->set_waker({});
    }
  } waker_guard{custom};
  if (custom != nullptr && custom->set_waker) custom->set_waker([&feed] { feed.notify(); });
  Wake wake_ctx{&slots, cfg.spin_mode() == SpinMode::Busy};
  const net::ReactorBackend net_backend =
      via_gateway ? net::ReactorBackend::Epoll : resolve_net_backend(cfg);
  if (!via_gateway) read_previous();
  for (std::size_t i = 0; i < slots.size(); ++i) {
    VenueSlot& s = *slots[i];
    const VenueId vid{static_cast<std::uint8_t>(i)};
    wire_venue_slot(s, vid, cfg, net_backend, symbols, instruments, &tsc_pub);
    // Orders first: order events must not wait behind a burst of market data.
    static_cast<void>(feed.add_ring(s.order_ring.get()));
    static_cast<void>(feed.add_ring(s.md_ring.get()));
    const bool replace = s.venue->caps().supports_replace && cfg.venues[i].supports_replace;
    transport.set_venue(vid, s.outbound.get(), replace);
    if (replace) replace_venues |= std::uint64_t{1} << i;
  }
  if (via_gateway) {
    // An adaptive gateway's network threads block in their reactors: wake them after a push.
    if (gateway->gateway_blocks())
      transport.set_wake_hook(&GatewayClient::wake_venue, gateway.get());
    for (GatewayVenue& v : gateway->venues()) {
      static_cast<void>(feed.add_ring(v.order.get()));
      static_cast<void>(feed.add_ring(v.md.get()));
      transport.set_venue(v.id, v.outbound.get(), v.replace);
      if (v.replace) replace_venues |= std::uint64_t{1} << v.id.value;
    }
    if (previous && cfg.engine.restore_position) {
      std::vector<RestoreVenue> rv;
      for (const GatewayVenue& v : gateway->venues()) rv.push_back({v.name, v.executions});
      restore_positions(
          *previous,
          instruments,
          rv,
          [&](std::size_t, const ReconcileMsg& m) {
            if (!control_ring.try_push(&m, m.hdr.len))
              FASTMM_LOG_ERROR("control ring full: a restored position was dropped");
          },
          [](std::size_t, const ResumePlan&) {});
    }
    // The strategy is not asked to quote what another attachment trades (a scoped pull, as
    // `fastmm-ctl pull --instrument` sends it; journaled like any control message).
    for (const InstrumentId id : not_mine) {
      ControlMsg m{};
      init_header(m, EventType::Control, id, instruments.get(id).venue);
      m.command = ControlCommand::PullQuotes;
      if (!control_ring.try_push(&m, m.hdr.len))
        FASTMM_LOG_ERROR("control ring full: an instrument of another strategy stays quotable");
    }
  } else {
    if (previous && cfg.engine.restore_position) {
      std::vector<RestoreVenue> rv;
      for (std::size_t i = 0; i < slots.size(); ++i) {
        const venues::VenueEntry* entry =
            venues::VenueRegistry::instance().find(cfg.venues[i].kind);
        rv.push_back({slots[i]->venue->name(), entry != nullptr && entry->caps.executions});
      }
      restore_positions(
          *previous,
          instruments,
          rv,
          [&](std::size_t i, const ReconcileMsg& m) {
            static_cast<void>(slots[i]->order_sink.push(m.hdr));
          },
          [&](std::size_t i, const ResumePlan& plan) {
            slots[i]->venue->resume_executions(plan.since_ms, plan.known);
            if (!plan.next_ids.empty()) slots[i]->venue->resume_trade_ids(plan.next_ids);
          });
    }
    transport.set_wake_hook(&wake_venue, &wake_ctx);
  }

  // ---- engine + journal -------------------------------------------------------------------
  RunnerDeps deps;
  deps.engine.session_id = static_cast<std::uint64_t>(wall_now().ns);
  deps.engine.rng_seed = cfg.engine.rng_seed;
  // Attached: the gateway gave this session its epoch, unique among the strategies trading on its
  // venues. Otherwise the epoch file's next. Best effort: SessionEpochStore reports its own error
  // if the file cannot be written.
  if (const std::filesystem::path epoch(cfg.engine.epoch_file);
      !gateway && epoch.has_parent_path()) {
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
  bool epoch_wrapped = false;
  const auto epoch = gateway ? Result<std::uint16_t, std::string>(gateway->session_epoch())
                             : SessionEpochStore::next_epoch(cfg.engine.epoch_file, &epoch_wrapped);
  if (!epoch) {
    // Fail closed: without a fresh epoch this session would reuse another one's client order ids.
    std::fprintf(stderr, "%s: %s\n", prog, epoch.error().c_str());
    return kExitConfig;
  }
  if (epoch_wrapped)
    FASTMM_LOG_WARN(
        "session epoch has cycled past 65535: client order ids of sessions that long ago can "
        "repeat");
  deps.engine.session_epoch = *epoch;
  deps.engine.max_events_per_step = cfg.engine.max_events_per_step;
  deps.engine.crossed_grace = milliseconds(cfg.engine.crossed_grace_ms);
  deps.engine.ack_timeout = milliseconds(cfg.engine.ack_timeout_ms);
  deps.engine.flatten_interval = milliseconds(cfg.engine.flatten_interval_ms);
  deps.engine.flatten_timeout = milliseconds(cfg.engine.flatten_timeout_ms);
  deps.engine.flatten_slippage_bps = cfg.engine.flatten_slippage_bps;
  deps.engine.queue_conservatism_bps = std::llround(cfg.engine.queue_conservatism * 10'000.0);
  deps.engine.max_param_age = milliseconds(cfg.strategy.max_param_age_ms);
  deps.engine.latency_publish_interval = milliseconds(cfg.engine.latency_publish_ms);
  deps.engine.cpu = cfg.engine.cpu;
  deps.engine.spin_mode = cfg.spin_mode();
  deps.engine.risk = cfg.risk_limits();
  deps.engine.quotes = cfg.quote_params();
  deps.engine.fx = fx_plan;
  deps.engine.fees = fees;
  deps.engine.quoting_enabled = !opts.dry_run;
  deps.instruments = &instruments;
  deps.params = cfg.strategy.params;

  // Latched kill switch and the loss budget already spent, so a restart does not re-arm
  // [risk] max_loss (docs/how-to/operations/kill-switch-and-shutdown.md).
  const std::string kill_path =
      cfg.engine.kill_file.empty()
          ? KillStateStore::default_path(cfg.engine.journal_dir, cfg.engine.name)
          : cfg.engine.kill_file;
  if (const std::filesystem::path kp(kill_path); kp.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(kp.parent_path(), ec);
  }
  if (opts.clear_kill) {
    if (auto r = KillStateStore::clear(kill_path); !r) {
      std::fprintf(stderr, "%s: %s\n", prog, r.error().c_str());
      return kExitConfig;
    }
    FASTMM_LOG_WARN("--clear-kill: {} removed; the whole [risk] max_loss budget is armed again",
                    kill_path);
  }
  KillState kill_state;
  if (auto loaded = KillStateStore::load(kill_path)) {
    kill_state = *loaded;
  } else {
    std::fprintf(stderr, "%s: %s\n", prog, loaded.error().c_str());
    return kExitConfig;
  }
  if (kill_state.latched) {
    std::fprintf(stderr,
                 "%s: a %s kill switch is latched in %s (net PnL %.8f over %llu session(s)). "
                 "Check the positions, then clear it with --clear-kill or by removing the file; "
                 "that arms the whole [risk] max_loss budget again.\n",
                 prog,
                 std::string(to_string(kill_state.reason)).c_str(),
                 kill_path.c_str(),
                 kill_state.carry().to_double(),
                 static_cast<unsigned long long>(kill_state.sessions));
    return kExitKilled;
  }
  ++kill_state.sessions;
  deps.engine.pnl_carry = kill_state.carry();

  // ---- storage backend ([storage] backend) ------------------------------------------------
  // The engine hands fills, orders, positions and kill events to a ring, exactly as it does the
  // journal; a StoreThread turns them into rows. "none" costs nothing: no ring, no thread, and
  // the engine's RecordWriter is disabled.
  const std::string backend_name = store::configured_backend(cfg.storage);
  std::unique_ptr<MsgRing> record_ring;
  std::unique_ptr<store::StoreThread> store_thread;
  if (backend_name != store::kNoBackend) {
    store::register_builtin_backends();
    auto& registry = store::StoreRegistry::instance();
    auto backend = registry.make_backend(backend_name);
    if (backend == nullptr) {
      std::fprintf(stderr,
                   "%s: [storage] backend = \"%s\" is not registered (have %s)\n",
                   prog,
                   backend_name.c_str(),
                   registry.names().c_str());
      return kExitConfig;
    }
    store::BackendOptions bopts;
    bopts.config = &cfg.storage;
    bopts.engine_name = cfg.engine.name;
    bopts.default_dir = cfg.engine.journal_dir;
    // Fail closed: an operator who does not want a store sets backend = "none" rather than
    // starting a session whose record silently goes nowhere.
    if (auto r = backend->open(bopts); !r) {
      std::fprintf(stderr, "%s: [storage] %s\n", prog, r.error().c_str());
      return kExitConfig;
    }
    const auto ring_bytes =
        static_cast<std::size_t>(cfg.storage.get_int("ring_bytes", std::int64_t{4} * 1024 * 1024));
    record_ring = std::make_unique<MsgRing>(ring_size(ring_bytes));
    store_thread = std::make_unique<store::StoreThread>(*record_ring, std::move(backend));
    deps.record_ring = record_ring.get();
    FASTMM_LOG_INFO("storage: {}", backend_name);
  }

  std::unique_ptr<MsgRing> journal_ring;
  std::unique_ptr<JournalFileWriter> journal;
  std::string journal_path;
  const bool journaling = !opts.no_journal && (!opts.journal_path.empty() || cfg.engine.journal);
  if (journaling && cfg.engine.journal_retention_days > 0) {
    std::string prune_error;
    const std::size_t removed =
        prune_journals(cfg.engine.journal_dir, cfg.engine.journal_retention_days, &prune_error);
    if (removed != 0)
      FASTMM_LOG_INFO("journal retention: {} file(s) older than {} day(s) removed from {}",
                      removed,
                      cfg.engine.journal_retention_days,
                      cfg.engine.journal_dir);
    if (!prune_error.empty())
      FASTMM_LOG_WARN("journal retention: {}", std::string_view(prune_error));
  }
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
    const std::string effective = recorded.effective_toml();
    JournalSessionInfo info;
    info.session_id = deps.engine.session_id;
    info.start_ts = wall_now();
    info.tsc = clock.calibration();
    info.config_hash = Config::text_hash(effective);
    info.rng_seed = deps.engine.rng_seed;
    info.strategy = strategy_name;
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = deps.engine.session_epoch;
    info.quoting_enabled = deps.engine.quoting_enabled;
    info.replace_venues = replace_venues;
    info.config_toml = effective;
    info.params = param_schema;
    if (custom != nullptr) info.strategy_meta = custom->meta;
    JournalOptions jopts;
    static_cast<void>(parse_journal_sync(cfg.engine.journal_sync, jopts.sync));  // validated
    jopts.max_bytes = cfg.engine.journal_max_bytes;
    journal = std::make_unique<JournalFileWriter>(*journal_ring, path, info, jopts);
    if (!journal->ok()) {
      std::fprintf(stderr, "%s: cannot open journal %s\n", prog, path.c_str());
      return kExitRuntime;
    }
    deps.journal_ring = journal_ring.get();
    journal_path = path;
    FASTMM_LOG_INFO("journal: {} (sync {}, rotate at {} bytes, 0 = never)",
                    path,
                    to_string(jopts.sync),
                    jopts.max_bytes);
  }

  LiveBackend backend{&clock, &transport, &feed};
  deps.backend = &backend;
  std::unique_ptr<IEngineRunner> runner;
  try {
    runner = custom != nullptr
                 ? custom->make(deps)
                 : StrategyRegistry::instance().make(strategy->name, TransportKind::Live, deps);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  if (runner == nullptr) {
    std::fprintf(stderr,
                 "%s: cannot build a live runner for '%s'\n",
                 prog,
                 std::string(strategy_name).c_str());
    return kExitConfig;
  }

  const bool single = cfg.single_threaded();
  Inline inline_ctx;
  if (single) {
    if (slots.size() != 1) {
      std::fprintf(stderr, "%s: [engine] threading = \"single\" needs exactly one venue\n", prog);
      return kExitConfig;
    }
    VenueSlot& s = *slots[0];
    inline_ctx.slot = &s;
    inline_ctx.runner = runner.get();
    s.md_sink.set_drain_hook(&inline_drain, &inline_ctx, /*on_commit=*/true);
    s.order_sink.set_drain_hook(&inline_drain, &inline_ctx, /*on_commit=*/false);
    transport.set_direct(&send_direct, s.venue.get());
    if (!cfg.engine.net_cpus.empty())
      FASTMM_LOG_WARN(
          "[engine] threading = \"single\": net_cpus is ignored (the engine thread, "
          "cpu, runs the network loop)");
  }

  // ---- threads --------------------------------------------------------------------------
  if (opts.confine_other_threads) {
    std::vector<int> reserved{cfg.engine.cpu};
    reserved.insert(reserved.end(), cfg.engine.net_cpus.begin(), cfg.engine.net_cpus.end());
    const ConfineResult confined = confine_threads(reserved);
    if (!confined.error.empty()) {
      FASTMM_LOG_WARN("{}", std::string_view(confined.error));
    } else if (!confined.cpus.empty()) {
      const std::string cpus = cpu_list(confined.cpus);
      FASTMM_LOG_INFO("thread affinity: {} thread(s) of this process moved to CPUs {}",
                      confined.threads,
                      std::string_view(cpus));
    }
  }
  // Threads started from here on (journal, network, engine) inherit the timer slack.
  if (cfg.engine.timer_slack_ns > 0) {
    if (set_timer_slack(nanoseconds(cfg.engine.timer_slack_ns))) {
      FASTMM_LOG_INFO("timer slack: {} ns", cfg.engine.timer_slack_ns);
    } else {
      FASTMM_LOG_WARN("timer slack: prctl(PR_SET_TIMERSLACK) refused");
    }
  }
  if (cfg.engine.lock_memory) {
    if (const int err = lock_all_memory(); err != 0) {
      FASTMM_LOG_WARN("lock_memory: mlockall failed: {} (raise ulimit -l / LimitMEMLOCK)",
                      std::strerror(err));
    } else {
      FASTMM_LOG_INFO("lock_memory: all memory locked");
    }
  }
  const SignalGuard signals;
  // A stop that does not finish in time ends the process anyway (shutdown_guard.hpp).
  ShutdownWatchdog shutdown_watchdog;
  struct WatchdogRegistration {
    explicit WatchdogRegistration(ShutdownWatchdog* w) { g_watchdog.store(w); }
    ~WatchdogRegistration() { g_watchdog.store(nullptr); }
    WatchdogRegistration(const WatchdogRegistration&) = delete;
    WatchdogRegistration& operator=(const WatchdogRegistration&) = delete;
  } const watchdog_registration(&shutdown_watchdog);
  if (store_thread) {
    store::SessionOpen so;
    so.session_id = deps.engine.session_id;
    so.session_epoch = deps.engine.session_epoch;
    so.started_ns = wall_now().ns;
    so.engine_name = cfg.engine.name;
    so.strategy = std::string(strategy_name);
    so.version = FASTMM_VERSION_STRING;
    so.build_info = build_info();
    so.config_hash = recorded.effective_hash();
    so.config_toml = recorded.effective_toml();
    so.journal_path = journal_path;
    so.host = host_name();
    so.pid = static_cast<std::uint32_t>(::getpid());
    so.dry_run = opts.dry_run;
    so.pnl_carry_raw = deps.engine.pnl_carry.raw;
    so.venues = venue_names;
    store::Backend& store_backend = store_thread->backend();
    if (auto r = store_backend.session_open(so); !r) {
      std::fprintf(stderr, "%s: [storage] %s\n", prog, r.error().c_str());
      return kExitConfig;
    }
    if (auto r = store_backend.instruments(
            deps.engine.session_id,
            std::span<const Instrument>(instruments.data(), instruments.size()));
        !r) {
      std::fprintf(stderr, "%s: [storage] %s\n", prog, r.error().c_str());
      return kExitConfig;
    }
    store_thread->start();
  }
  if (journal) journal->start();
  std::thread engine_thread;
  if (single) {
    engine_thread = std::thread(inline_loop, std::ref(inline_ctx), cfg.engine.cpu);
  } else {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      const int cpu = i < cfg.engine.net_cpus.size() ? cfg.engine.net_cpus[i] : -1;
      if (cfg.spin_mode() == SpinMode::Adaptive) slots[i]->consumer = &feed.waker();
      slots[i]->thread = std::thread(net_loop, std::ref(*slots[i]), cpu, i, cfg.spin_mode());
    }
    engine_thread = std::thread([&] { runner->run(); });
  }

  FASTMM_LOG_INFO(
      "fastmm-live: session {} strategy={} venues={} instruments={} dry_run={} epoch={} net={} "
      "threading={}",
      deps.engine.session_id,
      strategy_name,
      venue_names.size(),
      instruments.size(),
      opts.dry_run,
      deps.engine.session_epoch,
      net::to_string(net_backend),
      std::string_view(cfg.engine.threading));

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
  set_status_name(snap.strategy, strategy_name);
  snap.venue_count =
      static_cast<std::uint8_t>(std::min<std::size_t>(venue_names.size(), kStatusMaxVenues));
  const auto publish_status = [&](StatusRunState state, const EngineLiveStats& live) {
    snap.state = state;
    snap.updated_ns = wall_now().ns;
    snap.events = live.stats.events;
    snap.max_loss_raw = live.max_loss_raw;
    snap.book_updates = live.stats.book_updates;
    snap.orders_sent = live.stats.orders_sent;
    snap.cancels_sent = live.stats.cancels_sent;
    snap.replaces_sent = live.stats.replaces_sent;
    snap.fills = live.stats.fills;
    snap.risk_rejects = live.stats.risk_rejects;
    snap.venue_rejects = live.stats.venue_rejects;
    set_status_rejects(snap.risk_reject_reasons, live.stats.risk_rejects_by_reason);
    set_status_rejects(snap.venue_reject_reasons, live.stats.venue_rejects_by_reason);
    snap.quoting_elapsed_ns = live.quoting_elapsed_ns;
    snap.quoting_two_sided_ns = live.quoting_two_sided_ns;
    snap.kills = live.kills;
    snap.venue_kills = static_cast<std::uint32_t>(live.venue_kills);
    snap.kill_flags = live.kill_flags;
    snap.kill_reason = static_cast<std::uint8_t>(live.kill_reason);
    snap.flatten_state = static_cast<std::uint8_t>(live.flatten_state);
    snap.flatten_instruments_left = live.flatten_instruments_left;
    snap.flatten_orders = live.flatten_orders;
    snap.realized_pnl_raw = live.stats.realized_pnl_raw;
    snap.unrealized_pnl_raw = live.stats.unrealized_pnl_raw;
    snap.fees_raw = live.stats.fees_raw;
    for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i)
      snap.latency[i] = to_status_latency(live.latency.interval[i]);
    for (std::size_t i = 0; i < snap.venue_count; ++i) {
      StatusVenue& sv = snap.venues[i];
      const VenueId vid{static_cast<std::uint8_t>(i)};
      set_status_name(sv.name, venue_names[i]);
      sv.killed = (live.kill_flags & RiskEngine::venue_bit(vid)) != 0 ? 1 : 0;
      sv.kill_reason =
          static_cast<std::uint8_t>(live.venue_kill_reasons[RiskEngine::venue_slot(vid)]);
      if (i >= slots.size()) continue;  // the venue runs in the gateway
      fill_status_venue(slots[i]->venue->status(), sv);
    }
    if (status.is_open()) status.publish(snap);
  };
  // Writes the cumulative PnL (and latches a max-loss trip) next to the journal, off the engine
  // thread. A write failure is logged once and does not stop the session.
  bool kill_write_failed = false;
  bool kill_written = false;
  KillState kill_last;
  const auto persist_kill = [&](const EngineLiveStats& live) {
    KillState st = kill_state;
    st.realized = kill_state.realized + Notional::from_raw(live.stats.realized_pnl_raw);
    st.fees = kill_state.fees + Notional::from_raw(live.stats.fees_raw);
    if (live.kill_reason == KillReason::MaxLoss) {
      st.latched = true;
      st.reason = KillReason::MaxLoss;
    }
    snap.kill_latched = st.latched ? 1 : 0;
    snap.pnl_carry_raw = kill_state.carry().raw;
    // Nothing traded since the last write: no rewrite, and no fsync of the journal directory.
    if (kill_written && st.realized == kill_last.realized && st.fees == kill_last.fees &&
        st.latched == kill_last.latched) {
      return;
    }
    st.updated_ns = wall_now().ns;
    kill_last = st;
    kill_written = true;
    if (auto r = KillStateStore::store(kill_path, st); !r && !kill_write_failed) {
      kill_write_failed = true;
      FASTMM_LOG_ERROR("cannot persist the kill state: {}", std::string_view(r.error()));
    }
  };
  persist_kill(runner->live_stats());
  publish_status(StatusRunState::Running, runner->live_stats());

  // ---- control loop -----------------------------------------------------------------------
  const std::int64_t start = steady_now().ns;
  std::int64_t next_tick = start + 1'000'000'000;
  const std::int64_t recalibrate_ns =
      static_cast<std::int64_t>(cfg.engine.tsc_recalibrate_s) * 1'000'000'000;
  std::int64_t next_recalibration = start + recalibrate_ns;
  if (recalibrate_ns > 0 && !last_tsc.use_tsc)
    FASTMM_LOG_INFO(
        "no invariant TSC: the clock uses clock_gettime; TSC recalibration is off; latency "
        "intervals {}",
        last_tsc.has_rate() ? "use the TSC (constant_tsc)" : "use clock_gettime");
  // 1 duration, 2 signal, 3 order ring overflow, 4 kill switch tripped by the engine, 5 watchdog,
  // 6 the runner cannot run inline (threading = "single"), 7 the journal cannot be written,
  // 8 the control socket's `stop`, 9 the gateway closed the attachment
  int reason = 0;
  std::string watchdog_cause;
  std::int64_t next_status = start + 250'000'000;
  const bool exit_on_kill = cfg.engine.on_kill != "stay";
  KillReason engine_kill = KillReason::None;  // the unrequested global kill, once seen
  std::int64_t next_kill_reminder = 0;
  std::uint32_t reported_venue_kills = 0;  // venue kill bits already logged

  // ---- control socket ---------------------------------------------------------------------
  // Everything an operator can do to a running session besides the signals, over an AF_UNIX
  // socket this thread owns (live/control_socket.hpp). The commands that change what the engine
  // does become messages on the control ring, so the journal records them and a replay
  // reproduces them.
  const auto clear_kill = [&] {
    if (!push_control(control_ring, ControlCommand::ResetKill)) return false;
    feed.notify();
    kill_state.latched = false;
    kill_state.reason = KillReason::None;
    engine_kill = KillReason::None;
    reported_venue_kills = 0;
    return true;
  };
  std::unique_ptr<ParamPublisher> publisher;
  // Pushes onto the control ring and wakes an engine blocked while idle (adaptive spin).
  struct ControlSinkCtx {
    MsgRing* ring;
    RingFeed* feed;
  } control_sink_ctx{&control_ring, &feed};
  const ParamSink control_sink{&control_sink_ctx, [](void* c, const ParamUpdateMsg& m) noexcept {
                                 auto* x = static_cast<ControlSinkCtx*>(c);
                                 if (!x->ring->try_push(&m.hdr, m.hdr.len)) return false;
                                 x->feed->notify();
                                 return true;
                               }};
  if (custom == nullptr && strategy != nullptr && strategy->publisher != nullptr) {
    // The publisher shares the control ring, so a `param` and the `pull` after it reach the
    // engine in the order the operator typed them. This thread is the ring's only producer.
    try {
      publisher = strategy->publisher(control_sink, cfg.strategy.params);
    } catch (const std::exception& e) {
      FASTMM_LOG_WARN("parameter updates are not available: {}", std::string_view(e.what()));
    }
  }
  ControlPlane plane;
  plane.instruments = &instruments;
  plane.limits = cfg.risk_limits();
  plane.submit = [&](const EventHeader& h) {
    if (!control_ring.try_push(&h, h.len)) return false;
    feed.notify();
    return true;
  };
  plane.venue = [&](std::string_view name, VenueId& out) {
    for (std::size_t i = 0; i < venue_names.size(); ++i) {
      if (venue_names[i] != name) continue;
      out = VenueId{static_cast<std::uint8_t>(i)};
      return true;
    }
    return false;
  };
  plane.request_stop = [&] {
    if (reason == 0) reason = 8;
  };
  plane.clear_kill = [&] {
    FASTMM_LOG_WARN("control socket: clearing the kill switch and resuming quoting");
    return clear_kill();
  };
  plane.status = [&] {
    std::string out = format_status(snap, wall_now().ns, /*color=*/false);
    const RiskLimits& l = plane.limits;
    const auto dec = [](auto v) {
      char buf[48];
      const std::size_t n = v.to_decimal(buf);
      return std::string(buf, n);
    };
    out += fmt::format(
        "limits     max_order_qty={} max_order_notional={} max_position={} max_open_orders={} "
        "price_collar_bps={} fat_finger_bps={} stale_md_ms={} max_loss={} orders_per_sec={} "
        "burst={} stp={}\n",
        dec(l.max_order_qty),
        dec(l.max_order_notional),
        dec(l.max_position),
        l.max_open_orders,
        l.price_collar_bps,
        l.fat_finger_bps,
        l.stale_md.millis(),
        dec(l.max_loss),
        l.orders_per_sec,
        l.burst,
        l.stp);
    return out;
  };
  if (publisher) {
    plane.params = [&](const ControlPlane::ParamValues& values, InstrumentId inst) {
      try {
        if (!publisher->publish(values, inst.valid() ? inst : ParamPublisher::kAllInstruments))
          return std::string("the control ring is full; try again");
      } catch (const std::invalid_argument& e) {
        return std::string(e.what());
      }
      return std::string();
    };
  } else if (custom != nullptr && custom->set_params) {
    plane.params = custom->set_params;
  }
  ControlSocket control_socket;
  if (!opts.no_control) {
    const std::string ctl_path = opts.control_path.empty()
                                     ? cfg.engine.journal_dir + "/" + cfg.engine.name + ".ctl"
                                     : opts.control_path;
    std::string ctl_error;
    if (control_socket.open(ctl_path, &ctl_error)) {
      FASTMM_LOG_INFO("control socket: {} (fastmm-ctl --path {} status)", ctl_path, ctl_path);
    } else {
      FASTMM_LOG_WARN("control socket {} unavailable: {}", ctl_path, ctl_error);
    }
  }

  while (reason == 0) {
    sleep_for(milliseconds(50));
    control_socket.poll(plane);
    if (g_signal != 0) reason = 2;
    const std::int64_t now = steady_now().ns;
    if (opts.duration_ns > 0 && now - start >= opts.duration_ns) reason = reason == 0 ? 1 : reason;
    for (auto& s : slots) {
      if (s->order_overflows.load(std::memory_order_relaxed) != 0 && reason == 0) {
        FASTMM_LOG_ERROR("order ring overflow on {}: tripping the kill switch", s->venue->name());
        reason = 3;
      }
    }
    if (reason == 0 && inline_ctx.unsupported.load()) reason = 6;
    // The gateway exited or dropped this attachment: nothing reaches the venues any more.
    if (reason == 0 && gateway && !gateway->connected()) {
      FASTMM_LOG_ERROR("the gateway closed the attachment: no market data, no orders");
      reason = 9;
    }
    // A journal that cannot be written (a full filesystem, an I/O error) means the session is no
    // longer recoverable: stop trading rather than keep going blind.
    if (reason == 0 && journal && journal->failed()) {
      FASTMM_LOG_ERROR("journal write failed ({}): tripping the kill switch and shutting down",
                       to_string(journal->error()));
      reason = 7;
    }
    if (reason == 0 && opts.watchdog) {
      watchdog_cause = opts.watchdog();
      if (!watchdog_cause.empty()) {
        FASTMM_LOG_ERROR("fastmm-live: {}", std::string_view(watchdog_cause));
        reason = 5;
      }
    }
    // The engine publishes its kill-switch state as soon as a flag changes.
    const EngineLiveStats live = runner->live_stats();
    if (const std::uint32_t new_venue_kills = live.kill_flags & ~1U & ~reported_venue_kills;
        new_venue_kills != 0) {
      reported_venue_kills |= new_venue_kills;
      std::size_t trading = 0;
      for (std::size_t i = 0; i < venue_names.size(); ++i) {
        if ((live.kill_flags & RiskEngine::venue_bit(VenueId{static_cast<std::uint8_t>(i)})) == 0)
          ++trading;
      }
      for (std::size_t i = 0; i < venue_names.size(); ++i) {
        const VenueId vid{static_cast<std::uint8_t>(i)};
        if ((new_venue_kills & RiskEngine::venue_bit(vid)) == 0) continue;
        FASTMM_LOG_ERROR(
            "[{}] venue kill switch engaged ({}): its quotes are pulled and new orders to it are "
            "refused; {} of {} venue(s) still trading",
            venue_names[i],
            live.venue_kill_reasons[RiskEngine::venue_slot(vid)],
            trading,
            venue_names.size());
      }
    }
    if ((live.kill_flags & 1U) != 0 && live.kill_reason != KillReason::Requested) {
      if (engine_kill == KillReason::None) {
        engine_kill = live.kill_reason;
        next_kill_reminder = now;
        if (exit_on_kill && reason == 0) reason = 4;
      }
      if (!exit_on_kill && now >= next_kill_reminder) {
        next_kill_reminder = now + kKilledReminderNs;
        FASTMM_LOG_ERROR(
            "fastmm-live: kill switch engaged ({}, flags={:#x}) and [engine] on_kill = \"stay\": "
            "quoting is off and no new orders are sent; stop the process (SIGINT/SIGTERM) to "
            "cancel all and exit",
            engine_kill,
            live.kill_flags);
      }
    }
    if (g_hup != 0) {
      g_hup = 0;
      FASTMM_LOG_WARN("SIGHUP: clearing the kill switch and resuming quoting");
      if (!clear_kill()) FASTMM_LOG_ERROR("control ring full: kill reset message dropped");
    }
    if (now >= next_status) {
      next_status = now + 250'000'000;
      persist_kill(live);
      publish_status(StatusRunState::Running, live);
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
        log_venue_status(*v);
      }
    }
  }
  shutdown_watchdog.stop_requested();  // --duration, a kill switch, fastmm-ctl stop, ...
  control_socket.close();              // no command can reach a session that is shutting down
  const std::int64_t shutdown_start = steady_now().ns;
  persist_kill(runner->live_stats());
  publish_status(StatusRunState::Stopping, runner->live_stats());
  if (reason == 4) {
    FASTMM_LOG_ERROR("fastmm-live: shutting down (kill switch: {}; [engine] on_kill = \"exit\")",
                     engine_kill);
  } else if (reason == 5) {
    FASTMM_LOG_ERROR("fastmm-live: shutting down (slow tier failed)");
  } else if (reason == 6) {
    FASTMM_LOG_ERROR("fastmm-live: shutting down (the engine could not run inline)");
  } else if (reason == 7) {
    FASTMM_LOG_ERROR("fastmm-live: shutting down (the journal cannot be written: {})",
                     to_string(journal->error()));
  } else if (reason == 9) {
    FASTMM_LOG_ERROR("fastmm-live: shutting down (the gateway went away)");
  } else {
    FASTMM_LOG_WARN("fastmm-live: shutting down ({})",
                    reason == 1   ? std::string_view("duration elapsed")
                    : reason == 2 ? std::string_view("signal")
                    : reason == 8 ? std::string_view("control socket: stop")
                                  : std::string_view("order ring overflow"));
  }

  // Kill switch: the engine pulls quotes and queues cancels (it already did when it tripped the
  // switch itself); independently every venue cancels all open orders over its own REST
  // connection (6.7).
  if (publisher) publisher->close();
  if (reason != 4 && !push_control(control_ring, ControlCommand::TripKill))
    FASTMM_LOG_ERROR("control ring full: kill switch message dropped");
  feed.notify();
  bool cancel_ok = true;
  if (gateway) {
    FASTMM_LOG_WARN(
        "fastmm-live: no venue cancel_all here: the gateway cancels every open order when this "
        "attachment closes");
  } else if (!opts.dry_run) {
    for (auto& s : slots) cancel_ok = s->venue->cancel_all() && cancel_ok;
  }
  // Let queued cancels reach the wire, then stop the engine and the net threads.
  sleep_for(milliseconds(200));
  runner->stop();
  engine_thread.join();
  if (custom != nullptr && custom->finished) custom->finished(*runner);
  if (gateway) {
    gateway->close();  // the detach: the gateway cancels what rests
    FASTMM_LOG_INFO("gateway: detached");
  }
  // Single: the engine thread ran the network loop and has flushed it.
  for (auto& s : slots) {
    s->stop.store(true);
    s->reactor->wake();
  }
  for (auto& s : slots) {
    if (s->thread.joinable()) s->thread.join();
  }
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
  {
    const EngineLiveStats final_live = runner->live_stats();
    if (final_live.quoting_elapsed_ns > 0) {
      FASTMM_LOG_INFO("fastmm-live: quoting two_sided={}% of {} s",
                      100.0 * static_cast<double>(final_live.quoting_two_sided_ns) /
                          static_cast<double>(final_live.quoting_elapsed_ns),
                      static_cast<double>(final_live.quoting_elapsed_ns) / 1e9);
    }
  }
  log_reject_breakdown("risk_rejects", rs.risk_rejects_by_reason);
  log_reject_breakdown("venue_rejects", rs.venue_rejects_by_reason);
  FASTMM_LOG_INFO(
      "fastmm-live: realized_pnl={} unrealized_pnl={} fees={} tick_to_trade p50={} ns p99={} ns",
      Notional::from_raw(rs.realized_pnl_raw),
      Notional::from_raw(rs.unrealized_pnl_raw),
      Notional::from_raw(rs.fees_raw),
      rs.tick_to_trade_p50_ns,
      rs.tick_to_trade_p99_ns);
  if (rs.funding_raw != 0)
    FASTMM_LOG_INFO("fastmm-live: funding={} (part of realized_pnl)",
                    Notional::from_raw(rs.funding_raw));
  if (fx_plan.active())
    FASTMM_LOG_INFO("fastmm-live: the PnL totals are in {} ([accounting])", fx_plan.reporting());
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
    log_feed(s->venue->name(), st.feed, true);
  }
  // The engine thread has stopped, so its clock can be read here.
  FASTMM_LOG_INFO("fastmm-live: tsc clock re-anchors={} steps={} last_offset_ns={}",
                  clock.reanchors(),
                  clock.steps(),
                  clock.last_offset_ns());
  const EngineLiveStats final_live = runner->live_stats();  // published by the engine's finish()
  const bool unrequested_kill =
      final_live.kill_reason != KillReason::None && final_live.kill_reason != KillReason::Requested;
  if (unrequested_kill || final_live.venue_kills != 0) {
    FASTMM_LOG_ERROR("fastmm-live: kill switch flags={:#x} reason={} kills={} venue_kills={}",
                     final_live.kill_flags,
                     final_live.kill_reason,
                     final_live.kills,
                     final_live.venue_kills);
  }
  const std::int64_t shutdown_ms = (steady_now().ns - shutdown_start) / 1'000'000;
  FASTMM_LOG_INFO(
      "fastmm-live: shutdown took {} ms (cancel_all {})", shutdown_ms, cancel_ok ? "ok" : "FAILED");
  persist_kill(final_live);
  // The file stays: monitors show the final numbers and the kill reason.
  publish_status(StatusRunState::Stopped, final_live);
  const int rc = !cancel_ok                                                 ? kExitRuntime
                 : reason == 4                                              ? kExitKilled
                 : reason == 5                                              ? kExitSlowTier
                 : reason == 3 || reason == 6 || reason == 7 || reason == 9 ? kExitRuntime
                                                                            : kExitOk;
  if (store_thread) {
    store_thread->stop();  // drains and commits everything the ring still holds
    store::SessionClose sc;
    sc.session_id = deps.engine.session_id;
    sc.stopped_ns = wall_now().ns;
    sc.exit_code = rc;
    sc.kill_reason = final_live.kill_reason;
    sc.kill_latched = kill_last.latched;
    sc.stats = final_live.stats;
    if (journal) {
      sc.journal_complete = !journal->failed();
      sc.journal_bytes = journal->bytes_written();
      sc.journal_paths = journal->part_paths();
    }
    store::Backend& store_backend = store_thread->backend();
    if (auto r = store_backend.session_close(sc); !r)
      FASTMM_LOG_ERROR("store: {}", std::string_view(r.error()));
    const store::StoreThreadStats ss = store_thread->stats();
    FASTMM_LOG_INFO("store: {} record(s) in {} batch(es), {} row(s), {} error(s), {} dropped",
                    ss.records,
                    ss.batches,
                    store_backend.rows(),
                    store_backend.errors(),
                    final_live.stats.records_dropped);
    if (store_backend.errors() != 0)
      FASTMM_LOG_ERROR("store: last error: {}", std::string_view(store_backend.last_error()));
    store_backend.close();
  }
  shutdown_watchdog.done();
  FASTMM_LOG_INFO("fastmm-live: exit code {}", rc);
  return rc;
}

}  // namespace fastmm::live
