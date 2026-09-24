// fastmm-sim-itch: a Nasdaq-style simulated exchange (ADR-0015, section 6). TotalView-ITCH 5.0
// over MoldUDP64 multicast (lines A and B), MoldUDP64 re-requests, GLIMPSE 5.0 and OUCH 5.0 over
// SoupBinTCP, and the wire-to-wire histogram of orders that name the market data that triggered
// them.
//
//   fastmm-sim-itch --config configs/sim-itch.toml
//   fastmm-sim-itch --cpu 2 --busy-poll --duration 60s --summary-json runs/w2w.json
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 cannot open the sockets.
#include "command_line.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/sim/itch/sim_itch_server.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <system_error>

namespace {

using namespace fastmm;
using namespace fastmm::sim::itch;

constexpr int kExitOk = 0;
constexpr int kExitConfig = 3;
constexpr int kExitOpen = 4;

volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int sig) {
  g_signal = sig;
}

// "239.1.1.1:31001", or "off" / "" for no line.
bool parse_line(const std::string& s, SimItchLine& line) {
  if (s.empty() || s == "off") {
    line.group.clear();
    line.port = 0;
    return true;
  }
  const std::size_t colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  const char* first = s.data() + colon + 1;
  const char* last = s.data() + s.size();
  std::uint16_t port = 0;
  const auto [end, ec] = std::from_chars(first, last, port);
  if (ec != std::errc{} || end != last || port == 0) return false;
  line.group = s.substr(0, colon);
  line.port = port;
  return true;
}

void print_stats(const char* label, const SimItchServer& server) {
  const SimItchStats s = server.stats();
  std::printf(
      "[sim-itch] %s messages=%llu packets=%llu sent_a=%llu sent_b=%llu dropped_a=%llu "
      "dropped_b=%llu requests=%llu answered=%llu snapshots=%llu ouch_logins=%llu orders=%llu "
      "rejects=%llu executions=%llu w2w=%llu misses=%llu\n",
      label,
      static_cast<unsigned long long>(s.messages),
      static_cast<unsigned long long>(s.packets),
      static_cast<unsigned long long>(s.datagrams_sent[0]),
      static_cast<unsigned long long>(s.datagrams_sent[1]),
      static_cast<unsigned long long>(s.datagrams_dropped[0]),
      static_cast<unsigned long long>(s.datagrams_dropped[1]),
      static_cast<unsigned long long>(s.requests),
      static_cast<unsigned long long>(s.requests_answered),
      static_cast<unsigned long long>(s.snapshots),
      static_cast<unsigned long long>(s.ouch_logins),
      static_cast<unsigned long long>(s.orders),
      static_cast<unsigned long long>(s.order_rejects),
      static_cast<unsigned long long>(s.executions),
      static_cast<unsigned long long>(server.wire_to_wire().count()),
      static_cast<unsigned long long>(s.w2w_misses));
  std::fflush(stdout);
}

}  // namespace

