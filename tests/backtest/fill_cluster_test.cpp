// [risk] fill_cluster_names in a backtest: BasicMM on a busy synthetic market, the guard set to one
// instrument (a cooldown: every maker fill of a quote holds its side). No order on a side goes out
// during the hold after a fill on that side, the session trades less than without the guard, and
// its journal replays to the same outbound stream.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/journal.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

struct Stamped {
  Timestamp t;
  Side side;
};

struct Traffic {
  std::vector<Stamped> fills;  // maker fills of the session's orders, by receive time
  std::vector<Stamped> news;   // new orders, by engine time
};

Traffic traffic(const std::string& path) {
  JournalReader r;
  REQUIRE(r.open(path));
  Traffic o;
  r.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) != 0) {
      if (e->type == EventType::OutNewOrder)
        o.news.push_back({e->recv_ts, msg_cast<OutNewOrderMsg>(e).side});
    } else if (e->type == EventType::OrderFill) {
      const auto& f = msg_cast<OrderFillMsg>(e);
      if (f.liquidity == Liquidity::Maker) o.fills.push_back({e->recv_ts, f.side});
    }
  });
  return o;
}

struct Run {
  BacktestConfig cfg;
  BacktestResult res;
  Traffic out;
};

Run run(std::uint32_t names, const char* name) {
  Run r;
  r.cfg = synthetic_config(33, seconds(20));
  r.cfg.strategy = "basic_mm";
  r.cfg.transport.fill_model = sim::FillModel::L2Queue;
  r.cfg.engine.fill_cluster = FillClusterConfig{names, milliseconds(500), milliseconds(200)};
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  r.cfg.journal_out = p.string();
  r.res = run_backtest(r.cfg, "basic_mm");
  r.out = traffic(r.cfg.journal_out);
  return r;
}

}  // namespace

TEST_CASE("backtest.fill_cluster: a held side sends nothing until the hold ends, and replays") {
  const Run held = run(1, "fill_cluster_held.fmj");
  const Run open = run(0, "fill_cluster_open.fmj");
  MESSAGE("fills held=" << held.out.fills.size() << " open=" << open.out.fills.size()
                        << ", orders held=" << held.out.news.size()
                        << " open=" << open.out.news.size());
  REQUIRE(open.out.fills.size() >= 10);
  REQUIRE(held.out.fills.size() >= 3);
  CHECK(held.out.fills.size() < open.out.fills.size());
  // Each new order on a side comes at least the hold after the last fill on that side before it.
  std::size_t inside = 0;
  for (const Stamped& n : held.out.news) {
    for (const Stamped& f : held.out.fills) {
      if (f.side == n.side && f.t < n.t && n.t < f.t + milliseconds(200)) ++inside;
    }
  }
  CHECK(inside == 0);
  // Without the guard BasicMM requotes a filled side at once.
  std::size_t requoted = 0;
  for (const Stamped& n : open.out.news) {
    for (const Stamped& f : open.out.fills) {
      if (f.side == n.side && f.t < n.t && n.t < f.t + milliseconds(200)) ++requoted;
    }
  }
  CHECK(requoted > 0);

  for (const Run* r : {&held, &open}) {
    const ReplayResult rp = replay_journal(r->cfg.journal_out, r->cfg);
    CHECK(rp.first_mismatch == -1);
    CHECK(rp.outbound_sha256 == r->res.outbound_sha256);
    CHECK(rp.ok());
  }
}
