// [risk] max_feed_lag_ms on recorded data with [backtest] md_arrival = "recorded": a synthetic
// market whose messages arrive 300 us after their venue time, except for one second in which they
// arrive 30 ms late. With the gate the quotes are pulled when the burst starts, no order goes out
// while it lasts and quoting resumes 100 ms after it; without it BasicMM keeps requoting. The
// session's journal replays to the same outbound stream.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/journal.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// SyntheticSource with a receive time after each venue time: 300 us, 30 ms inside the burst.
class LaggedSource final : public MdSource {
 public:
  LaggedSource(const SyntheticSourceConfig& c, Timestamp burst_from, Timestamp burst_to)
      : src_(c), from_(burst_from), to_(burst_to) {}
  const EventHeader* next() override {
    const EventHeader* h = src_.next();
    if (h == nullptr) return nullptr;
    buf_.resize(h->len);
    std::memcpy(buf_.data(), h, h->len);
    auto* e = reinterpret_cast<EventHeader*>(buf_.data());
    const bool late = e->exch_ts >= from_ && e->exch_ts < to_;
    e->recv_ts = e->exch_ts + (late ? milliseconds(30) : microseconds(300));
    return e;
  }
  void reset() override { src_.reset(); }
  [[nodiscard]] Timestamp start_ts() const override { return src_.start_ts(); }

 private:
  SyntheticSource src_;
  Timestamp from_;
  Timestamp to_;
  std::vector<std::byte> buf_;
};

struct Outbound {
  std::vector<Timestamp> news;
  std::vector<Timestamp> cancels;
};

Outbound outbound(const std::string& path) {
  JournalReader r;
  REQUIRE(r.open(path));
  Outbound o;
  r.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) == 0) return;
    if (e->type == EventType::OutNewOrder) o.news.push_back(e->recv_ts);  // engine time
    if (e->type == EventType::OutCancel) o.cancels.push_back(e->recv_ts);
  });
  return o;
}

std::size_t in(const std::vector<Timestamp>& ts, Timestamp a, Timestamp b) {
  std::size_t n = 0;
  for (const Timestamp t : ts) n += t >= a && t < b ? 1U : 0U;
  return n;
}

struct Run {
  BacktestConfig cfg;
  BacktestResult res;
  Outbound out;
};

Run run(std::uint32_t max_feed_lag_ms, const char* name, Timestamp& start) {
  Run r;
  r.cfg = synthetic_config(21, seconds(10));
  r.cfg.strategy = "basic_mm";
  r.cfg.transport.fill_model = sim::FillModel::L2Queue;
  r.cfg.transport.md_recorded_arrival = true;
  r.cfg.engine.risk.max_feed_lag_ms = max_feed_lag_ms;
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  r.cfg.journal_out = p.string();
  const SyntheticSourceConfig sc = synthetic_source_config(r.cfg);
  start = sc.start;
  LaggedSource src(sc, sc.start + seconds(4), sc.start + seconds(5));
  r.res = run_backtest(r.cfg, "basic_mm", &src);
  r.out = outbound(r.cfg.journal_out);
  return r;
}

}  // namespace

TEST_CASE("backtest.feed_lag: the gate pulls quotes during a lag burst and resumes after it") {
  Timestamp t0{};
  const Run gated = run(5, "feed_lag_gated.fmj", t0);
  const Run open = run(0, "feed_lag_open.fmj", t0);
  // The first late message is consumed 30 ms after the burst starts; the last one 30 ms after it
  // ends, and the gate holds for 100 ms more.
  const Timestamp engaged = t0 + seconds(4) + milliseconds(30);
  const Timestamp released = t0 + seconds(5) + milliseconds(130);
  MESSAGE("orders gated=" << gated.out.news.size() << " open=" << open.out.news.size());
  REQUIRE(in(open.out.news, engaged, released) >= 2);  // BasicMM requotes without the gate
  CHECK(in(gated.out.news, engaged + microseconds(1), released) == 0);
  CHECK(in(gated.out.cancels, engaged, engaged + milliseconds(1)) >= 1);  // the pull
  CHECK(in(gated.out.news, released, released + seconds(1)) >= 1);        // quoting again
  // Before the burst both runs sent the same orders.
  CHECK(in(gated.out.news, t0, engaged) == in(open.out.news, t0, engaged));
  CHECK(gated.res.engine.risk_rejects_by_reason[RejectReason::FeedLag] == 0);  // quotes: ignored

  for (const Run* r : {&gated, &open}) {
    const ReplayResult rp = replay_journal(r->cfg.journal_out, r->cfg);
    CHECK(rp.first_mismatch == -1);
    CHECK(rp.outbound_sha256 == r->res.outbound_sha256);
    CHECK(rp.ok());
  }
}

TEST_CASE("backtest.feed_lag: md_arrival = venue ignores the recorded receive times") {
  Timestamp t0{};
  BacktestConfig cfg = synthetic_config(21, seconds(10));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.engine.risk.max_feed_lag_ms = 5;
  const SyntheticSourceConfig sc = synthetic_source_config(cfg);
  t0 = sc.start;
  LaggedSource src(sc, t0 + seconds(4), t0 + seconds(5));
  const BacktestResult venue = run_backtest(cfg, "basic_mm", &src);
  cfg.engine.risk.max_feed_lag_ms = 0;
  src.reset();
  const BacktestResult off = run_backtest(cfg, "basic_mm", &src);
  CHECK(venue.outbound_sha256 == off.outbound_sha256);
}
