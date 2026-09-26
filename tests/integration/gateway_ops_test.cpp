// fastmm-gateway's operability: its status segment (fastmm-top, --json and --metrics) and its
// control socket (fastmm-ctl --path). Two strategies trade BTCUSDT and ETHUSDT on the simulator
// through one gateway; everything is a real child process.
#include "gateway_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/gateway.hpp"

#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE) && defined(FASTMM_TOP_EXE) && \
    defined(FASTMM_CTL_EXE)

namespace {

GatewayProcess spawn_gateway_with(const SessionFiles& f, const std::vector<std::string>& extra) {
  GatewayProcess g;
  g.socket = f.config + ".gw";
  g.log = f.config + ".gw.log";
  remove_all_of({g.socket, g.log});
  std::vector<std::string> args{"--config", f.config, "--socket", g.socket, "--log", g.log};
  args.insert(args.end(), extra.begin(), extra.end());
  g.pid = spawn_process(FASTMM_GATEWAY_EXE, args);
  return g;
}

// A program run to completion: its exit code and what it printed on stdout.
struct Output {
  int rc = -1;
  std::string out;
};
Output run(const char* exe, const std::vector<std::string>& args) {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  std::vector<const char*> argv{exe};
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc =
      posix_spawn(&pid, exe, &fa, nullptr, const_cast<char* const*>(argv.data()), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  REQUIRE_MESSAGE(rc == 0, "posix_spawn " << exe << " failed");
  Output o;
  char buf[4096];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;)
    o.out.append(buf, static_cast<std::size_t>(n));
  ::close(fds[0]);
  o.rc = reap(pid);
  return o;
}

Output ctl(const GatewayProcess& g, const std::vector<std::string>& command) {
  std::vector<std::string> args{"--path", g.socket + ".ctl"};
  args.insert(args.end(), command.begin(), command.end());
  return run(FASTMM_CTL_EXE, args);
}

std::optional<StatusSnapshot> read_status(const std::string& path) {
  StatusReader r;
  std::string err;
  if (!r.open(path, &err)) return std::nullopt;
  StatusSnapshot s;
  if (!r.read(s)) return std::nullopt;
  return s;
}

const StatusPosition* find_position(const StatusSnapshot& s, std::string_view symbol) {
  for (std::uint32_t i = 0; i < s.gateway.position_count; ++i) {
    const StatusPosition& p = s.gateway.positions[i];
    if (std::string_view(p.symbol) == symbol) return &p;
  }
  return nullptr;
}

std::int64_t qty_of(const StatusSnapshot& s, std::string_view symbol) {
  const StatusPosition* p = find_position(s, symbol);
  return p == nullptr ? -1 : p->qty_raw;
}

// A port nothing listens on right now.
std::uint16_t free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0);
  socklen_t len = sizeof a;
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
  ::close(fd);
  return ntohs(a.sin_port);
}

// GET /metrics from 127.0.0.1:port: the body, or empty when nothing answered.
std::string scrape(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string reply;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
    const std::string req = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static_cast<void>(::send(fd, req.data(), req.size(), MSG_NOSIGNAL));
    char buf[4096];
    for (ssize_t n; (n = ::recv(fd, buf, sizeof buf, 0)) > 0;)
      reply.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  const std::size_t body = reply.find("\r\n\r\n");
  return body == std::string::npos ? std::string() : reply.substr(body + 4);
}

// The value of the sample `series` (name and labels) in a Prometheus text, if present.
std::optional<double> sample(const std::string& text, const std::string& series) {
  const std::string key = "\n" + series + " ";
  const std::size_t at = text.find(key);
  if (at == std::string::npos) return std::nullopt;
  return std::strtod(text.c_str() + at + key.size(), nullptr);
}

bool log_has(const std::string& path, std::string_view what) {
  return fastmm::test::read_file(path).find(what) != std::string::npos;
}

// Fills one resting order of `epoch` at the simulator (none resting: nothing).
void fill_one(ServerFixture& fx, std::uint16_t epoch) {
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == epoch) {
      static_cast<void>(fx.server.fill_open_order(id));
      return;
    }
  }
}

// The pulls and resumes a strategy's engine consumed, read back from its journals: "pull *",
// "resume ETHUSDT". (A strategy also pulls the other strategies' instruments when it starts.)
std::multiset<std::string> journal_controls(const SessionFiles& f) {
  std::multiset<std::string> out;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) != 0 || h->type != EventType::Control) return;
      const ControlCommand cmd = msg_cast<ControlMsg>(h).command;
      if (cmd != ControlCommand::PullQuotes && cmd != ControlCommand::ResumeQuotes) return;
      std::string scope = "*";
      for (const Instrument& i : r.instruments()) {
        if (i.id == h->instrument) scope = std::string(i.symbol.view());
      }
      out.insert((cmd == ControlCommand::PullQuotes ? "pull " : "resume ") + scope);
    });
  }
  return out;
}

}  // namespace

