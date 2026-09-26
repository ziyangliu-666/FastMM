// A session that booked funding replays from its journal alone to the identical outbound stream:
// the payments are journaled like fills, and one that takes the session past max_loss trips the
// kill switch in the replay exactly where it did live.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

constexpr InstrumentId kFirst{0};

std::string fresh_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

// A payment as a connector emits it, onto the engine's control ring (fastmm-live's connectors put
// it on the order ring; the engine and its journal treat both alike).
void pay(LiveEngine& live, const char* amount, const char* id) {
  FundingMsg m{};
  init_header(m, EventType::Funding, kFirst, VenueId{0});
  m.amount = Notional::from_decimal(amount).value();
  m.asset.assign("USDT");
  m.funding_id.assign(id);
  m.hdr.exch_ts = wall_now();
  m.hdr.recv_ts = wall_now();
  REQUIRE(live.control_ring().try_push(&m, m.hdr.len));
}

}  // namespace

TEST_CASE("sim_exchange replay: a session with funding replays to the identical outbound stream") {
  const std::string path = fresh_journal("funding.fmj");
  ServerFixture fx;
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = 12;
  Config cfg = sim_local_config(fx, false);
  cfg.risk.max_loss = "1000";
  LiveEngine live(cfg, opts);
  live.start();
  REQUIRE(wait_until([&] { return fx.server.stats().fills >= 1; }, 30000));

  pay(live, "-1.5", "tran-1");
  pay(live, "-1.5", "tran-1");  // the stream and the venue's history both deliver it
  pay(live, "0.25", "tran-2");
  REQUIRE(wait_until(
      [&] {
        return live.live_stats().stats.funding_raw == Notional::from_decimal("-1.25").value().raw;
      },
      10000));
  CHECK(live.live_stats().kill_reason == KillReason::None);
  // A payment that takes the session past its loss budget: the kill switch pulls the quotes.
  pay(live, "-5000", "tran-3");
  REQUIRE(wait_until([&] { return live.live_stats().kill_reason == KillReason::MaxLoss; }, 10000));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  live.stop();

  CHECK(live.engine().funding_stats().payments == 3);
  CHECK(live.engine().funding_stats().duplicates == 1);
  CHECK(live.engine().positions().total_funding() == Notional::from_decimal("-5001.25").value());

  std::size_t funding_records = 0;
  {
    JournalReader reader;
    REQUIRE(reader.open(path));
    reader.for_each([&](const EventHeader* h) {
      if (h->type == EventType::Funding) ++funding_records;
    });
  }
  CHECK(funding_records == 4);  // every one consumed, the duplicate too

  const bt::ReplayResult r = bt::replay_journal(path);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}
