// A live session against the simulator reports the simulator's balances into the engine, and the
// session replays from its journal alone to the identical outbound stream: the balance events are
// journaled like fills, and the quotes BasicMM cut to a short balance are cut again in the replay.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

std::optional<LiveBalance> live_balance(const EngineLiveStats& s, std::string_view asset) {
  for (std::uint32_t i = 0; i < s.balance_count; ++i) {
    const LiveBalance& b = s.balances[i];
    if (b.known != 0 && std::string_view(b.asset, ::strnlen(b.asset, sizeof b.asset)) == asset)
      return b;
  }
  return std::nullopt;
}

// A report as a connector emits it, onto the engine's control ring (fastmm-live's connectors put
// it on the order ring; the engine and its journal treat both alike).
void report(LiveEngine& live, const char* asset, const char* free) {
  BalanceMsg m{};
  init_header(m, EventType::Balance, InstrumentId::invalid(), VenueId{0});
  m.asset.assign(asset);
  m.free = Notional::from_decimal(free).value();
  m.total = m.free;
  m.equity = m.free;
  m.hdr.exch_ts = wall_now();
  m.hdr.recv_ts = wall_now();
  REQUIRE(live.control_ring().try_push(&m, m.hdr.len));
}

}  // namespace

TEST_CASE("sim_exchange replay: a session with balances replays to the identical outbound stream") {
  const std::string path = fresh_journal("balances.fmj");
  sim::server::SimServerConfig sc = test_server_config();
  sc.balances = {{"BTC", Qty::from_int(2)}, {"USDT", Qty::from_int(100000)}};
  sc.generator.market_rate_per_s = 0.0;  // only the test fills an order
  ServerFixture fx(sc);
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = 13;
  LiveEngine live(sim_local_config(fx, false), opts);
  live.start();
  // The engine's view is the simulator's account: 2 BTC and 100000 USDT, some of it held by the
  // quotes resting there.
  REQUIRE(wait_until(
      [&] {
        const EngineLiveStats s = live.live_stats();
        const auto btc = live_balance(s, "BTC");
        const auto usdt = live_balance(s, "USDT");
        return btc && usdt && btc->total_raw == Qty::from_int(2).raw &&
               usdt->total_raw == Qty::from_int(100000).raw && usdt->locked_raw > 0 &&
               btc->locked_raw > 0;
      },
      30000));
  // One of the quotes fills: the estimate moves with it, and the venue's update replaces it.
  Qty filled{};
  REQUIRE(wait_until(
      [&] {
        for (const std::string& id : fx.server.open_client_order_ids()) {
          filled = fx.server.fill_open_order(id);
          if (filled.is_positive()) return true;
        }
        return false;
      },
      10000));
  REQUIRE(wait_until(
      [&] {
        const auto btc = live_balance(live.live_stats(), "BTC");
        return btc && btc->total_raw != Qty::from_int(2).raw;
      },
      10000));
  // A report of 10 USDT: the bid (0.001 at about 60000) no longer fits and is cancelled. (The
  // venue's next update says otherwise and it comes back: the replay has to follow both.)
  const std::uint64_t cancels = fx.server.stats().cancels;
  report(live, "USDT", "10");
  REQUIRE(wait_until([&] { return fx.server.stats().cancels > cancels; }, 10000));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  live.stop();
  CHECK(live.engine().balances().stats().reports >= 3);
  CHECK(fx.server.stats().orders_rejected == 0);

  std::size_t balance_records = 0;
  {
    JournalReader reader;
    REQUIRE(reader.open(path));
    reader.for_each([&](const EventHeader* h) {
      if (h->type == EventType::Balance) ++balance_records;
    });
  }
  CHECK(balance_records >= 3);

  const bt::ReplayResult r = bt::replay_journal(path);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}
