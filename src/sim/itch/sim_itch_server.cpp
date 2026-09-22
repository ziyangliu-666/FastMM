// fastmm-sim-itch: construction, the poll loop, re-requests and the summary (sim_itch_server.hpp).
#include "fastmm/sim/itch/sim_itch_server.hpp"

#include "sim_itch_impl.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fastmm::sim::itch {

namespace {

constexpr std::int64_t kNsPerDay = 86'400'000'000'000;
constexpr std::int64_t kTimerPeriodNs = 100'000'000;

codecs::moldudp::TransmitterConfig transmitter_config(const SimItchConfig& c) {
  c.validate();
  codecs::moldudp::TransmitterConfig t;
  t.history_bytes = c.history_bytes;
  t.history_messages = c.history_messages;
  t.max_datagram = c.max_datagram;
  t.overwrite_oldest = true;
  return t;
}

MarketGeneratorParams generator_params(const SimItchConfig& c, const SimItchSymbol& s) {
  MarketGeneratorParams p = c.generator;
  p.start_mid = s.start_mid;
  p.tick = s.tick;
  p.lot = s.lot;
  return p;
}

}  // namespace

Impl::Impl(SimItchConfig c)
    : cfg(std::move(c)),
      tx(cfg.session, transmitter_config(cfg)),
      stamps(cfg.stamp_ring),
      requests(*this),
      glimpse(*this, Listener::Kind::Glimpse),
      ouch(*this, Listener::Kind::Ouch) {
  reactor.set_busy_poll(cfg.busy_poll);
  clock.calibrate(milliseconds(20));
  steady_start_ns = net::Reactor::now_ns();
  const std::int64_t wall = wall_now().ns;
  itch_ts_start = wall % kNsPerDay;
  const Timestamp start = sim_now();
  symbols.reserve(cfg.symbols.size());
  for (std::size_t i = 0; i < cfg.symbols.size(); ++i) {
    Symbol s;
    s.cfg = cfg.symbols[i];
    s.locate = s.cfg.locate != 0 ? s.cfg.locate : static_cast<std::uint16_t>(i + 1);
    s.pub = std::make_unique<ItchPublisher>(*this, numbering, s.locate, s.cfg.symbol);
    s.pub->set_client(this);
    s.engine = std::make_unique<MatchingEngine>(1, s.pub.get());
    s.gen = std::make_unique<MarketGenerator>(generator_params(cfg, s.cfg),
                                              cfg.seed + 0x9E37'79B9'7F4A'7C15ULL * i,
                                              InstrumentId{0},
                                              start);
    by_symbol.emplace(s.cfg.symbol, i);
    symbols.push_back(std::move(s));
  }
}

Impl::~Impl() {
  for (auto& c : conns) {
    c->close();
    c->release();
  }
  if (requests.sock.valid()) reactor.remove(requests.sock.fd());
  if (glimpse.sock.valid()) reactor.remove(glimpse.sock.fd());
  if (ouch.sock.valid()) reactor.remove(ouch.sock.fd());
}

std::uint64_t Impl::itch_ts() const noexcept {
  return static_cast<std::uint64_t>(itch_ts_start + (net::Reactor::now_ns() - steady_start_ns));
}

