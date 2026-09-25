// fastmm-gateway: the venue connections and their network threads, and one strategy process
// attached over shared-memory rings at a time (live/gateway.hpp).
#include "fastmm/live/gateway.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

// What the gateway keeps per venue besides the slot. `out` is the attached strategy's outbound
// ring; only the venue's network thread touches it (set and cleared by posted tasks).
struct GatewayVenueState {
  VenueSlot* slot = nullptr;
  ShmRing* out = nullptr;
  std::atomic<std::uint64_t> md_discarded{0};
  std::atomic<std::uint64_t> order_discarded{0};
};

// The network thread's hook: throws away what the venue emitted while no strategy is attached
// (the sinks point at the slot's local rings then) and moves the attached strategy's orders from
// its shared ring into the venue's local outbound ring, which on_wake() drains into the wire the
// same way it does for fastmm-live. One memcpy of each order (128 bytes) buys that no connector
// needs to know where its orders come from.
std::size_t gateway_hook(void* ctx) noexcept {
  auto& g = *static_cast<GatewayVenueState*>(ctx);
  VenueSlot& s = *g.slot;
  std::size_t n = 0;
  if (!s.md_ring->empty_approx()) {
    std::uint64_t k = 0;
    while (s.md_ring->try_peek() != nullptr) {
      s.md_ring->release();
      ++k;
    }
    g.md_discarded.fetch_add(k, std::memory_order_relaxed);
    n += k;
  }
  if (!s.order_ring->empty_approx()) {
    std::uint64_t k = 0;
    while (s.order_ring->try_peek() != nullptr) {
      s.order_ring->release();
      ++k;
    }
    g.order_discarded.fetch_add(k, std::memory_order_relaxed);
    n += k;
  }
  if (g.out != nullptr) {
    std::size_t moved = 0;
    while (const std::byte* p = g.out->try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      if (!s.outbound->try_push(p, h->len)) break;  // the venue catches up first
      g.out->release();
      ++moved;
    }
    if (moved != 0) {
      s.venue->on_wake();
      n += moved;
    }
  }
  return n;
}

// VenueSlot::Pending: the recheck of an adaptive network thread about to block. The engine takes
// the thread's flag in the wake page after it pushed an order (GatewayClient::wake_venue).
bool gateway_pending(void* ctx) noexcept {
  const auto& g = *static_cast<const GatewayVenueState*>(ctx);
  return g.out != nullptr && !g.out->empty_approx();
}

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
  struct Rings {
    std::unique_ptr<ShmRing> md;
    std::unique_ptr<ShmRing> order;
    std::unique_ptr<ShmRing> outbound;
    std::array<std::string, 3> paths;
  };
  std::vector<Rings> rings;  // per venue
  std::vector<std::uint64_t> overflows_at_attach;
  // The wake page (live/gateway.hpp) and this process's Waker over the engine's flag in it.
  gw::WakePage* page = nullptr;
  std::unique_ptr<Waker> engine_waker;
};