TEST_CASE(
    "gateway ops: the status segment shows both attachments with their instruments, the venue "
    "live and the account's positions, and fastmm-top prints and exports it") {
  ServerFixture fx(two_markets(kEthUsdt));
  const Configs c = write_configs(fx, "gw-ops", {}, kEthUsdt);
  // The default path: /dev/shm/fastmm-<name>.gw.status, fastmm-top --gateway <name>.
  const std::string gw_name = "gw-ops-gw";
  const std::string status = default_gateway_status_path(gw_name);
  remove_all_of({status});
  const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  // Each holds a position: one of its quotes filled.
  fill_one(fx, ea);
  fill_one(fx, eb);
  REQUIRE(wait_until([&] { return fills(fx, 0) >= 1 && fills(fx, 1) >= 1; }, 10000));

  // Both attachments, what each trades, the venue's channels.
  StatusSnapshot s;
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const auto r = read_status(status);
                        if (!r) return false;
                        s = *r;
                        // The connector publishes its channels on its once-a-second timer, and the
                        // strategies can be quoting before that.
                        return s.gateway.attachment_count == 2 && s.venue_count == 1 &&
                               s.venues[0].md == 2;
                      },
                      10000),
                  "no status with two attachments and the venue live at "
                      << status << ": " << fastmm::test::read_file(g.log));
  CHECK(s.kind == StatusKind::Gateway);
  CHECK(s.state == StatusRunState::Running);
  CHECK(std::string_view(s.engine_name) == gw_name);
  CHECK(s.pid == static_cast<std::uint32_t>(g.pid));
  REQUIRE(s.venue_count == 1);
  CHECK(std::string_view(s.venues[0].name) == "sim");
  CHECK(s.venues[0].md == 2);  // live
  CHECK(s.venues[0].user == 2);
  CHECK(s.venues[0].order == 2);
  std::set<std::string> engines;
  std::set<std::uint16_t> epochs;
  std::set<std::uint32_t> pids;
  for (std::uint32_t i = 0; i < s.gateway.attachment_count; ++i) {
    engines.insert(s.gateway.attachments[i].engine);
    epochs.insert(s.gateway.attachments[i].epoch);
    pids.insert(s.gateway.attachments[i].pid);
  }
  CHECK(engines == std::set<std::string>{c.a_name, c.b_name});
  CHECK(epochs == std::set<std::uint16_t>{ea, eb});
  CHECK(pids ==
        std::set<std::uint32_t>{static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)});
  REQUIRE(find_position(s, "BTCUSDT") != nullptr);
  REQUIRE(find_position(s, "ETHUSDT") != nullptr);
  CHECK(find_position(s, "BTCUSDT")->owner_epoch == ea);
  CHECK(find_position(s, "ETHUSDT")->owner_epoch == eb);

  // The account's positions are the simulator's, and not flat (the market's flow can flatten
  // them: then a quote of a's is filled again). The strategies trade on, so wait for a snapshot
  // taken between two fills.
  sim::server::SimServerStats ss;
  std::int64_t last_fill_ns = 0;
  const bool agree = wait_until(
      [&] {
        ss = fx.server.stats();
        if (position(ss, 0).is_zero() && position(ss, 1).is_zero()) {
          if (steady_now().ns - last_fill_ns > 1'000'000'000) {
            fill_one(fx, ea);
            last_fill_ns = steady_now().ns;
          }
          return false;
        }
        const auto r = read_status(status);
        if (!r) return false;
        s = *r;
        return qty_of(s, "BTCUSDT") == position(ss, 0).raw &&
               qty_of(s, "ETHUSDT") == position(ss, 1).raw;
      },
      30000);
  INFO("BTCUSDT: venue " << position(ss, 0).raw << ", status " << qty_of(s, "BTCUSDT")
                         << "; ETHUSDT: venue " << position(ss, 1).raw << ", status "
                         << qty_of(s, "ETHUSDT"));
  CHECK(agree);
  CHECK(s.fees_raw > 0);

  // fastmm-top reads the same segment: one frame, one JSON object.
  const Output frame = run(FASTMM_TOP_EXE, {"--gateway", gw_name, "--once", "--no-color"});
  INFO(frame.out);
  CHECK(frame.rc == 0);
  CHECK(frame.out.find("gateway=" + gw_name) != std::string::npos);
  CHECK(frame.out.find(c.a_name) != std::string::npos);
  CHECK(frame.out.find(c.b_name) != std::string::npos);
  CHECK(frame.out.find("sim:BTCUSDT") != std::string::npos);
  CHECK(frame.out.find("sim:ETHUSDT") != std::string::npos);
  const Output json = run(FASTMM_TOP_EXE, {"--gateway", gw_name, "--json"});
  INFO(json.out);
  CHECK(json.rc == 0);
  CHECK(json.out.starts_with(R"({"kind": "gateway",)"));
  CHECK(json.out.find(R"("engine": ")" + c.a_name + "\"") != std::string::npos);
  CHECK(json.out.find(R"("instruments": [{"venue": "sim", "symbol": "ETHUSDT"}])") !=
        std::string::npos);

  // fastmm-top --metrics: the account's PnL and positions for Prometheus.
  const std::uint16_t port = free_port();
  const pid_t top = spawn_process(
      FASTMM_TOP_EXE, {"--gateway", gw_name, "--metrics", "127.0.0.1:" + std::to_string(port)});
  std::string text;
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        text = scrape(port);
                        return text.find("fastmm_up 1\n") != std::string::npos;
                      },
                      10000),
                  "no metrics on port " << port << ": " << text);
  INFO(text);
  CHECK(sample(text, "fastmm_account_net_pnl").has_value());
  CHECK(sample(text, "fastmm_account_realized_pnl").has_value());
  CHECK(sample(text, "fastmm_account_fees").value_or(0) > 0);
  CHECK(sample(text, "fastmm_gateway_attachments") == 2.0);
  CHECK(sample(text,
               "fastmm_gateway_attachment_info{epoch=\"" + std::to_string(ea) + "\",engine=\"" +
                   c.a_name + "\",pid=\"" + std::to_string(a) + "\",attachment=\"1\"}") == 1.0);
  CHECK(sample(text, "fastmm_venue_channel_state{venue=\"sim\",channel=\"md\"}") == 2.0);
  const std::string btc = "fastmm_account_position{venue=\"sim\",instrument=\"BTCUSDT\"}";
  const std::string eth = "fastmm_account_position{venue=\"sim\",instrument=\"ETHUSDT\"}";
  const bool scraped = wait_until(
      [&] {
        text = scrape(port);
        ss = fx.server.stats();
        return sample(text, btc) == position(ss, 0).to_double() &&
               sample(text, eth) == position(ss, 1).to_double();
      },
      20000);
  CHECK_MESSAGE(scraped,
                "venue " << position(ss, 0).to_double() << " / " << position(ss, 1).to_double());
  REQUIRE(::kill(top, SIGINT) == 0);
  CHECK(reap(top) == 0);

  stop_strategy(a);
  stop_strategy(b);
  stop_gateway(g);
  // The last snapshot: stopped, nobody attached, the positions still the account's.
  const auto last = read_status(status);
  REQUIRE(last.has_value());
  CHECK(last->state == StatusRunState::Stopped);
  CHECK(last->gateway.attachment_count == 0);
  CHECK(qty_of(*last, "BTCUSDT") == position(fx.server.stats(), 0).raw);
  remove_all_of({status});
}

