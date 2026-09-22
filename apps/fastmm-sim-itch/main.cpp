// fastmm-sim-itch: a Nasdaq-style simulated exchange (ADR-0015, section 6). TotalView-ITCH 5.0
// over MoldUDP64 multicast (lines A and B), MoldUDP64 re-requests, GLIMPSE 5.0 and OUCH 5.0 over
// SoupBinTCP, and the wire-to-wire histogram of orders that name the market data that triggered
// them.
//
//   fastmm-sim-itch --config configs/sim-itch.toml
//   fastmm-sim-itch --cpu 2 --busy-poll --duration 60s --summary-json runs/w2w.json
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 cannot open the sockets.
#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/sim/itch/sim_itch_server.hpp"
#include "fastmm/version.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace fastmm;
using namespace fastmm::sim::itch;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitOpen = 4;

volatile std::sig_atomic_t g_signal = 0;
extern "C" void on_signal(int sig) {
  g_signal = sig;
}

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "usage: fastmm-sim-itch [--config <file.toml>] [options]\n"
      "  --config <file>          [[instruments]] + [sim] configuration (default: FMAA, FMBB)\n"
      "  --bind <ip>              re-request, GLIMPSE and OUCH servers (default 127.0.0.1)\n"
      "  --rerequest-port <n>     MoldUDP64 re-request server, UDP (default 31000, 0 = ephemeral)\n"
      "  --glimpse-port <n>       GLIMPSE 5.0 over SoupBinTCP (default 31010)\n"
      "  --ouch-port <n>          OUCH 5.0 over SoupBinTCP (default 31020)\n"
      "  --line-a <addr:port>     line A: multicast group or unicast address (default\n"
      "                           239.192.0.1:31001)\n"
      "  --line-b <addr:port>     line B (default 239.192.0.2:31002, off = none)\n"
      "  --interface <if>         multicast interface, name or IPv4 address (default lo)\n"
      "  --source <ip>            local address of the multicast socket\n"
      "  --ttl <n>                multicast TTL (default 1)\n"
      "  --drop-a <p>             probability of not sending a data datagram on line A\n"
      "  --drop-b <p>             the same on line B\n"
      "  --drop-seed <n>          seed of the drop draws\n"
      "  --rate <n>               datagrams per second per line (default 0 = unpaced)\n"
      "  --burst <n>              datagrams per sendmmsg call (default 32)\n"
      "  --speed <x>              generator time per wall-clock time (default 1)\n"
      "  --seed <n>               generator seed\n"
      "  --busy-poll              never block waiting for I/O\n"
      "  --cpu <n>                pin the simulator thread to a CPU\n"
      "  --duration <t>           stop after t (e.g. 60s, 5m, 1500ms; default: until "
      "SIGINT/SIGTERM)\n"
      "  --stats-interval <t>     print statistics every t (default 5s, 0 = only at exit)\n"
      "  --summary-json <file>    write the wire-to-wire summary at exit\n"
      "  --version | --help\n"
      "\n"
      "Orders time wire-to-wire when their OUCH ClOrdID is 'T' + 13 digits of the triggering\n"
      "ITCH sequence number. See docs/reference/sim-itch.md.\n");
}

// "60s", "5m", "1500ms", "2h", or a bare number of seconds.
bool parse_duration(std::string_view s, std::int64_t& ns) {
  if (s.empty()) return false;
  std::size_t i = 0;
  std::int64_t v = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    if (v > 10'000'000) return false;
    ++i;
  }
  if (i == 0) return false;
  const std::string_view unit = s.substr(i);
  std::int64_t mult = 1'000'000'000;
  if (unit == "ms") {
    mult = 1'000'000;
  } else if (unit == "s" || unit.empty()) {
    mult = 1'000'000'000;
  } else if (unit == "m") {
    mult = 60'000'000'000;
  } else if (unit == "h") {
    mult = 3'600'000'000'000;
  } else {
    return false;
  }
  ns = v * mult;
  return true;
}

bool parse_uint(std::string_view s, std::uint64_t max, std::uint64_t& out) {
  if (s.empty() || s.size() > 20) return false;
  std::uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (v > max) return false;
  }
  out = v;
  return true;
}

bool parse_double(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0';
}