Timestamp Impl::sim_now() const noexcept {
  const auto elapsed = static_cast<double>(net::Reactor::now_ns() - steady_start_ns) * cfg.speed;
  return Timestamp{1'000'000'000 + static_cast<std::int64_t>(elapsed)};
}

std::size_t Impl::find_symbol(std::string_view symbol) const noexcept {
  const auto it = by_symbol.find(std::string(symbol));
  return it == by_symbol.end() ? std::string::npos : it->second;
}

bool Impl::open() {
  if (opened) return true;
  if (!open_feed() || !open_servers()) return false;
  opened = true;
  next_timer_ns = net::Reactor::now_ns() + kTimerPeriodNs;
  last_send_ns = net::Reactor::now_ns();
  last_refill_ns = last_send_ns;
  return true;
}

bool Impl::open_servers() {
  auto addr_of = [&](int port) {
    return net::SockAddr::from_ip(cfg.bind_host, static_cast<std::uint16_t>(port));
  };
  if (cfg.rerequest_port >= 0) {
    const auto a = addr_of(cfg.rerequest_port);
    requests.sock = net::UdpSocket::open();
    if (!a || !requests.sock.valid() || requests.sock.bind(*a) != 0 ||
        !reactor.add(requests.sock.fd(), requests, net::IoEvent::Read)) {
      error = "cannot bind the re-request server to " + cfg.bind_host + ":" +
              std::to_string(cfg.rerequest_port) + ": " + std::strerror(requests.sock.last_error());
      return false;
    }
  }
  for (Listener* l : {&glimpse, &ouch}) {
    const int port = l == &glimpse ? cfg.glimpse_port : cfg.ouch_port;
    if (port < 0) continue;
    const auto a = addr_of(port);
    if (!a) {
      error = "bad bind address '" + cfg.bind_host + "'";
      return false;
    }
    l->sock = net::TcpSocket::listen(*a);
    if (!l->sock.valid() || !reactor.add(l->sock.fd(), *l, net::IoEvent::Read)) {
      error = std::string("cannot listen on ") + cfg.bind_host + ":" + std::to_string(port) + ": " +
              std::strerror(l->sock.last_error());
      return false;
    }
  }
  return true;
}

int Impl::wait_ms(int max_wait_ms, std::int64_t now_ns) const {
  if (cfg.busy_poll || max_wait_ms <= 0) return 0;
  std::int64_t due = now_ns + static_cast<std::int64_t>(max_wait_ms) * 1'000'000;
  auto earlier = [&due](std::int64_t t) {
    if (t < due) due = t;
  };
  if (tx.next_unsent() <= tx.published()) {
    if (cfg.packet_rate <= 0.0 && cfg.flush_us == 0) return 0;
    earlier(cfg.flush_us != 0 ? first_unsent_ns + std::int64_t{cfg.flush_us} * 1000 : now_ns);
    if (cfg.packet_rate > 0.0 && tokens < 1.0)
      earlier(now_ns + static_cast<std::int64_t>((1.0 - tokens) / cfg.packet_rate * 1e9));
  }
  if (generator_on && cfg.generator_enabled && !ended) {
    for (const Symbol& s : symbols) {
      const Timestamp t = s.gen->next_ts();
      if (t == Timestamp::max()) continue;
      const double rel = static_cast<double>(t.ns - 1'000'000'000) / cfg.speed;
      earlier(steady_start_ns + static_cast<std::int64_t>(rel));
    }
  }
  earlier(last_send_ns + std::int64_t{cfg.heartbeat_ms} * 1'000'000);
  earlier(next_timer_ns);
  if (due <= now_ns) return 0;
  return static_cast<int>((due - now_ns) / 1'000'000);  // epoll rounds up
}

void Impl::poll(int max_wait_ms) {
  if (!opened) return;
  if (!started) start_day();
  std::int64_t now = net::Reactor::now_ns();
  run_market(now);
  if (!ended) flush(now, false);
  reactor.run_once(wait_ms(max_wait_ms, now));
  now = net::Reactor::now_ns();
  run_market(now);
  if (!ended) {
    flush(now, false);
    if (tx.next_unsent() > tx.published() &&
        now - last_send_ns >= std::int64_t{cfg.heartbeat_ms} * 1'000'000) {
      std::array<std::byte, codecs::moldudp::kHeaderLength> hb{};
      send_control({hb.data(), tx.heartbeat(hb)});
      ++stats.heartbeats;
      last_send_ns = now;
    }
  }
  if (now >= next_timer_ns) {
    next_timer_ns = now + kTimerPeriodNs;
    on_timer(now);
  }
  reap();
}

void Impl::on_timer(std::int64_t now_ns) {
  for (auto& c : conns) {
    if (!c->dead()) c->on_timer(now_ns);
  }
}

void Impl::reap() {
  std::size_t w = 0;
  for (std::size_t i = 0; i < conns.size(); ++i) {
    if (conns[i]->dead()) {
      conns[i]->release();
    } else {
      if (i != w) conns[w] = std::move(conns[i]);
      ++w;
    }
  }
  conns.resize(w);
}

void Impl::accept(Listener::Kind kind, net::TcpSocket sock) {
  static_cast<void>(sock.set_nodelay(true));
  const std::int64_t now = net::Reactor::now_ns();
  std::unique_ptr<TcpConn> conn;
  if (kind == Listener::Kind::Glimpse) {
    conn = std::make_unique<GlimpseConn>(*this, std::move(sock), now);
  } else {
    AccountId account = 0;
    for (AccountId a = 1; a < kMaxAccounts; ++a) {
      if (accounts[a] == nullptr) {
        account = a;
        break;
      }
    }
    if (account == 0) return;  // every account in use: the socket closes
    auto oc = std::make_unique<OuchConn>(*this, std::move(sock), account, now);
    accounts[account] = oc.get();
    conn = std::move(oc);
  }
  if (!reactor.add(conn->fd(), *conn, net::IoEvent::Read)) {
    conn->close();
    conn->release();
    return;
  }
  conns.push_back(std::move(conn));
}

void Listener::on_readable() {
  for (;;) {
    net::TcpSocket s = sock.accept();
    if (!s.valid()) return;
    impl_.accept(kind_, std::move(s));
  }
}

void RequestServer::on_readable() {
  for (;;) {
    net::SockAddr from;
    const net::IoResult r = sock.recv_from(in_, &from);
    if (!r.ok() || r.would_block()) return;
    ++impl_.stats.requests;
    const std::size_t n = impl_.tx.answer_request(
        std::span<const std::byte>(in_.data(), r.bytes < in_.size() ? r.bytes : in_.size()), out_);
    if (n == 0) {
      ++impl_.stats.requests_unanswered;
      continue;
    }
    if (sock.send_to(std::span<const std::byte>(out_.data(), n), from).ok())
      ++impl_.stats.requests_answered;
  }
}

// ---- SimItchServer --------------------------------------------------------------------------

SimItchServer::SimItchServer(SimItchConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
SimItchServer::~SimItchServer() = default;

bool SimItchServer::open() {
  return impl_->open();
}
const std::string& SimItchServer::last_error() const noexcept {
  return impl_->error;
}
const SimItchConfig& SimItchServer::config() const noexcept {
  return impl_->cfg;
}

namespace {
std::uint16_t port_of(const net::TcpSocket& s) {
  const auto a = s.valid() ? s.local_addr() : std::nullopt;
  return a ? a->port() : 0;
}
}  // namespace

std::uint16_t SimItchServer::rerequest_port() const noexcept {
  const auto a = impl_->requests.sock.valid() ? impl_->requests.sock.local_addr() : std::nullopt;
  return a ? a->port() : 0;
}
std::uint16_t SimItchServer::glimpse_port() const noexcept {
  return port_of(impl_->glimpse.sock);
}
std::uint16_t SimItchServer::ouch_port() const noexcept {
  return port_of(impl_->ouch.sock);
}

void SimItchServer::poll(int max_wait_ms) {
  impl_->poll(max_wait_ms);
}
void SimItchServer::end_session() {
  impl_->end_session();
}
void SimItchServer::set_generator_enabled(bool on) noexcept {
  impl_->generator_on = on;
}
bool SimItchServer::idle() const noexcept {
  return impl_->tx.next_unsent() > impl_->tx.published();
}
std::uint64_t SimItchServer::published() const noexcept {
  return impl_->tx.published();
}
std::size_t SimItchServer::symbol_count() const noexcept {
  return impl_->symbols.size();
}
const MatchingEngine& SimItchServer::engine(std::size_t symbol) const noexcept {
  return *impl_->symbols[symbol].engine;
}
std::uint64_t SimItchServer::order_ref(std::size_t symbol, std::uint64_t order_id) const noexcept {
  return impl_->symbols[symbol].pub->ref_of(order_id);
}
SimItchStats SimItchServer::stats() const noexcept {
  SimItchStats s = impl_->stats;
  s.history_evicted = impl_->tx.evicted();
  return s;
}
const LogLinearHistogram& SimItchServer::wire_to_wire() const noexcept {
  return impl_->w2w;
}

std::string SimItchServer::summary_json() const {
  const LogLinearHistogram& h = impl_->w2w;
  const SimItchStats s = stats();
  char buf[2048];
  const int n = std::snprintf(buf,
                              sizeof buf,
                              "{\n"
                              "  \"wire_to_wire_ns\": {\"count\": %" PRIu64 ", \"min\": %" PRIu64
                              ", \"p50\": %" PRIu64 ", \"p90\": %" PRIu64 ", \"p99\": %" PRIu64
                              ", \"p999\": %" PRIu64 ", \"max\": %" PRIu64 ", \"mean\": %" PRIu64
                              "},\n"
                              "  \"tokens\": %" PRIu64
                              ",\n"
                              "  \"misses\": %" PRIu64
                              ",\n"
                              "  \"messages\": %" PRIu64
                              ",\n"
                              "  \"packets\": %" PRIu64
                              ",\n"
                              "  \"datagrams_sent\": [%" PRIu64 ", %" PRIu64
                              "],\n"
                              "  \"datagrams_dropped\": [%" PRIu64 ", %" PRIu64
                              "],\n"
                              "  \"send_errors\": %" PRIu64
                              ",\n"
                              "  \"requests\": %" PRIu64
                              ",\n"
                              "  \"requests_answered\": %" PRIu64
                              ",\n"
                              "  \"snapshots\": %" PRIu64
                              ",\n"
                              "  \"orders\": %" PRIu64
                              ",\n"
                              "  \"executions\": %" PRIu64
                              "\n"
                              "}\n",
                              h.count(),
                              h.min(),
                              h.percentile(0.50),
                              h.percentile(0.90),
                              h.percentile(0.99),
                              h.percentile(0.999),
                              h.max(),
                              h.mean(),
                              s.w2w_tokens,
                              s.w2w_misses,
                              s.messages,
                              s.packets,
                              s.datagrams_sent[0],
                              s.datagrams_sent[1],
                              s.datagrams_dropped[0],
                              s.datagrams_dropped[1],
                              s.send_errors,
                              s.requests,
                              s.requests_answered,
                              s.snapshots,
                              s.orders,
                              s.executions);
  return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
}

}  // namespace fastmm::sim::itch
