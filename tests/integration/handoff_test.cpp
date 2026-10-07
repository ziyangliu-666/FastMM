// Handing a running session over to a new process (docs/how-to/operations/hand-over-a-session.md):
// two real fastmm-live processes on the same engine name against the in-process simulator. Whatever
// the moment, at most one of them has orders at the venue, the venue accepts every order of the old
// one before the first of the new one, no client order id repeats, and the new one ends holding
// what the venue holds.
#include "process_util.hpp"

#include "fastmm/core/strong_id.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#ifdef FASTMM_LIVE_EXE

namespace {

// write_config's session with [engine] instance_lock = true.
SessionFiles locked_config(const ServerFixture& fx, const std::string& stem) {
  SessionFiles f = write_config(fx, stem, "exit", "1000");
  std::string text = fastmm::test::read_file(f.config);
  const std::string section = "[engine]\n";
  text.replace(text.find(section), section.size(), section + "instance_lock = true\n");
  std::ofstream(f.config, std::ios::trunc) << text;
  remove_all_of({f.epoch,
                 f.kill,
                 f.journal_dir,
                 f.config + ".a.log",
                 f.config + ".b.log",
                 f.config + ".c.log"});
  return f;
}

// The control socket both processes of `f` use. Short: sockaddr_un holds 107 bytes, and the build
// directory alone can be longer.
std::string ctl_of(const SessionFiles& f) {
  return (std::filesystem::temp_directory_path() /
          ("fastmm-" + std::to_string(::getpid()) + "-" +
           std::filesystem::path(f.config).stem().string() + ".ctl"))
      .string();
}

// fastmm-live on `f` with its own log; `extra` after the common arguments.
pid_t start(const SessionFiles& f, const std::string& log, const std::vector<std::string>& extra) {
  std::vector<std::string> args{"--config",
                                f.config,
                                "--duration",
                                "120s",
                                "--status",
                                f.status,
                                "--log",
                                log,
                                "--control",
                                ctl_of(f)};
  args.insert(args.end(), extra.begin(), extra.end());
  return spawn_process(FASTMM_LIVE_EXE, args);
}

std::uint16_t epoch_of(const std::string& client_order_id) {
  const auto id = decode_cl_ord_id(client_order_id);
  REQUIRE_MESSAGE(id.has_value(), "not a FastMM client order id: " << client_order_id);
  return static_cast<std::uint16_t>(id.value_or(ClientOrderId{}).value >> 32);
}

// Samples the venue's open orders until stopped: the epochs it saw open at the same moment.
struct OpenOrderWatch {
  const ServerFixture& fx;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint64_t> mixed{0};  // samples with orders of two epochs open
  std::thread thread;