class Gateway {
 public:
  Gateway(const Config& cfg, const GatewayOptions& opts, VenueSlots& slots, InstrumentTable& insts)
      : cfg_(cfg), opts_(opts), slots_(slots), instruments_(insts), state_(slots.size()) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      state_[i].slot = slots[i].get();
      slots[i]->hook = &gateway_hook;
      slots[i]->pending = &gateway_pending;
      slots[i]->hook_ctx = &state_[i];
    }
  }

  [[nodiscard]] bool attached() const noexcept { return att_.fd >= 0; }
  [[nodiscard]] int fd() const noexcept { return att_.fd; }

  // A new connection on the listener. Answers it and keeps it as the attachment, or refuses it.
  void on_connection(int fd) {
    // The request follows the connect at once; a client that sends nothing is dropped.
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 2000) <= 0) {
      FASTMM_LOG_WARN("gateway: a connection sent no attach request");
      ::close(fd);
      return;
    }
    std::vector<std::byte> buf(sizeof(gw::AttachRequest) +
                               gw::kMaxKnownExecIds * sizeof(gw::ExecId));
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
    if (req.known_count > gw::kMaxKnownExecIds ||
        static_cast<std::size_t>(n) != sizeof req + req.known_count * sizeof(gw::ExecId)) {
      send_error(fd, "malformed attach request");
      ::close(fd);
      return;
    }
    const std::string engine = from_field(req.engine);
    if (attached()) {
      FASTMM_LOG_WARN("gateway: refused {} (pid {}): {} (pid {}) is attached",
                      std::string_view(engine),
                      req.pid,
                      std::string_view(att_.engine),
                      att_.pid);
      send_error(fd, "another strategy (" + att_.engine + ") is attached to this gateway");
      ::close(fd);
      return;
    }
    std::vector<std::string> known;
    known.reserve(req.known_count);
    for (std::uint32_t i = 0; i < req.known_count; ++i) {
      gw::ExecId id{};
      std::memcpy(&id, buf.data() + sizeof req + i * sizeof id, sizeof id);
      known.push_back(from_field(id.id));
    }
    attach(fd, req, engine, known);
  }

  // The attachment's connection closed (or misbehaved): the strategy is gone.
  void detach(std::string_view why) {
    if (!attached()) return;
    const std::int64_t t0 = steady_now().ns;
    // First stop its orders from reaching the wire, then cancel what rests.
    on_net_threads(slots_, [this](std::size_t i) {
      VenueSlot& s = *slots_[i];
      state_[i].out = nullptr;
      s.md_sink.attach(s.md_ring.get(), venues::SinkPolicy::Drop);
      s.order_sink.attach(s.order_ring.get(), venues::SinkPolicy::Spin);
      s.consumer = nullptr;
      s.blocked = &s.net_blocked;
    });
    gw::unmap_wake_page(att_.page);
    att_.page = nullptr;
    att_.engine_waker.reset();
    bool cancel_ok = true;
    if (!opts_.dry_run) {
      for (auto& s : slots_) cancel_ok = s->venue->cancel_all() && cancel_ok;
    }
    const std::int64_t ms = (steady_now().ns - t0) / 1'000'000;
    for (const Attachment::Rings& r : att_.rings) {
      std::error_code ec;
      for (const std::string& p : r.paths) std::filesystem::remove(p, ec);
    }
    att_.rings.clear();
    ::close(att_.fd);
    att_.fd = -1;
    const double held_s = static_cast<double>(steady_now().ns - att_.since_ns) / 1e9;
    if (cancel_ok) {
      FASTMM_LOG_WARN(
          "gateway: {} (pid {}, attachment {}) detached after {:.1f} s ({}): its sinks discard, "
          "cancel_all ok in {} ms",
          std::string_view(att_.engine),
          att_.pid,
          att_.id,
          held_s,
          why,
          ms);
    } else {
      FASTMM_LOG_ERROR(
          "gateway: {} (pid {}, attachment {}) detached after {:.1f} s ({}): cancel_all FAILED in "
          "{} ms; orders may still be resting",
          std::string_view(att_.engine),
          att_.pid,
          att_.id,
          held_s,
          why,
          ms);
    }
    cancel_failed_ = cancel_failed_ || !cancel_ok;
  }

  // The strategy stopped reading its order ring: the venue could not hand it an order event.
  void check_overflows() {
    if (!attached()) return;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i]->order_overflows.load(std::memory_order_relaxed) !=
          att_.overflows_at_attach[i]) {
        FASTMM_LOG_ERROR("gateway: [{}] the attached strategy's order ring is full",
                         slots_[i]->venue->name());
        detach("its order ring overflowed");
        return;
      }
    }
  }

  void log_discards() const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      const std::uint64_t md = state_[i].md_discarded.load(std::memory_order_relaxed);
      const std::uint64_t order = state_[i].order_discarded.load(std::memory_order_relaxed);
      if (md == 0 && order == 0) continue;
      FASTMM_LOG_INFO("gateway: [{}] discarded while no strategy was attached: md={} order={}",
                      slots_[i]->venue->name(),
                      md,
                      order);
    }
  }

  [[nodiscard]] bool cancel_failed() const noexcept { return cancel_failed_; }

 private:
  void attach(int fd,
              const gw::AttachRequest& req,
              const std::string& engine,
              const std::vector<std::string>& known) {
    const std::uint32_t id = ++next_id_;
    Attachment a;
    a.id = id;
    a.engine = engine;
    a.pid = req.pid;
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
        std::error_code ec;
        for (const Attachment::Rings& made : a.rings)
          for (const std::string& p : made.paths) std::filesystem::remove(p, ec);
        for (const std::string& p : r.paths) std::filesystem::remove(p, ec);
        gw::unmap_wake_page(a.page);
        return;
      }
      r.md = std::make_unique<ShmRing>(std::move(*md));
      r.order = std::make_unique<ShmRing>(std::move(*order));
      r.outbound = std::make_unique<ShmRing>(std::move(*out));
      a.rings.push_back(std::move(r));
    }
    const bool resume = (req.flags & gw::kResumeExecutions) != 0;
    // On each network thread, between two of its callbacks: from here on the venue writes into the
    // strategy's rings, starting with fresh books and the account's truth (executions, then the
    // open orders), so the strategy's OMS starts from what the venue holds.
    on_net_threads(slots_, [&](std::size_t i) {
      VenueSlot& s = *slots_[i];
      s.md_sink.attach(a.rings[i].md.get(), venues::SinkPolicy::Drop);
      s.order_sink.attach(a.rings[i].order.get(), venues::SinkPolicy::Spin);
      state_[i].out = a.rings[i].outbound.get();
      s.blocked = &a.page->net[i].flag;
      s.consumer = strategy_blocks ? a.engine_waker.get() : nullptr;
      s.venue->resync_books();
      if (resume && executions(i)) {
        s.venue->resume_executions(req.exec_since_ms, known);
        static_cast<void>(s.venue->request_executions(req.exec_since_ms));
      }
      s.venue->request_open_orders();
    });
    a.overflows_at_attach.resize(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i)
      a.overflows_at_attach[i] = slots_[i]->order_overflows.load(std::memory_order_relaxed);

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
    att_ = std::move(a);
    log_discards();
    std::vector<int> fds{page_fd.fd};
    for (const auto& s : slots_) fds.push_back(s->reactor->wake_fd());
    if (gw::send_with_fds(fd, out.data(), out.size(), fds) != static_cast<ssize_t>(out.size())) {
      FASTMM_LOG_ERROR("gateway: cannot answer {} (pid {}): {}",
                       std::string_view(engine),
                       req.pid,
                       std::strerror(errno));
      detach("the attach reply could not be sent");
      return;
    }
    FASTMM_LOG_WARN(
        "gateway: {} (pid {}) attached as attachment {}: {} venue(s), {} instrument(s), "
        "reconciling{}",
        std::string_view(engine),
        req.pid,
        id,
        slots_.size(),
        instruments_.size(),
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
  std::vector<GatewayVenueState> state_;
  Attachment att_;
  std::uint32_t next_id_ = 0;
  bool cancel_failed_ = false;
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

  const std::string path = opts.socket_path.empty() ? default_gateway_path(cfg) : opts.socket_path;
  std::string err;
  const int listen_fd = listen_seqpacket(path, &err);
  if (listen_fd < 0) {
    std::fprintf(stderr, "%s: gateway socket %s: %s\n", prog, path.c_str(), err.c_str());
    return kExitConfig;
  }

  Gateway gateway(cfg, opts, slots, instruments);
  const GatewaySignals signals;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const int cpu = i < cfg.engine.net_cpus.size() ? cfg.engine.net_cpus[i] : -1;
    slots[i]->thread = std::thread(net_loop, std::ref(*slots[i]), cpu, i, cfg.spin_mode());
  }
  FASTMM_LOG_INFO(
      "fastmm-gateway: {} venue(s), {} instrument(s), dry_run={}, net={}; attach with "
      "fastmm-live --gateway {}",
      slots.size(),
      instruments.size(),
      opts.dry_run,
      net::to_string(net_backend),
      path);

  const std::int64_t start = steady_now().ns;
  std::int64_t next_tick = start + 1'000'000'000;
  while (g_gw_signal == 0) {
    const std::int64_t now = steady_now().ns;
    if (opts.duration_ns > 0 && now - start >= opts.duration_ns) break;
    pollfd fds[2] = {{listen_fd, POLLIN, 0}, {gateway.fd(), POLLIN | POLLRDHUP, 0}};
    const nfds_t nfds = gateway.attached() ? 2 : 1;
    // Wakes at once when the strategy's connection closes; otherwise at least every 50 ms for the
    // signals, the overflow check and the once-a-second housekeeping.
    const int pr = ::poll(fds, nfds, 50);
    if (pr > 0 && nfds == 2 && fds[1].revents != 0) {
      char b[64];
      const ssize_t n = ::recv(gateway.fd(), b, sizeof b, MSG_DONTWAIT);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        gateway.detach(n == 0 ? "its connection closed" : std::strerror(errno));
      } else if (n > 0) {
        FASTMM_LOG_WARN("gateway: ignored {} byte(s) from the attached strategy", n);
      }
    }
    if (pr > 0 && (fds[0].revents & POLLIN) != 0) {
      const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) gateway.on_connection(fd);
    }
    gateway.check_overflows();
    if (steady_now().ns >= next_tick) {
      next_tick += 1'000'000'000;
      for (auto& s : slots) {
        venues::Venue* v = s->venue.get();
        s->reactor->post([v] { v->on_timer(net::Reactor::now_ns()); });
        log_venue_status(*v);
      }
    }
  }
  FASTMM_LOG_WARN("fastmm-gateway: shutting down ({})",
                  g_gw_signal != 0 ? std::string_view("signal") : std::string_view("duration"));
  ::close(listen_fd);
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  gateway.detach("the gateway is shutting down");
  bool cancel_ok = !gateway.cancel_failed();
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
  gateway.log_discards();
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
