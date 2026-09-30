// A live session against the simulator receives a venue's mark for its instrument, which values
// the position a fill left ([accounting] mark = "venue") and trips [risk] max_loss; the session
// replays from its journal alone to the identical outbound stream: the PerpState events are
// journaled like the market data they are, and the replay values and trips as the session did.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"

#include <chrono>
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

// A mark as a connector emits it, onto the engine's control ring (fastmm-live's connectors put it
// on the market-data ring; the engine and its journal treat both alike).
void mark(LiveEngine& live, const char* price) {
  PerpStateMsg m{};
  init_header(m, EventType::PerpState, InstrumentId{0}, VenueId{0});
  m.mark_price = Price::from_decimal(price).value_or(Price{});
  m.fields = PerpStateMsg::kMark;
  m.hdr.exch_ts = wall_now();
  m.hdr.recv_ts = wall_now();
  REQUIRE(live.control_ring().try_push(&m, m.hdr.len));
}

}  // namespace

TEST_CASE("sim_exchange replay: a session valued at the venue mark replays to the same kill") {
  const std::string path = fresh_journal("perp_state.fmj");
  sim::server::SimServerConfig sc = test_server_config();
  sc.generator.market_rate_per_s = 0.0;  // only the test fills an order
  ServerFixture fx(sc);
  Config cfg = sim_local_config(fx, false);
  cfg.risk.max_loss = "5";
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = 14;
  LiveEngine live(cfg, opts);
  live.start();
  REQUIRE(wait_until([&] { return fx.server.open_client_order_ids().size() >= 2; }, 30000));
  // One quote fills: a position, valued at the book's mid, well inside max_loss.
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
  REQUIRE(wait_until([&] { return live.live_stats().stats.fills >= 1; }, 10000));
  CHECK(live.live_stats().kill_flags == 0);
  // The venue marks it half its price away, against it whichever side filled: one of the two trips
  // max_loss, and the kill switch cancels the quotes.
  mark(live, "30000");
  mark(live, "90000");
  REQUIRE(wait_until([&] { return (live.live_stats().kill_flags & 1U) != 0; }, 10000));
  CHECK(live.live_stats().kill_reason == KillReason::MaxLoss);
  REQUIRE(wait_until([&] { return fx.server.open_client_order_ids().empty(); }, 10000));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  live.stop();
  CHECK(live.engine().perps().stats().reports == 2);

  std::size_t perp_records = 0;
  {
    JournalReader reader;
    REQUIRE(reader.open(path));
    reader.for_each([&](const EventHeader* h) {
      if (h->type == EventType::PerpState) ++perp_records;
    });
  }
  CHECK(perp_records == 2);

  const bt::ReplayResult r = bt::replay_journal(path);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}