  explicit OpenOrderWatch(const ServerFixture& f) : fx(f) {
    thread = std::thread([this] {
      while (!stop.load()) {
        std::set<std::uint16_t> epochs;
        for (const std::string& id : fx.server.open_client_order_ids()) {
          if (const auto cl = decode_cl_ord_id(id))
            epochs.insert(static_cast<std::uint16_t>(cl->value >> 32));
        }
        if (epochs.size() > 1) mixed.fetch_add(1);
        samples.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~OpenOrderWatch() { finish(); }
  OpenOrderWatch(const OpenOrderWatch&) = delete;
  OpenOrderWatch& operator=(const OpenOrderWatch&) = delete;
  void finish() {
    stop.store(true);
    if (thread.joinable()) thread.join();
  }
};

// The epochs of the accepted orders in acceptance order may only ever go up: no order of an older
// process after the first of a newer one.
void check_one_trader_at_a_time(const ServerFixture& fx, std::size_t processes) {
  const std::vector<std::string> accepted = fx.server.accepted_client_order_ids();
  REQUIRE_FALSE(accepted.empty());
  std::vector<std::uint16_t> epochs;
  for (const std::string& id : accepted) {
    const std::uint16_t e = epoch_of(id);
    if (epochs.empty() || epochs.back() != e) epochs.push_back(e);
  }
  INFO("epochs in acceptance order, runs collapsed: " << epochs.size());
  CHECK(epochs.size() == processes);
  CHECK(std::is_sorted(epochs.begin(), epochs.end()));
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
}

}  // namespace

TEST_CASE("handoff: --takeover replaces a running session, one process trading at a time") {
  ServerFixture fx;
  const SessionFiles f = locked_config(fx, "handoff-takeover");
  const std::string log_a = f.config + ".a.log";
  const std::string log_b = f.config + ".b.log";
  OpenOrderWatch watch(fx);

  const pid_t first = start(f, log_a, {});
  REQUIRE_MESSAGE(wait_until([&] { return !fx.server.stats().position.is_zero(); }, 60000),
                  "the first session never traded: " << fastmm::test::read_file(log_a));

  // A plain second start is refused before it touches anything.
  const std::uint64_t sessions_before = fx.server.stats().api_sessions_opened;
  const pid_t refused = start(f, f.config + ".refused.log", {});
  CHECK(reap(refused) == live::kExitLocked);
  CHECK(fx.server.stats().api_sessions_opened == sessions_before);

  // The new process: reference data, then the handoff, then the lock.
  const auto t0 = std::chrono::steady_clock::now();
  const pid_t second = start(f, log_b, {"--takeover"});
  CHECK(reap(first) == live::kExitOk);
  const auto t_old_exit = std::chrono::steady_clock::now();
  const std::size_t old_orders = fx.server.accepted_client_order_ids().size();
  REQUIRE_MESSAGE(
      wait_until([&] { return fx.server.accepted_client_order_ids().size() > old_orders; }, 30000),
      "the new process never quoted: " << fastmm::test::read_file(log_b));
  const auto t_new_order = std::chrono::steady_clock::now();
  const auto ms = [](auto d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };
  MESSAGE("takeover: old process exited " << ms(t_old_exit - t0) << " ms after the start, first "
                                          << "order of the new one " << ms(t_new_order - t_old_exit)
                                          << " ms after that");
  CHECK(fastmm::test::read_file(log_a).find("control socket: handoff") != std::string::npos);

  // Let the new one trade, then stop it the usual way.
  const std::uint64_t fills = fx.server.stats().fills;
  static_cast<void>(wait_until([&] { return fx.server.stats().fills > fills; }, 20000));
  REQUIRE(::kill(second, SIGTERM) == 0);
  CHECK(reap(second) == live::kExitOk);
  watch.finish();
  CHECK(fastmm::test::read_file(log_b).find("taken over") != std::string::npos);

  CHECK(watch.samples.load() > 100);
  CHECK(watch.mixed.load() == 0);
  check_one_trader_at_a_time(fx, 2);
  CHECK(fx.server.stats().open_orders == 0);
  const std::vector<std::string> twice = booked_twice(f, "handoff-takeover");
  CHECK(twice.empty());
  CHECK(store_position(f, "handoff-takeover") == fx.server.stats().position);
}

TEST_CASE("handoff: a standby waits and takes over when the holder hands over") {
  ServerFixture fx;
  const SessionFiles f = locked_config(fx, "handoff-standby");
  const std::string log_a = f.config + ".a.log";
  const std::string log_b = f.config + ".b.log";
  OpenOrderWatch watch(fx);

  const pid_t first = start(f, log_a, {});
  REQUIRE(wait_until([&] { return fx.server.stats().orders_accepted > 0; }, 60000));
  const pid_t second = start(f, log_b, {"--standby"});
  // Waiting is all it does: it stays up and the venue sees orders of the first process only.
  std::this_thread::sleep_for(std::chrono::seconds(3));
  REQUIRE(::waitpid(second, nullptr, WNOHANG) == 0);
  const std::uint16_t first_epoch = epoch_of(fx.server.accepted_client_order_ids().front());
  for (const std::string& id : fx.server.accepted_client_order_ids())
    CHECK(epoch_of(id) == first_epoch);

  // The operator's handoff, as fastmm-ctl --path <ctl> handoff sends it.
  const std::string ctl = ctl_of(f);
  std::string reply;
  REQUIRE(live::control_request(ctl, "handoff", 2000, reply) == live::ControlReply::Answered);
  CHECK(reply.starts_with("ok"));
  CHECK(reap(first) == live::kExitOk);
  const std::size_t old_orders = fx.server.accepted_client_order_ids().size();
  REQUIRE_MESSAGE(
      wait_until([&] { return fx.server.accepted_client_order_ids().size() > old_orders; }, 30000),
      "the standby never quoted: " << fastmm::test::read_file(log_b));
  // The new holder answers on the same socket.
  REQUIRE(live::control_request(ctl, "status", 2000, reply) == live::ControlReply::Answered);
  REQUIRE(live::control_request(ctl, "stop", 2000, reply) == live::ControlReply::Answered);
  CHECK(reap(second) == live::kExitOk);
  watch.finish();

  CHECK(watch.mixed.load() == 0);
  check_one_trader_at_a_time(fx, 2);
  CHECK(fx.server.stats().open_orders == 0);
  CHECK(booked_twice(f, "handoff-standby").empty());
  CHECK(store_position(f, "handoff-standby") == fx.server.stats().position);
}

TEST_CASE("handoff: a takeover the holder cannot accept leaves the holder trading") {
  ServerFixture fx;
  const SessionFiles f = locked_config(fx, "handoff-refused");
  const std::string log_a = f.config + ".a.log";

  // No control socket: nobody can ask it to hand over.
  const pid_t first = start(f, log_a, {"--no-control"});
  REQUIRE(wait_until([&] { return fx.server.stats().orders_accepted > 0; }, 60000));
  const pid_t second = start(f, f.config + ".b.log", {"--takeover"});
  CHECK(reap(second) == live::kExitLocked);

  // The holder never noticed: it keeps quoting under its own epoch.
  const std::size_t before = fx.server.accepted_client_order_ids().size();
  CHECK(wait_until([&] { return fx.server.accepted_client_order_ids().size() > before; }, 20000));
  REQUIRE(::kill(first, SIGTERM) == 0);
  CHECK(reap(first) == live::kExitOk);
  check_one_trader_at_a_time(fx, 1);
  CHECK(fx.server.stats().open_orders == 0);
}

// The pause a handoff costs, cold and warm. Cold: the session stops, a new process starts from
// nothing. Warm: a --standby has had its market data, books and strategy running all along, and
// its private channels and reconciliation are all that is left once the lock is free.
TEST_CASE("handoff: a warm standby reads market data before the handoff and quotes sooner") {
  ServerFixture fx;
  const SessionFiles f = locked_config(fx, "handoff-warm");
  OpenOrderWatch watch(fx);
  const auto ms = [](auto d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };
  // The epochs of the orders the venue has accepted so far.
  const auto epochs_accepted = [&] {
    std::set<std::uint16_t> out;
    for (const std::string& id : fx.server.accepted_client_order_ids()) out.insert(epoch_of(id));
    return out;
  };
  // Waits for the venue to accept an order of an epoch not in `old`, the new process's first;
  // that epoch.
  const auto new_epoch = [&](const std::set<std::uint16_t>& old, const std::string& log) {
    std::uint16_t epoch = 0;
    const auto arrived = [&] {
      for (const std::string& id : fx.server.accepted_client_order_ids()) {
        epoch = epoch_of(id);
        if (!old.contains(epoch)) return true;
      }
      return false;
    };
    REQUIRE_MESSAGE(wait_until(arrived, 30000),
                    "no order from the new process: " << fastmm::test::read_file(log));
    return epoch;
  };

  const pid_t a = start(f, f.config + ".a.log", {});
  REQUIRE(wait_until([&] { return !fx.server.accepted_client_order_ids().empty(); }, 60000));

  // Cold: stop, then start.
  REQUIRE(::kill(a, SIGTERM) == 0);
  CHECK(reap(a) == live::kExitOk);
  const auto a_exit = std::chrono::steady_clock::now();
  const pid_t b = start(f, f.config + ".b.log", {});
  const std::uint16_t b_epoch = new_epoch(epochs_accepted(), f.config + ".b.log");
  const auto cold = std::chrono::steady_clock::now() - a_exit;

  // Warm: a standby next to the running session, once that one's start-up queries are done.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const sim::server::SimServerStats running = fx.server.stats();
  const pid_t c = start(f, f.config + ".c.log", {"--standby"});
  REQUIRE_MESSAGE(
      wait_until([&] { return fx.server.stats().md_sessions > running.md_sessions; }, 30000),
      "the standby never connected to market data");
  std::this_thread::sleep_for(std::chrono::seconds(3));  // its books sync, its strategy runs
  const sim::server::SimServerStats warm_up = fx.server.stats();
  CHECK(warm_up.depth_snapshots > running.depth_snapshots);  // the standby's books
  // Nothing of the account: no WebSocket API session, no user stream, no cancel. The running
  // session reconciles on its own whenever its events ask for it, so the venue's query counts say
  // nothing of the standby; its log does, below.
  CHECK(warm_up.api_sessions_opened == running.api_sessions_opened);
  CHECK(warm_up.user_subscriptions == running.user_subscriptions);
  CHECK(warm_up.cancel_all_requests == running.cancel_all_requests);
  for (const std::string& id : fx.server.open_client_order_ids()) CHECK(epoch_of(id) == b_epoch);

  std::string reply;
  const std::set<std::uint16_t> old = epochs_accepted();
  const std::uint64_t snapshots = fx.server.stats().depth_snapshots;
  const auto handoff = std::chrono::steady_clock::now();
  REQUIRE(live::control_request(ctl_of(f), "handoff", 2000, reply) == live::ControlReply::Answered);
  REQUIRE(reply.starts_with("ok"));
  CHECK(new_epoch(old, f.config + ".c.log") > b_epoch);
  const auto warm = std::chrono::steady_clock::now() - handoff;
  // Its book was live before the handoff: no snapshot between the handoff and its first order.
  CHECK(fx.server.stats().depth_snapshots == snapshots);
  MESSAGE("first order of the new process: cold " << ms(cold) << " ms after the old one exited, "
                                                  << "warm " << ms(warm)
                                                  << " ms after the handoff");
  CHECK(reap(b) == live::kExitOk);

  REQUIRE(live::control_request(ctl_of(f), "stop", 2000, reply) == live::ControlReply::Answered);
  CHECK(reap(c) == live::kExitOk);
  watch.finish();
  const std::string c_log = fastmm::test::read_file(f.config + ".c.log");
  CHECK(c_log.find("private=deferred") != std::string::npos);
  // Until it took over, the standby read nothing of the account and opened none of it.
  const std::size_t took_over = c_log.find("took over after");
  REQUIRE(took_over != std::string::npos);
  const std::string_view warming = std::string_view(c_log).substr(0, took_over);
  CHECK(warming.find("reconciled") == std::string_view::npos);
  CHECK(warming.find("replayed") == std::string_view::npos);
  CHECK(warming.find("user=live") == std::string_view::npos);
  CHECK(warming.find("order=live") == std::string_view::npos);
  CHECK(watch.mixed.load() == 0);
  check_one_trader_at_a_time(fx, 3);
  CHECK(fx.server.stats().open_orders == 0);
  CHECK(booked_twice(f, "handoff-warm").empty());
  CHECK(store_position(f, "handoff-warm") == fx.server.stats().position);
}

#endif  // FASTMM_LIVE_EXE