static int run(int argc, char** argv) {
  std::string config_path;
  std::string summary_path;
  std::optional<std::string> bind;
  std::optional<int> rerequest_port;
  std::optional<int> glimpse_port;
  std::optional<int> ouch_port;
  std::optional<std::string> line_a_arg;
  std::optional<std::string> line_b_arg;
  std::optional<std::string> interface;
  std::optional<std::string> source;
  std::optional<int> ttl;
  std::optional<double> drop_a;
  std::optional<double> drop_b;
  std::optional<std::uint64_t> drop_seed;
  std::optional<double> rate;
  std::optional<std::uint32_t> burst;
  std::optional<double> speed;
  std::optional<std::uint64_t> seed;
  bool busy_poll = false;
  std::optional<int> cpu;
  std::int64_t duration_ns = 0;
  std::int64_t stats_interval_ns = 5'000'000'000;

  const CLI::Validator port_range = CLI::Range(0, 65535);
  // A real number `ok` accepts, else "bad '<value>' (<what>)".
  const auto real = [](bool (*ok)(double), const char* what) {
    return CLI::Validator(
        [ok, what](const std::string& v) {
          char* end = nullptr;
          const double d = std::strtod(v.c_str(), &end);
          return !v.empty() && *end == '\0' && ok(d) ? std::string()
                                                     : "bad '" + v + "' (" + what + ")";
        },
        "");
  };
  const CLI::Validator probability =
      real([](double d) { return d >= 0.0 && d < 1.0; }, "a probability, 0 <= p < 1");
  const CLI::Validator line_spec(
      [](const std::string& v) {
        SimItchLine line;
        return parse_line(v, line) ? std::string() : "bad '" + v + "' (<addr>:<port> or off)";
      },
      "");
  CLI::App app("A Nasdaq-style simulated exchange: TotalView-ITCH 5.0, GLIMPSE and OUCH.",
               "fastmm-sim-itch");
  fastmm::cli::setup(app);
  app.footer(
      "Orders time wire-to-wire when their OUCH ClOrdID is 'T' + 13 digits of the triggering\n"
      "ITCH sequence number. See docs/reference/sim-itch.md.");
  app.add_option(
         "--config", config_path, "[[instruments]] + [sim] configuration (default: FMAA, FMBB)")
      ->option_text("<file>");
  app.add_option("--bind", bind, "re-request, GLIMPSE and OUCH servers (default 127.0.0.1)")
      ->option_text("<ip>");
  app.add_option("--rerequest-port",
                 rerequest_port,
                 "MoldUDP64 re-request server, UDP (default 31000, 0 = ephemeral)")
      ->option_text("<n>")
      ->check(port_range);
  app.add_option("--glimpse-port", glimpse_port, "GLIMPSE 5.0 over SoupBinTCP (default 31010)")
      ->option_text("<n>")
      ->check(port_range);
  app.add_option("--ouch-port", ouch_port, "OUCH 5.0 over SoupBinTCP (default 31020)")
      ->option_text("<n>")
      ->check(port_range);
  app.add_option("--line-a",
                 line_a_arg,
                 "line A: multicast group or unicast address (default 239.192.0.1:31001)")
      ->option_text("<addr:port>")
      ->check(line_spec);
  app.add_option("--line-b", line_b_arg, "line B (default 239.192.0.2:31002, off = none)")
      ->option_text("<addr:port>")
      ->check(line_spec);
  app.add_option("--interface", interface, "multicast interface, name or IPv4 address (default lo)")
      ->option_text("<if>");
  app.add_option("--source", source, "local address of the multicast socket")->option_text("<ip>");
  app.add_option("--ttl", ttl, "multicast TTL (default 1)")
      ->option_text("<n>")
      ->check(CLI::Range(0, 255));
  app.add_option("--drop-a", drop_a, "probability of not sending a data datagram on line A")
      ->option_text("<p>")
      ->check(probability);
  app.add_option("--drop-b", drop_b, "the same on line B")->option_text("<p>")->check(probability);
  app.add_option("--drop-seed", drop_seed, "seed of the drop draws")->option_text("<n>");
  app.add_option("--rate", rate, "datagrams per second per line (default 0 = unpaced)")
      ->option_text("<n>")
      ->check(real([](double d) { return d >= 0.0; }, ">= 0"));
  app.add_option("--burst", burst, "datagrams per sendmmsg call (default 32)")
      ->option_text("<n>")
      ->check(CLI::Range(1, 1024));
  app.add_option("--speed", speed, "generator time per wall-clock time (default 1)")
      ->option_text("<x>")
      ->check(real([](double d) { return d > 0.0; }, "> 0"));
  app.add_option("--seed", seed, "generator seed")->option_text("<n>");
  app.add_flag("--busy-poll", busy_poll, "never block waiting for I/O");
  app.add_option("--cpu", cpu, "pin the simulator thread to a CPU")
      ->option_text("<n>")
      ->check(CLI::Range(0, 4095));
  fastmm::cli::add_duration(app,
                            "--duration",
                            duration_ns,
                            "stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT/SIGTERM)");
  fastmm::cli::add_duration(app,
                            "--stats-interval",
                            stats_interval_ns,
                            "print statistics every t (default 5s, 0 = only at exit)",
                            /*zero_ok=*/true);
  app.add_option("--summary-json", summary_path, "write the wire-to-wire summary at exit")
      ->option_text("<file>");
  if (const auto rc = fastmm::cli::parse(app, argc, argv)) return *rc;
  std::optional<SimItchLine> line_a;
  std::optional<SimItchLine> line_b;
  if (line_a_arg) parse_line(*line_a_arg, line_a.emplace());
  if (line_b_arg) parse_line(*line_b_arg, line_b.emplace());

  SimItchConfig cfg;
  try {
    cfg = config_path.empty() ? SimItchConfig::defaults() : SimItchConfig::load(config_path);
    if (bind) cfg.bind_host = *bind;
    if (rerequest_port) cfg.rerequest_port = *rerequest_port;
    if (glimpse_port) cfg.glimpse_port = *glimpse_port;
    if (ouch_port) cfg.ouch_port = *ouch_port;
    if (line_a) {
      line_a->drop_rate = cfg.line_a.drop_rate;
      cfg.line_a = *line_a;
    }
    if (line_b) {
      line_b->drop_rate = cfg.line_b.drop_rate;
      cfg.line_b = *line_b;
    }
    if (interface) cfg.interface = *interface;
    if (source) cfg.source = *source;
    if (ttl) cfg.ttl = *ttl;
    if (drop_a) cfg.line_a.drop_rate = *drop_a;
    if (drop_b) cfg.line_b.drop_rate = *drop_b;
    if (drop_seed) cfg.drop_seed = *drop_seed;
    if (rate) cfg.packet_rate = *rate;
    if (burst) cfg.burst = *burst;
    if (speed) cfg.speed = *speed;
    if (seed) cfg.seed = *seed;
    if (busy_poll) cfg.busy_poll = true;
    cfg.validate();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-sim-itch: %s\n", e.what());
    return kExitConfig;
  }
  if (cpu && !pin_to_cpu(*cpu)) {
    std::fprintf(stderr, "fastmm-sim-itch: cannot pin to CPU %d\n", *cpu);
    return kExitConfig;
  }

  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().start(stderr, LogLevel::Warn);
  int rc = kExitOk;
  try {
    SimItchServer server(cfg);
    if (!server.open()) {
      std::fprintf(stderr, "fastmm-sim-itch: %s\n", server.last_error().c_str());
      Logger::instance().stop();
      return kExitOpen;
    }
    struct sigaction sa {};
    sa.sa_handler = &on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const SimItchConfig& c = server.config();
    std::printf("fastmm-sim-itch %s: seed=%llu symbols=%zu session=%s\n",
                fastmm::build_info(),
                static_cast<unsigned long long>(c.seed),
                c.symbols.size(),
                c.session.c_str());
    for (const SimItchLine* l : {&c.line_a, &c.line_b}) {
      if (l->group.empty()) continue;
      std::printf("  ITCH      udp://%s:%u via %s (drop %.4f)\n",
                  l->group.c_str(),
                  l->port,
                  c.interface.empty() ? "default route" : c.interface.c_str(),
                  l->drop_rate);
    }
    if (server.rerequest_port() != 0)
      std::printf("  requests  udp://%s:%u\n", c.bind_host.c_str(), server.rerequest_port());
    if (server.glimpse_port() != 0)
      std::printf("  GLIMPSE   tcp://%s:%u\n", c.bind_host.c_str(), server.glimpse_port());
    if (server.ouch_port() != 0)
      std::printf("  OUCH      tcp://%s:%u\n", c.bind_host.c_str(), server.ouch_port());
    std::fflush(stdout);

    const std::int64_t start = steady_now().ns;
    std::int64_t next_stats = start + stats_interval_ns;
    while (g_signal == 0) {
      server.poll(c.busy_poll ? 0 : 50);
      const std::int64_t now = steady_now().ns;
      if (duration_ns > 0 && now - start >= duration_ns) break;
      if (stats_interval_ns > 0 && now >= next_stats) {
        next_stats += stats_interval_ns;
        print_stats("stats", server);
      }
    }
    server.end_session();
    std::printf("fastmm-sim-itch: shutting down (%s)\n",
                g_signal != 0 ? "signal" : "duration elapsed");
    print_stats("final", server);
    const LogLinearHistogram& h = server.wire_to_wire();
    std::printf("wire-to-wire: count=%llu p50=%lluns p99=%lluns p99.9=%lluns max=%lluns\n",
                static_cast<unsigned long long>(h.count()),
                static_cast<unsigned long long>(h.percentile(0.50)),
                static_cast<unsigned long long>(h.percentile(0.99)),
                static_cast<unsigned long long>(h.percentile(0.999)),
                static_cast<unsigned long long>(h.max()));
    std::fflush(stdout);
    if (!summary_path.empty()) {
      std::FILE* f = std::fopen(summary_path.c_str(), "w");
      const std::string json = server.summary_json();
      if (f == nullptr || std::fwrite(json.data(), 1, json.size(), f) != json.size()) {
        std::fprintf(stderr, "fastmm-sim-itch: cannot write %s\n", summary_path.c_str());
        rc = kExitConfig;
      }
      if (f != nullptr) std::fclose(f);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-sim-itch: %s\n", e.what());
    rc = kExitConfig;
  }
  Logger::instance().stop();
  return rc;
}

int main(int argc, char** argv) {
  return fastmm::cli::guarded_main("fastmm-sim-itch", [&] { return run(argc, argv); });
}