// "239.1.1.1:31001", or "off" / "" for no line.
bool parse_line(const std::string& s, SimItchLine& line) {
  if (s.empty() || s == "off") {
    line.group.clear();
    line.port = 0;
    return true;
  }
  const std::size_t colon = s.rfind(':');
  std::uint64_t port = 0;
  if (colon == std::string::npos || !parse_uint(s.substr(colon + 1), 65535, port) || port == 0)
    return false;
  line.group = s.substr(0, colon);
  line.port = static_cast<std::uint16_t>(port);
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

int main(int argc, char** argv) {
  std::string config_path;
  std::string summary_path;
  std::optional<std::string> bind;
  std::optional<int> rerequest_port;
  std::optional<int> glimpse_port;
  std::optional<int> ouch_port;
  std::optional<SimItchLine> line_a;
  std::optional<SimItchLine> line_b;
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
  int cpu = -1;
  std::int64_t duration_ns = 0;
  std::int64_t stats_interval_ns = 5'000'000'000;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-sim-itch: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    auto bad = [&](const std::string& v) {
      std::fprintf(stderr, "fastmm-sim-itch: bad %s '%s'\n", argv[i - 1], v.c_str());
      return kExitUsage;
    };
    std::string v;
    std::uint64_t n = 0;
    double d = 0.0;
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return kExitOk;
    }
    if (a == "--version") {
      std::printf("fastmm-sim-itch %s\n", fastmm::build_info());
      return kExitOk;
    }
    if (a == "--busy-poll") {
      busy_poll = true;
      continue;
    }
    if (!a.starts_with("--")) {
      std::fprintf(stderr, "fastmm-sim-itch: unknown argument '%s'\n\n", argv[i]);
      usage(stderr);
      return kExitUsage;
    }
    if (!value(v)) return kExitUsage;
    if (a == "--config") {
      config_path = v;
    } else if (a == "--summary-json") {
      summary_path = v;
    } else if (a == "--bind") {
      bind = v;
    } else if (a == "--interface") {
      interface = v;
    } else if (a == "--source") {
      source = v;
    } else if (a == "--rerequest-port" || a == "--glimpse-port" || a == "--ouch-port") {
      if (!parse_uint(v, 65535, n)) return bad(v);
      std::optional<int>& port = a == "--rerequest-port" ? rerequest_port : ouch_port;
      (a == "--glimpse-port" ? glimpse_port : port) = static_cast<int>(n);
    } else if (a == "--line-a" || a == "--line-b") {
      SimItchLine line;
      if (!parse_line(v, line)) return bad(v);
      (a == "--line-a" ? line_a : line_b) = line;
    } else if (a == "--ttl") {
      if (!parse_uint(v, 255, n)) return bad(v);
      ttl = static_cast<int>(n);
    } else if (a == "--drop-a" || a == "--drop-b") {
      if (!parse_double(v, d) || d < 0.0 || d >= 1.0) return bad(v);
      (a == "--drop-a" ? drop_a : drop_b) = d;
    } else if (a == "--drop-seed" || a == "--seed") {
      if (!parse_uint(v, UINT64_MAX / 10, n)) return bad(v);
      (a == "--seed" ? seed : drop_seed) = n;
    } else if (a == "--rate" || a == "--speed") {
      if (!parse_double(v, d) || d < 0.0 || (a == "--speed" && d == 0.0)) return bad(v);
      (a == "--rate" ? rate : speed) = d;
    } else if (a == "--burst") {
      if (!parse_uint(v, 1024, n) || n == 0) return bad(v);
      burst = static_cast<std::uint32_t>(n);
    } else if (a == "--cpu") {
      if (!parse_uint(v, 4095, n)) return bad(v);
      cpu = static_cast<int>(n);
    } else if (a == "--duration" || a == "--stats-interval") {
      std::int64_t ns = 0;
      const bool is_duration = a == "--duration";
      if (!parse_duration(v, ns) || (is_duration && ns <= 0)) {
        std::fprintf(stderr,
                     "fastmm-sim-itch: bad %s '%s' (examples: 60s, 5m, 1500ms)\n",
                     argv[i - 1],
                     v.c_str());
        return kExitUsage;
      }
      (is_duration ? duration_ns : stats_interval_ns) = ns;
    } else {
      std::fprintf(stderr, "fastmm-sim-itch: unknown argument '%s'\n\n", argv[i - 1]);
      usage(stderr);
      return kExitUsage;
    }
  }

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
  if (cpu >= 0 && !pin_to_cpu(cpu)) {
    std::fprintf(stderr, "fastmm-sim-itch: cannot pin to CPU %d\n", cpu);
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
