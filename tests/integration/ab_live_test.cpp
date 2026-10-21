// [ab] in fastmm-live: a time test against the simulated exchange changes arm at each block,
// answers `ab` on the control socket, refuses a `param` naming the test's parameters, ends with
// `ab promote`, and its journal replays exactly and reads out with fastmm-data ab.
#include "integration_util.hpp"

#include "fastmm/backtest/ab_report.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/store/reader.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

std::string ask(const std::string& path, std::string_view request) {
  std::string reply;
  if (live::control_request(path, request, 2000, reply) != live::ControlReply::Answered) return {};
  return reply;
}

}  // namespace

TEST_CASE("ab live: a time test changes arm each block, answers fastmm-ctl ab and replays") {
  static const bool registered = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(registered);
  ServerFixture fx;
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = "ab-live-test";
  cfg.engine.epoch_file = fresh("ab-live-test.epoch");
  cfg.engine.kill_file = fresh("ab-live-test.kill");
  const std::filesystem::path dir = fastmm::test::tmp_dir() / "ab-live-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  cfg.engine.journal_dir = dir.string();
  cfg.ab.name = "spread-live";
  cfg.ab.assignment = "time";
  cfg.ab.block_s = 1;
  cfg.ab.arms = {{"control", {}}, {"wide", {{"half_spread_bps", "3"}}}};
  live::LiveOptions o;
  o.duration_ns = std::chrono::nanoseconds(std::chrono::seconds(6)).count();
  o.journal_path = fresh("ab-live-test.fmj");
  o.status_path = fresh("ab-live-test.status");
  o.control_path = fresh("ab-live-test.ctl");
  o.program = "ab-live-test";

  std::map<std::string, std::string> replies;
  std::thread operator_thread([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline && ask(o.control_path, "ab").empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    replies["status"] = ask(o.control_path, "ab");
    replies["param"] = ask(o.control_path, "param half_spread_bps=4");
    replies["other"] = ask(o.control_path, "param quote_qty=0.002");
    replies["bad"] = ask(o.control_path, "ab promote nope");
    replies["promote"] = ask(o.control_path, "ab promote wide");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    replies["after"] = ask(o.control_path, "ab");
    replies["again"] = ask(o.control_path, "ab stop");
  });
  const int rc = live::run_live(cfg, o);
  operator_thread.join();
  REQUIRE(rc == live::kExitOk);

  CHECK(replies["status"].starts_with(
      "ok spread-live: running, assignment time, 2 arms, seed 1, block 1 s\n"));
  CHECK(replies["status"].find("arm        1 wide: half_spread_bps=3") != std::string::npos);
  CHECK(replies["param"].starts_with(
      "error parameter 'half_spread_bps' belongs to the [ab] test spread-live"));
  CHECK(replies["other"].starts_with("ok param queued"));
  CHECK(replies["bad"] == "error no arm named 'nope'\n");
  CHECK(replies["promote"] == "ok ab promote wide queued\n");
  CHECK(replies["after"].find("stopped    every instrument on arm wide's parameters (promoted)") !=
        std::string::npos);
  CHECK(replies["again"] == "error the [ab] test spread-live has ended\n");

  // The journal: an update per block until the promote, then one out of the test.
  JournalReader reader;
  REQUIRE(reader.open(o.journal_path));
  std::vector<std::uint8_t> arms;
  std::map<std::uint8_t, std::size_t> orders;
  reader.for_each([&](const EventHeader* h) {
    if (h->type == EventType::ParamUpdate &&
        msg_cast<ParamUpdateMsg>(h).origin == ParamUpdateMsg::Origin::Experiment)
      arms.push_back(msg_cast<ParamUpdateMsg>(h).arm);
    if (h->type == EventType::OutNewOrder && (h->flags & EventHeader::kOutbound) != 0)
      ++orders[msg_cast<OutNewOrderMsg>(h).arm];
  });
  REQUIRE(arms.size() >= 3);
  CHECK(arms.back() == ParamUpdateMsg::kArmNone);
  for (std::size_t i = 0; i + 1 < arms.size(); ++i) CHECK((arms[i] == 1 || arms[i] == 2));
  CHECK(orders[1] + orders[2] > 0);

  const bt::ReplayResult rr = bt::replay_journal(o.journal_path);
  INFO("expected: " << rr.expected_message);
  INFO("actual:   " << rr.actual_message);
  CHECK(rr.ok());

  const std::vector<std::string> journals{o.journal_path};
  const bt::AbReport r = bt::ab_report(journals, {});
  CHECK(r.test == "spread-live");
  CHECK(r.arms[0].ns + r.arms[1].ns > 0);

  // The store names the arm of each order.
  store::register_builtin_backends();
  auto store_reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(store_reader != nullptr);
  store::BackendOptions bo;
  bo.config = &cfg.storage;
  bo.engine_name = cfg.engine.name;
  bo.default_dir = cfg.engine.journal_dir;
  bo.read_only = true;
  REQUIRE(store_reader->open(bo).has_value());
  const auto rows = store_reader->orders(store::QueryFilter{});
  REQUIRE(rows.has_value());
  const auto col = std::find(rows->columns.begin(), rows->columns.end(), "arm");
  REQUIRE(col != rows->columns.end());
  const auto k = static_cast<std::size_t>(col - rows->columns.begin());
  std::size_t named = 0;
  for (const auto& row : rows->rows) named += row[k] == "control" || row[k] == "wide" ? 1U : 0U;
  CHECK(named > 0);
  store_reader.reset();
  std::filesystem::remove(o.journal_path);
  std::filesystem::remove_all(dir);
}
