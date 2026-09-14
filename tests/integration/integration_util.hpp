#pragma once
// Shared helpers for the sim-exchange integration tests: an in-process SimExchangeServer on
// ephemeral ports, ring-backed sinks for driving BinanceVenue directly, and LiveEngine, the
// apps/fastmm-live wiring (venue reactor thread + Engine<BasicMM, TscClock, LiveTransport,
// RingFeed> thread + kill-switch shutdown) built from configs/sim-local(-tls).toml. With
// LiveEngineOptions::journal_path it also journals like fastmm-live (session epoch, cancel-replace
// from venue capability && config, effective config, TSC calibration source).
//
// Set FASTMM_IT_LOG=1 to see the connector/engine log on stderr.
#include "test_support.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/sim/server/sim_exchange_server.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/venue_factory.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fastmm::integration {

inline constexpr const char* kApiKey = "it-key";
inline constexpr const char* kApiSecret = "it-secret";

inline void ensure_logging() {
  static const bool started = [] {
    const char* v = std::getenv("FASTMM_IT_LOG");
    if (v == nullptr || *v == '\0' || *v == '0') return false;
    Logger::instance().set_level(LogLevel::Info);
    Logger::instance().start(stderr, LogLevel::Warn);
    std::atexit([] { Logger::instance().stop(); });
    return true;
  }();
  static_cast<void>(started);
}

inline std::filesystem::path repo_root() {
  return (fastmm::test::fixtures_dir() / ".." / "..").lexically_normal();
}

// FASTMM_TEST_NET_BACKEND=io_uring runs the simulator's and the connector's reactors on io_uring
// (epoll when unset, or when io_uring is not available).
inline net::ReactorBackend test_net_backend() {
  net::ReactorBackend backend = net::ReactorBackend::Epoll;
  if (const char* env = std::getenv("FASTMM_TEST_NET_BACKEND"); env != nullptr)
    static_cast<void>(net::parse_reactor_backend(env, backend));
  return net::Reactor::resolve_backend(backend);
}

// Ephemeral ports, the fixture certificate, a faster counter-party flow than configs/sim.toml
// so fills happen within a second or two, and frequent server pings.
inline sim::server::SimServerConfig test_server_config(bool tls = false) {
  sim::server::SimServerConfig c = sim::server::SimServerConfig::defaults();
  c.port = 0;
  c.tls_port = tls ? 0 : -1;
  c.tls_cert = (fastmm::test::fixtures_dir() / "tls" / "cert.pem").string();
  c.tls_key = (fastmm::test::fixtures_dir() / "tls" / "key.pem").string();
  c.api_key = kApiKey;
  c.api_secret = kApiSecret;
  c.generator.market_rate_per_s = 25.0;
  c.generator.regimes = false;
  c.ping_interval_ms = 1000;
  // Serve depth snapshots at batch boundaries: deterministic first sync. The live-book mode is
  // covered by its own conformance test (see the BookSyncer note there).
  c.snapshot_at_flush = true;
  c.net_backend = test_net_backend();
  return c;
}

struct ServerFixture {
  sim::server::SimExchangeServer server;

  explicit ServerFixture(sim::server::SimServerConfig cfg = test_server_config())
      : server(std::move(cfg)) {
    ensure_logging();
    REQUIRE_MESSAGE(server.listen(), server.last_error());
    server.start();
  }
  ~ServerFixture() { server.stop(); }
  ServerFixture(const ServerFixture&) = delete;
  ServerFixture& operator=(const ServerFixture&) = delete;

  [[nodiscard]] std::string http() const {
    return "http://127.0.0.1:" + std::to_string(server.port());
  }
  [[nodiscard]] std::string ws() const { return "ws://127.0.0.1:" + std::to_string(server.port()); }
  [[nodiscard]] std::string https() const {
    return "https://127.0.0.1:" + std::to_string(server.tls_port());
  }
  [[nodiscard]] std::string wss() const {
    return "wss://127.0.0.1:" + std::to_string(server.tls_port());
  }
};

