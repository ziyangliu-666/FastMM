// fastmm-sim-exchange: a Binance Spot-compatible simulated exchange (plan 8.3, ADR-0008).
//
//   fastmm-sim-exchange --config configs/sim.toml                  # 127.0.0.1:9080 and :9443
//   fastmm-sim-exchange --config configs/sim.toml --bind 0.0.0.0   # docker-compose
//   fastmm-live --config configs/sim-local.toml                    # the engine against it
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 cannot listen.
#include "command_line.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/sim/server/sim_exchange_server.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace {

using namespace fastmm;
using namespace fastmm::sim::server;

constexpr int kExitOk = 0;
constexpr int kExitConfig = 3;
constexpr int kExitListen = 4;

volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int sig) {
  g_signal = sig;
}

std::string decimal(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  const std::size_t n = Price::from_raw(raw).to_decimal(buf);
  return std::string(buf, n);
}

void print_stats(const char* label, const SimServerStats& s) {
  std::string line = "[sim] ";
  line += label;
  auto field = [&line](const char* name, std::uint64_t v) {
    line += ' ';
    line += name;
    line += '=';
    line += std::to_string(v);
  };
  line += " uptime=" + std::to_string(s.uptime_ms / 1000) + "s";
  field("http", s.http_connections);
  field("md_ws", s.md_sessions);
  field("api_ws", s.api_sessions);
  field("user_subs", s.user_subscriptions);
  field("orders", s.orders_accepted);
  field("rejects", s.orders_rejected);
  field("cancels", s.cancels);
  field("replaces", s.replaces);
  field("fills", s.fills);
  field("open", s.open_orders);
  field("trades", s.trades);
  field("depth_diffs", s.depth_updates);
  field("tickers", s.book_tickers);
  field("snapshots", s.depth_snapshots);
  field("rest", s.rest_requests);
  field("ws_api", s.ws_api_requests);
  field("rate_limited", s.rate_limited);
  field("auth_errors", s.signature_errors + s.timestamp_errors);
  line += " position=" + decimal(s.position.raw);
  line += " fees=" + decimal(s.fees.raw);
  line += " pnl=" + decimal(s.pnl.raw);
  line += " bid=" + decimal(s.best_bid.price.raw) + " ask=" + decimal(s.best_ask.price.raw);
  line += '\n';
  std::fputs(line.c_str(), stdout);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::optional<std::string> bind;
  std::optional<int> port;
  std::optional<int> tls_port;
  std::optional<std::string> tls_cert;
  std::optional<std::string> tls_key;
  std::optional<std::uint64_t> seed;
  bool no_tls = false;
  std::int64_t duration_ns = 0;
  std::int64_t stats_interval_ns = 5'000'000'000;

  CLI::App app("A Binance Spot-compatible simulated exchange.", "fastmm-sim-exchange");
  fastmm::cli::setup(app);
  app.footer(
      "Endpoints: REST /api/v3/*, market data /stream?streams=... and /ws/<stream>,\n"
      "WebSocket API /ws-api/v3. The API key/secret come from [sim.account] or\n"
      "FASTMM_SIM_API_KEY / FASTMM_SIM_API_SECRET. See docs/reference/sim-exchange.md.");
  app.add_option("--config",
                 config_path,
                 "[[instruments]] + [sim] configuration (default: built-in BTCUSDT)")
      ->option_text("<file>");
  app.add_option("--bind", bind, "listen address (default 127.0.0.1; 0.0.0.0 for containers)")
      ->option_text("<ip>");
  app.add_option("--port", port, "plain HTTP/WebSocket port (default 9080, 0 = ephemeral)")
      ->option_text("<n>")
      ->check(CLI::Range(0, 65535));
  app.add_option("--tls-port", tls_port, "TLS port (default 9443, 0 = ephemeral)")
      ->option_text("<n>")
      ->check(CLI::Range(0, 65535));
  app.add_flag("--no-tls", no_tls, "do not open the TLS listener");
  app.add_option("--tls-cert", tls_cert, "certificate chain (default tests/fixtures/tls/cert.pem)")
      ->option_text("<pem>");
  app.add_option("--tls-key", tls_key, "private key (default tests/fixtures/tls/key.pem)")
      ->option_text("<pem>");
  app.add_option("--seed", seed, "generator seed (overrides [sim] seed)")->option_text("<n>");
  fastmm::cli::add_duration(app,
                            "--duration",
                            duration_ns,
                            "stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT/SIGTERM)");
  fastmm::cli::add_duration(app,
                            "--stats-interval",
                            stats_interval_ns,
                            "print statistics every t (default 5s, 0 = only at exit)",
                            /*zero_ok=*/true);
  if (const auto rc = fastmm::cli::parse(app, argc, argv)) return *rc;

  SimServerConfig cfg;
  try {
    cfg = config_path.empty() ? SimServerConfig::defaults() : SimServerConfig::load(config_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-sim-exchange: %s\n", e.what());
    return kExitConfig;
  }
  if (bind) cfg.bind_host = *bind;
  if (port) cfg.port = *port;
  if (tls_port) cfg.tls_port = *tls_port;
  if (no_tls) cfg.tls_port = -1;
  if (tls_cert) cfg.tls_cert = *tls_cert;
  if (tls_key) cfg.tls_key = *tls_key;
  if (seed) cfg.seed = *seed;

  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().start(stderr, LogLevel::Warn);
  int rc = kExitOk;
  try {
    SimExchangeServer server(cfg);
    if (!server.listen()) {
      std::fprintf(stderr, "fastmm-sim-exchange: %s\n", server.last_error().c_str());
      Logger::instance().stop();
      return kExitListen;
    }
    struct sigaction sa {};
    sa.sa_handler = &on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const SimServerConfig& c = server.config();
    std::printf("fastmm-sim-exchange %s: seed=%llu symbols=%zu\n",
                fastmm::build_info(),
                static_cast<unsigned long long>(c.seed),
                c.symbols.size());
    if (server.port() != 0) {
      std::printf("  REST      http://%s:%u/api/v3\n", c.bind_host.c_str(), server.port());
      std::printf(
          "  streams   ws://%s:%u/stream?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade\n",
          c.bind_host.c_str(),
          server.port());
      std::printf("  WS API    ws://%s:%u/ws-api/v3\n", c.bind_host.c_str(), server.port());
    }
    if (server.tls_port() != 0) {
      std::printf("  TLS       https://%s:%u  wss://%s:%u/stream  wss://%s:%u/ws-api/v3\n",
                  c.bind_host.c_str(),
                  server.tls_port(),
                  c.bind_host.c_str(),
                  server.tls_port(),
                  c.bind_host.c_str(),
                  server.tls_port());
    }
    std::fflush(stdout);

    const std::int64_t start = steady_now().ns;
    std::int64_t next_stats = start + stats_interval_ns;
    while (g_signal == 0) {
      server.poll(50);
      const std::int64_t now = steady_now().ns;
      if (duration_ns > 0 && now - start >= duration_ns) break;
      if (stats_interval_ns > 0 && now >= next_stats) {
        next_stats += stats_interval_ns;
        print_stats("stats", server.stats());
      }
    }
    std::printf("fastmm-sim-exchange: shutting down (%s)\n",
                g_signal != 0 ? "signal" : "duration elapsed");
    print_stats("final", server.stats());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-sim-exchange: %s\n", e.what());
    rc = kExitConfig;
  }
  Logger::instance().stop();
  return rc;
}