TEST_CASE(
    "gateway ops: pull and resume reach the strategies in their scope, kill latches the account, "
    "clear-kill lets a new strategy trade without a restart") {
  ServerFixture fx(two_markets(kEthUsdt));
  // max_loss far out of reach: the account's kill file is kept, so the latch is on disk too.
  const Configs c = write_configs(fx, "gw-ctl", "\n[gateway]\nmax_loss = \"1000\"\n", kEthUsdt);
  // b stays attached after a kill (on_kill = "stay"): clear-kill waits for it to go.
  rewrite(c.b.config,
          [](std::string& t) { replace_first(t, R"(on_kill = "exit")", R"(on_kill = "stay")"); });
  const std::string status = tmp_path("gw-ctl.gw.status");
  remove_all_of({status});
  const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s", "--status", status});
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});

  const Output list = ctl(g, {"attachments"});
  INFO(list.out);
  CHECK(list.rc == 0);
  CHECK(list.out.find("engine=" + c.a_name) != std::string::npos);
  CHECK(list.out.find("instruments=sim:ETHUSDT") != std::string::npos);
  CHECK(ctl(g, {"clear-kill"}).rc == 1);  // nothing to clear
  CHECK(ctl(g, {"bogus"}).rc == 1);

  // pull: every strategy stops quoting, and stays stopped.
  const Output pull = ctl(g, {"pull"});
  INFO(pull.out);
  CHECK(pull.rc == 0);
  CHECK(pull.out.find("sent to 2 strategies") != std::string::npos);
  CHECK_MESSAGE(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000),
                "orders still open after pull: " << fx.server.stats().open_orders);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(fx.server.stats().open_orders == 0);
  // resume: both quote again.
  CHECK(ctl(g, {"resume"}).rc == 0);
  CHECK(wait_until([&] { return open_of(fx, ea) > 0 && open_of(fx, eb) > 0; }, 10000));
  // A scope: only the owner of ETHUSDT pulls; a quotes on.
  const Output scoped = ctl(g, {"pull", "--instrument", "ETHUSDT"});
  INFO(scoped.out);
  CHECK(scoped.out.find("sent to 1 strategies") != std::string::npos);
  CHECK(wait_until([&] { return open_of(fx, eb) == 0; }, 5000));
  std::this_thread::sleep_for(std::chrono::seconds(1));
  CHECK(open_of(fx, ea) > 0);
  CHECK(open_of(fx, eb) == 0);
  CHECK(ctl(g, {"resume", "--instrument", "ETHUSDT"}).rc == 0);
  CHECK(wait_until([&] { return open_of(fx, eb) > 0; }, 10000));

  // kill: the account's kill switch, as a max_loss trip.
  const Output kill = ctl(g, {"kill"});
  INFO(kill.out);
  CHECK(kill.rc == 0);
  CHECK(reap(a) == live::kExitKilled);
  CHECK(log_has(c.a.config + ".log", "GatewayOperator"));
  // b stays up, its venue killed for the gateway's reason (its own status file).
  CHECK(wait_until(
      [&] {
        const auto r = read_status(c.b.status);
        return r && r->venues[0].killed == 1 &&
               r->venues[0].kill_reason == static_cast<std::uint8_t>(KillReason::GatewayOperator);
      },
      5000));
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  CHECK(fx.server.stats().cancel_all_requests >= 1);
  StatusSnapshot s;
  CHECK(wait_until(
      [&] {
        const auto r = read_status(status);
        if (!r) return false;
        s = *r;
        return s.gateway.kill_active == 1;
      },
      5000));
  CHECK(s.kill_reason == static_cast<std::uint8_t>(KillReason::GatewayOperator));
  CHECK(s.kill_latched == 1);
  const auto st = KillStateStore::load(c.gw.kill);
  REQUIRE(st.has_value());
  CHECK(st->latched);
  CHECK(st->reason == KillReason::GatewayOperator);
  std::string err;
  live::GatewayAttachRequest req;
  req.engine = "late";
  req.instruments = {{"sim", "BTCUSDT"}};
  CHECK(live::GatewayClient::attach(g.socket, req, &err) == nullptr);
  CHECK_MESSAGE(err.find("clear-kill") != std::string::npos, err);

  // clear-kill: refused while b, killed by the trip, is attached; then the account is clear and a
  // new strategy attaches and trades, the gateway never restarted.
  const Output early = ctl(g, {"clear-kill"});
  INFO(early.out);
  CHECK(early.rc == 1);
  CHECK(early.out.find(c.b_name) != std::string::npos);
  REQUIRE(::kill(b, SIGTERM) == 0);
  CHECK(reap(b) >= 0);
  CHECK(wait_until(
      [&] { return ctl(g, {"attachments"}).out.find("no strategy") != std::string::npos; }, 5000));
  const Output clear = ctl(g, {"clear-kill"});
  INFO(clear.out);
  CHECK(clear.rc == 0);
  const auto cleared = KillStateStore::load(c.gw.kill);
  REQUIRE(cleared.has_value());
  CHECK(!cleared->latched);
  CHECK(cleared->carry().is_zero());
  CHECK(wait_until(
      [&] {
        const auto r = read_status(status);
        return r && r->gateway.kill_active == 0 && r->kill_latched == 0;
      },
      5000));
  const std::uint64_t before = fills(fx, 0);
  const pid_t a2 = spawn_strategy(c.a, g);
  const std::uint16_t ea2 = wait_resting(fx, c.a, {});
  fill_one(fx, ea2);
  CHECK_MESSAGE(wait_until([&] { return fills(fx, 0) > before; }, 10000),
                "no trade after clear-kill: " << fastmm::test::read_file(c.a.config + ".log"));
  stop_strategy(a2);
  stop_gateway(g);

  // The pulls and resumes are the engine's own control messages, in each strategy's journal.
  const std::multiset<std::string> ca = journal_controls(c.a);
  const std::multiset<std::string> cb = journal_controls(c.b);
  CHECK(ca.count("pull *") == 1);
  CHECK(ca.count("resume *") == 1);
  CHECK(ca.count("pull BTCUSDT") == 0);
  CHECK(cb.count("pull *") == 1);
  CHECK(cb.count("resume *") == 1);
  CHECK(cb.count("pull ETHUSDT") == 1);
  CHECK(cb.count("resume ETHUSDT") == 1);
  remove_all_of({status});
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE && FASTMM_TOP_EXE && FASTMM_CTL_EXE