// Connector and simulator state for failure messages: INFO(describe(...)) is evaluated only when
// an assertion fails, so it shows the state at that moment. Engine-side reasons (risk rejects) are
// in the FASTMM_IT_LOG=1 log.
inline std::string describe(const venues::VenueStatus& v, const sim::server::SimServerStats& s) {
  std::ostringstream o;
  o << "venue: md=" << venues::to_string(v.md) << " order=" << venues::to_string(v.order)
    << " user=" << venues::to_string(v.user) << " books_synced=" << v.books_synced
    << " resyncs=" << v.resyncs << " md_messages=" << v.md_messages
    << " orders_sent=" << v.orders_sent << " order_events=" << v.order_events
    << " rest_errors=" << v.rest_errors << " reconnects=" << v.reconnects
    << " | server: open_orders=" << s.open_orders << " accepted=" << s.orders_accepted
    << " rejected=" << s.orders_rejected << " fills=" << s.fills
    << " depth_snapshots=" << s.depth_snapshots;
  return o.str();
}

// Sleeps in 5 ms steps until pred() or the timeout; returns pred().
template <class Pred>
bool wait_until(Pred pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return pred();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// Pumps `reactor` on this thread until pred() or the timeout; returns pred().
template <class Pred>
bool pump_until(net::Reactor& reactor, Pred pred, int timeout_ms) {
  const std::int64_t deadline =
      net::Reactor::now_ns() + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
  while (!pred()) {
    if (net::Reactor::now_ns() >= deadline) return pred();
    reactor.run_once(5);
  }
  return true;
}

inline std::size_t open_fd_count() {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) ++n;
  return n;
}

// Copies every committed message out of a MsgRing-backed sink.
struct RecordingSink {
  MsgRing ring;
  venues::EventSink sink;
  explicit RecordingSink(std::size_t bytes, venues::SinkPolicy policy = venues::SinkPolicy::Drop)
      : ring(bytes), sink(&ring, policy, 10) {}

  void drain_into(std::vector<std::vector<std::byte>>& out) {
    while (const std::byte* p = ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      out.emplace_back(p, p + h->len);
      ring.release();
    }
  }
};

struct Collected {
  std::vector<std::vector<std::byte>> all;

  void take(RecordingSink& s) { s.drain_into(all); }
  [[nodiscard]] static EventType type_of(const std::vector<std::byte>& m) {
    return reinterpret_cast<const EventHeader*>(m.data())->type;
  }
  template <class M>
  [[nodiscard]] static const M& as(const std::vector<std::byte>& m) {
    return *reinterpret_cast<const M*>(m.data());
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& m : all) n += type_of(m) == t ? 1U : 0U;
    return n;
  }
  // Messages of type M satisfying pred.
  template <class M, class Pred>
  [[nodiscard]] std::size_t count_if(EventType t, Pred pred) const {
    std::size_t n = 0;
    for (const auto& m : all) {
      if (type_of(m) == t && pred(as<M>(m))) ++n;
    }
    return n;
  }
  template <class M>
  [[nodiscard]] const M* last(EventType t) const {
    const M* out = nullptr;
    for (const auto& m : all) {
      if (type_of(m) == t) out = &as<M>(m);
    }
    return out;
  }
};

// configs/sim-local.toml (or -tls) pointed at the in-process server.
inline Config sim_local_config(const ServerFixture& fx, bool tls) {
  std::string text = fastmm::test::read_file(repo_root() / "configs" /
                                             (tls ? "sim-local-tls.toml" : "sim-local.toml"));
  auto replace_all = [&text](std::string_view from, const std::string& to) {
    for (std::size_t p = text.find(from); p != std::string::npos;
         p = text.find(from, p + to.size()))
      text.replace(p, from.size(), to);
  };
  replace_all("127.0.0.1:9080", "127.0.0.1:" + std::to_string(fx.server.port()));
  replace_all("127.0.0.1:9443", "127.0.0.1:" + std::to_string(fx.server.tls_port()));
  replace_all("\"tests/fixtures/tls/cert.pem\"",
              "\"" + (fastmm::test::fixtures_dir() / "tls" / "cert.pem").string() + "\"");
  Config::LoadOptions opts;
  opts.substitute_env = false;
  Config cfg = Config::parse(text, opts, tls ? "sim-local-tls.toml" : "sim-local.toml");
  REQUIRE(cfg.venues.size() == 1);
  cfg.venues[0].api_key = kApiKey;
  cfg.venues[0].api_secret = kApiSecret;
  cfg.engine.journal = false;
  return cfg;
}

