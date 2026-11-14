// [[risk.hazard]] in a backtest: BasicMM on a busy synthetic market, the instrument a group of its
// own with a threshold under one fill (a cooldown: every maker fill of a quote pulls its side). No
// order on a side goes out during the hold after a fill on that side, and the journal replays to
// the same outbound stream. The same group in log mode sends what a session without it sends.
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

enum class Mode { Off, Log, Pull };

Run run(Mode mode, const char* name) {
  Run r;
  r.cfg = synthetic_config(33, seconds(20));
  r.cfg.strategy = "basic_mm";
  r.cfg.transport.fill_model = sim::FillModel::L2Queue;
  if (mode != Mode::Off) {
    HazardGroupConfig g;
    g.name = "one";
    g.members = {InstrumentId{0}};
    g.threshold = 1e-6;
    g.half_life = milliseconds(20);
    g.hold = milliseconds(200);
    g.prints = false;  // the hold runs from the fill report, which the checks below time by
    g.log_only = mode == Mode::Log;
    r.cfg.engine.hazard.groups.push_back(g);
  }
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  r.cfg.journal_out = p.string();
  r.res = run_backtest(r.cfg, "basic_mm");
  r.out = traffic(r.cfg.journal_out);
  return r;
}

std::size_t inside_hold(const Traffic& t) {
  std::size_t n = 0;
  for (const Stamped& o : t.news) {
    for (const Stamped& f : t.fills) {
      if (f.side == o.side && f.t < o.t && o.t < f.t + milliseconds(200)) ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("backtest.hazard: a pulled side sends nothing until the hold ends, and replays") {
  const Run pull = run(Mode::Pull, "hazard_pull.fmj");
  const Run log = run(Mode::Log, "hazard_log.fmj");
  const Run off = run(Mode::Off, "hazard_off.fmj");
  MESSAGE("fills pull=" << pull.out.fills.size() << " off=" << off.out.fills.size()
                        << ", orders pull=" << pull.out.news.size()
                        << " off=" << off.out.news.size());
  REQUIRE(off.out.fills.size() >= 10);
  REQUIRE(pull.out.fills.size() >= 3);
  CHECK(pull.out.fills.size() < off.out.fills.size());
  CHECK(inside_hold(pull.out) == 0);
  // Without the group BasicMM requotes a filled side at once.
  CHECK(inside_hold(off.out) > 0);
  // Log mode changes nothing that is sent.
  CHECK(log.res.outbound_sha256 == off.res.outbound_sha256);
  CHECK(pull.res.outbound_sha256 != off.res.outbound_sha256);

  for (const Run* r : {&pull, &log, &off}) {
    const ReplayResult rp = replay_journal(r->cfg.journal_out, r->cfg);
    CHECK(rp.first_mismatch == -1);
    CHECK(rp.outbound_sha256 == r->res.outbound_sha256);
    CHECK(rp.ok());
  }
}