struct LiveEngineOptions {
  std::string journal_path;         // empty: no journal
  std::uint16_t session_epoch = 1;  // fastmm-live takes it from SessionEpochStore
};

// The fastmm-live session wiring (src/live/session.cpp) for one venue and BasicMM.
class LiveEngine {
 public:
  using EngineT = Engine<BasicMM, TscClock, LiveTransport, RingFeed>;

  explicit LiveEngine(Config cfg, LiveEngineOptions opts = {})
      : cfg_(std::move(cfg)),
        reactor_(test_net_backend()),
        md_ring_(ring_bytes(cfg_.engine.md_ring_bytes)),
        order_ring_(ring_bytes(cfg_.engine.order_ring_bytes)),
        outbound_(ring_bytes(cfg_.engine.order_ring_bytes)),
        control_(1U << 16) {
    ensure_logging();
    instruments_ = load_instruments(cfg_);
    venue_ = venues::make_venue(VenueId{0}, cfg_.venues[0], venues::VenueFactoryOptions{});
    const auto ref = venue_->load_reference_data(instruments_);
    REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
    REQUIRE(symbols_.build(instruments_));
    clock_.calibrate();
    initial_tsc_ = clock_.calibration();
    tsc_pub_.store(initial_tsc_);
    clock_.attach_calibration_source(&tsc_pub_,
                                     TscClock::kDefaultStepThreshold,
                                     cfg_.engine.tsc_recalibrate_s > 0
                                         ? seconds(cfg_.engine.tsc_recalibrate_s)
                                         : TscClock::kDefaultSlewHorizon);
    md_sink_.attach(&md_ring_, venues::SinkPolicy::Drop);
    order_sink_.attach(&order_ring_, venues::SinkPolicy::Spin);
    static_cast<void>(feed_.add_ring(&control_));
    static_cast<void>(feed_.add_ring(&order_ring_));
    static_cast<void>(feed_.add_ring(&md_ring_));
    replace_ = venue_->caps().supports_replace && cfg_.venues[0].supports_replace;
    transport_.set_venue(VenueId{0}, &outbound_, replace_);
    venue_->attach(symbols_, instruments_, md_sink_, order_sink_, &outbound_);
    std::vector<InstrumentId> ids;
    for (const Instrument& inst : instruments_) ids.push_back(inst.id);
    venue_->subscribe(ids);
    transport_.set_wake_hook(&LiveEngine::wake, this);

    EngineConfig ec;
    ec.session_id = static_cast<std::uint64_t>(wall_now().ns);
    ec.rng_seed = cfg_.engine.rng_seed;
    ec.session_epoch = opts.session_epoch;
    ec.max_events_per_step = cfg_.engine.max_events_per_step;
    ec.crossed_grace = milliseconds(cfg_.engine.crossed_grace_ms);
    ec.latency_publish_interval = milliseconds(cfg_.engine.latency_publish_ms);
    ec.cpu = -1;
    ec.spin_mode = cfg_.spin_mode();
    ec.risk = cfg_.risk_limits();
    ec.quotes = cfg_.quote_params();
    ec.quoting_enabled = true;
    const auto err = strategy_.configure(cfg_.strategy.params);
    REQUIRE_MESSAGE(!err, (err ? *err : std::string()));
    if (!opts.journal_path.empty()) {
      const std::string effective = cfg_.effective_toml();
      JournalSessionInfo info;
      info.session_id = ec.session_id;
      info.start_ts = wall_now();
      info.tsc = initial_tsc_;
      info.config_hash = Config::text_hash(effective);
      info.rng_seed = ec.rng_seed;
      info.strategy = BasicMM::name();
      info.instruments = &instruments_;
      info.has_session = true;
      info.session_epoch = ec.session_epoch;
      info.quoting_enabled = ec.quoting_enabled;
      info.replace_venues = replace_ ? 1U : 0U;
      info.config_toml = effective;
      journal_ring_ = std::make_unique<MsgRing>(ring_bytes(cfg_.engine.journal_ring_bytes));
      journal_ = std::make_unique<JournalFileWriter>(*journal_ring_, opts.journal_path, info);
      REQUIRE_MESSAGE(journal_->ok(), "cannot open journal " << opts.journal_path);
    }
    engine_ = std::make_unique<EngineT>(
        ec, instruments_, clock_, transport_, feed_, strategy_, journal_ring_.get());
  }
  ~LiveEngine() { stop(); }
  LiveEngine(const LiveEngine&) = delete;
  LiveEngine& operator=(const LiveEngine&) = delete;

  void start() {
    if (journal_) journal_->start();
    net_thread_ = std::thread([this] { net_loop(); });
    engine_thread_ = std::thread([this] { engine_->run(); });
    started_ = true;
  }

  // Kill switch -> independent REST cancel_all -> stop the engine, then the net thread.
  void stop() {
    if (!started_ || stopped_) return;
    stopped_ = true;
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.command = ControlCommand::TripKill;
    m.hdr.recv_ts = wall_now();
    kill_pushed_ = control_.try_push(&m, m.hdr.len);
    cancel_all_ok_ = venue_->cancel_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine_->stop();
    engine_thread_.join();
    net_stop_.store(true, std::memory_order_release);
    reactor_.wake();
    net_thread_.join();
    if (journal_) journal_->stop();
  }

  // What the control thread's TscCalibrator would publish; the engine picks it up in its next
  // step (a step when it disagrees with the engine's mapping by more than 1 ms).
  void publish_tsc(const TscCalibration& c) { tsc_pub_.store(c); }
  [[nodiscard]] const TscCalibration& initial_tsc() const noexcept { return initial_tsc_; }
  [[nodiscard]] bool supports_replace() const noexcept { return replace_; }

  [[nodiscard]] venues::Venue& venue() noexcept { return *venue_; }
  [[nodiscard]] EngineT& engine() noexcept { return *engine_; }  // after stop() only
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }
  [[nodiscard]] bool cancel_all_ok() const noexcept { return cancel_all_ok_; }
  [[nodiscard]] bool kill_pushed() const noexcept { return kill_pushed_; }

 private:
  static std::size_t ring_bytes(std::size_t bytes) {
    std::size_t n = 1U << 16;
    while (n < bytes) n <<= 1U;
    return n;
  }
  static void wake(void* ctx, VenueId) noexcept {
    auto* self = static_cast<LiveEngine*>(ctx);
    self->wake_.store(true, std::memory_order_release);
    self->reactor_.wake();
  }
  void net_loop() {
    venue_->connect(reactor_);
    while (!net_stop_.load(std::memory_order_acquire)) {
      reactor_.run_once(1);
      if (wake_.exchange(false, std::memory_order_acquire)) venue_->on_wake();
    }
    venue_->on_wake();
    for (int i = 0; i < 20; ++i) reactor_.run_once(5);
    venue_->disconnect();
    reactor_.run_once(0);
  }

  Config cfg_;
  net::Reactor reactor_;  // declared before venue_: the venue's channels reference it
  InstrumentTable instruments_;
  std::unique_ptr<venues::Venue> venue_;
  venues::SymbolTable symbols_;
  TscClock clock_;
  TscCalibration initial_tsc_{};
  Seqlocked<TscCalibration> tsc_pub_;
  LiveTransport transport_;
  RingFeed feed_;
  MsgRing md_ring_;
  MsgRing order_ring_;
  MsgRing outbound_;
  MsgRing control_;
  venues::EventSink md_sink_;
  venues::EventSink order_sink_;
  BasicMM strategy_;
  std::unique_ptr<MsgRing> journal_ring_;
  std::unique_ptr<JournalFileWriter> journal_;
  bool replace_ = false;
  std::unique_ptr<EngineT> engine_;
  std::atomic<bool> wake_{false};
  std::atomic<bool> net_stop_{false};
  std::thread net_thread_;
  std::thread engine_thread_;
  bool started_ = false;
  bool stopped_ = false;
  bool cancel_all_ok_ = false;
  bool kill_pushed_ = false;
};

}  // namespace fastmm::integration
