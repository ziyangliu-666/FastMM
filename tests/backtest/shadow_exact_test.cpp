// A shadow point against the same point run as a backtest and as a sweep of the finished journal,
// field by field (bt::result_difference), and a journal followed as it grows against the same
// journal read finished. The sessions here are backtests recorded to a journal, which is then
// written out again record by record as a live session's would be: in parts, with parameter
// updates, operator commands and flows of money among its records, and marked live so its market
// data is stripped of its own orders.
#include "backtest_test_util.hpp"
#include "random_market.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/follow_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/multiverse.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/shadow.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/registry.hpp"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

std::string tmp_journal(const std::string& name) {
  const std::string path = (test::tmp_dir() / name).string();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  for (std::uint64_t k = 1; k < 200; ++k)
    std::filesystem::remove(JournalFileWriter::part_path(path, k), ec);
  return path;
}

// Two instruments on one venue or on two; with `accounts`, on a pool of two accounts with the
// balances a venue would report.
std::string session_toml(bool second_venue, bool accounts = false) {
  std::string s = R"(
[engine]
name = "mm"
rng_seed = 3
min_requote_ticks = 1
min_requote_interval_ms = 0
min_qty_bps = 8000
post_only = true

[venues.a]
kind = "sim"
supports_replace = true
[venues.a.fees]
maker_bps = -0.5
taker_bps = 3.0
)";
  if (accounts) {
    s += R"(
[venues.m]
kind = "sim"
pool_of = "a"
)";
  }
  if (second_venue) {
    s += R"(
[venues.b]
kind = "sim"
supports_replace = true
[venues.b.fees]
maker_bps = 0.5
taker_bps = 3.0
)";
  }
  s += R"(
[[instruments]]
venue = "a"
symbol = "S0"
base = "B0"
quote = "USD"
tick = "0.01"
lot = "1"
min_qty = "1"
)";
  s += fmt::format(R"(
[[instruments]]
venue = "{}"
symbol = "S1"
base = "B1"
quote = "USD"
tick = "0.01"
lot = "1"
min_qty = "1"
)",
                   second_venue ? "b" : "a");
  s += R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1.0
skew_bps_per_unit = 0.1
quote_qty = 1
max_inventory = 10
requote_threshold_ticks = 1
pull_on_stale_ms = 0

[risk]
max_order_qty = "100"
max_order_notional = "100000"
max_position = "50"
max_open_orders = 32
stp = true
stale_md_ms = 0

[backtest]
fill_model = "l2_queue"
latency_fixed_us = 100
latency_jitter_us = 20
equity_bar_s = 1
)";
  if (accounts) {
    s += R"(
[backtest.venues.a.balances]
USD = "100000"
B0 = "50"
B1 = "50"

[backtest.venues.m.balances]
USD = "100000"
B0 = "50"
B1 = "50"
)";
  }
  return s;
}

struct Recording {
  std::string path;
  std::string toml;
  std::size_t records = 0;
};

// What the rewriter adds at the record `at` of a recorded session.
struct Added {
  std::size_t at = 0;
  std::vector<std::uint64_t> bytes;
};

// A backtest over a random market, recorded.
std::string record_session(const std::string& name, const std::string& toml, std::size_t events) {
  const std::string path = tmp_journal(name);
  BacktestConfig c = BacktestConfig::from_config(Config::parse(toml));
  c.measure_wall_clock = false;
  c.output_dir.clear();
  c.journal_out = path;
  c.config_toml = toml;
  RandomMarketConfig rc;
  rc.events = events;
  rc.seed = 5;
  rc.venues = {VenueId{0}, c.instruments.get(InstrumentId{1}).venue};
  rc.tickers = true;
  RandomMarket market(rc);
  const BacktestResult r = run_backtest(c, "basic_mm", &market);
  CHECK(r.fills.size() > 50);
  return path;
}

template <class M>
std::vector<std::uint64_t> words_of(const M& m) {
  std::vector<std::uint64_t> w((m.hdr.len + 7) / 8);
  std::memcpy(w.data(), &m, m.hdr.len);
  return w;
}

// The session's records in order, each with what the rewriter adds ahead of it: a parameter update,
// a pull and resume of the quotes, money in and out of the accounts. Its parameter updates name the
// parameter table of the first part.
std::vector<Added> additions(const std::string& source, bool accounts = false) {
  JournalReader r;
  REQUIRE(r.open(source));
  std::vector<std::int64_t> recv;
  r.for_each([&](const EventHeader* h) { recv.push_back(h->recv_ts.ns); });
  r.reset();
  std::size_t half_spread = 0;
  for (std::size_t i = 0; i < r.params().size(); ++i) {
    if (r.params()[i].name == "half_spread_bps") half_spread = i;
  }
  std::vector<Added> out;
  const auto at_share = [&](double s) {
    return static_cast<std::size_t>(s * static_cast<double>(recv.size()));
  };
  const auto time_at = [&](std::size_t k) { return Timestamp{recv[k]}; };
  {
    ParamUpdateMsg m{};
    init_header(m, EventType::ParamUpdate);
    m.count = 1;
    m.field[0] = static_cast<std::uint16_t>(half_spread);
    m.value[0] = Ratio::from_double(1.4).raw;
    m.origin = ParamUpdateMsg::Origin::Control;
    const std::size_t k = at_share(0.3);
    m.hdr.recv_ts = time_at(k);
    out.push_back(Added{k, words_of(m)});
  }
  for (const auto& [share, command] : {std::pair{0.45, ControlCommand::PullQuotes},
                                       std::pair{0.55, ControlCommand::ResumeQuotes}}) {
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.command = command;
    const std::size_t k = at_share(share);
    m.hdr.recv_ts = time_at(k);
    out.push_back(Added{k, words_of(m)});
  }
  {
    AccountFlowMsg m{};
    init_header(m, EventType::AccountFlow, InstrumentId{}, VenueId{0});
    m.asset = "USD";
    m.amount = Notional::from_int(25);
    const std::size_t k = at_share(0.7);
    m.hdr.recv_ts = time_at(k);
    out.push_back(Added{k, words_of(m)});
  }
  if (accounts) {
    // The connector reads the account again.
    BalanceMsg m{};
    init_header(m, EventType::Balance, InstrumentId{}, VenueId{0});
    m.asset = "USD";
    m.total = Notional::from_int(100025);
    m.free = m.total;
    m.flags = BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd;
    const std::size_t k = at_share(0.8);
    m.hdr.recv_ts = time_at(k);
    out.push_back(Added{k, words_of(m)});
  }
  return out;
}

// Writes `source` out again as a live session's journal at `path`: `tick(n)` after each record,
// with parts of `max_bytes`, drained as a session's writer thread would. `tick` lets a test pace
// it.
void rewrite_live(const std::string& source,
                  const std::string& path,
                  std::size_t max_bytes,
                  const std::vector<Added>& added,
                  const std::function<void(std::size_t)>& tick = {}) {
  JournalReader r;
  REQUIRE(r.open(source));
  InstrumentTable table;
  for (const Instrument& i : r.instruments()) REQUIRE(table.add(i));
  MsgRing ring(1U << 24);
  std::vector<std::pair<std::string, std::uint8_t>> params;
  JournalSessionInfo info;
  info.session_id = r.header().session_id + 1;
  info.start_ts = Timestamp{r.header().start_ts_ns};
  info.rng_seed = r.header().rng_seed;
  info.strategy = "basic_mm";
  info.instruments = &table;
  info.has_session = true;
  info.quoting_enabled = true;
  info.replace_venues = r.header().replace_venues;
  info.config_toml = r.config_text();
  info.tsc.tsc0 = 1;  // a live session's: its feed shows its orders
  register_builtin_strategies();
  info.params = StrategyRegistry::instance().find("basic_mm")->schema;
  JournalOptions jo;
  jo.max_bytes = max_bytes;
  JournalFileWriter fw(ring, path, info, jo);
  REQUIRE(fw.ok());
  // Paced, the writer's own thread drains the ring, as a session's does.
  if (tick) fw.start();
  JournalWriter w(&ring);
  std::size_t n = 0;
  std::size_t next_added = 0;
  r.for_each([&](const EventHeader* h) {
    while (next_added < added.size() && added[next_added].at == n) {
      const auto* e = reinterpret_cast<const EventHeader*>(added[next_added].bytes.data());
      static_cast<void>(w.record(*e));
      ++next_added;
    }
    if (tick) {
      while (!w.record(*h)) std::this_thread::yield();
      tick(++n);
      return;
    }
    while (!w.record(*h)) REQUIRE(fw.drain_once() > 0);
    if ((++n & 63U) == 0) fw.drain_once();
  });
  if (!tick) fw.drain_once();
  fw.stop();
}

BacktestConfig finished_config(const std::string& path, Duration window) {
  JournalSource s(path, true);
  return shadow_config(s.reader(), *s.recorded(), window);
}

ShadowOptions shadow_opts(const SweepPoints& variants, int threads) {
  ShadowOptions o;
  o.variants = variants;
  o.threads = threads;
  o.window = seconds(1);
  o.settle = seconds(3);
  o.keep_results = true;
  o.follow.trailer_grace = seconds(10);
  o.follow.poll = microseconds(200);
  return o;
}

// Each point against its backtest and its sweep over the finished journal.
void check_points(const std::string& path,
                  const SweepPoints& points,
                  const std::vector<BacktestResult>& shadow) {
  const BacktestConfig start = shared_start(finished_config(path, seconds(1)));
  std::vector<BacktestConfig> cfgs;
  cfgs.push_back(point_config(start, ParamMap{}));
  for (const ParamMap& p : points) cfgs.push_back(point_config(start, p));
  REQUIRE(shadow.size() == cfgs.size());
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    return std::make_unique<JournalSource>(path, true);
  };
  const std::vector<BacktestResult> sweep = run_multiverse(cfgs, "basic_mm", factory, 1);
  std::uint64_t fills = 0;
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    JournalSource source(path, true);
    const BacktestResult alone = run_backtest(cfgs[i], "basic_mm", &source);
    CHECK(result_difference(alone, shadow[i]) == "");
    CHECK(result_difference(sweep[i], shadow[i]) == "");
    fills += alone.fills.size();
  }
  CHECK(fills > 0);
}

std::vector<std::vector<std::uint64_t>> stream_of(MdSource& s, bool records) {
  std::vector<std::vector<std::uint64_t>> out;
  while (const EventHeader* h = s.next()) {
    if (!records && h->type == EventType::Padding) continue;
    std::vector<std::uint64_t> w((h->len + 7) / 8);
    std::memcpy(w.data(), h, h->len);
    out.push_back(std::move(w));
  }
  return out;
}

const SweepPoints kVariants = {{{"half_spread_bps", "0.6"}},
                               {{"half_spread_bps", "2.5"}},
                               {{"quote_qty", "2"}},
                               {{"half_spread_bps", "1.4"}, {"quote_qty", "3"}},
                               {{"queue_conservatism", "0.5"}}};

// Removes what does not depend on the journal's records: when the report was asked for.
std::string settled(std::string json) {
  const std::size_t a = json.find("\"state\":");
  if (a != std::string::npos) json.erase(a, json.find('\n', a) - a);
  // How the writer happened to cut its parts.
  for (std::size_t p = json.find("\"parts\":"); p != std::string::npos; p = json.find("\"parts\":"))
    json.erase(p, json.find(',', p) - p);
  return json;
}

}  // namespace

TEST_CASE("backtest.shadow.exact: a finished journal, against its backtest and sweep") {
  const std::string source = record_session("shadow_exact_src.fmj", session_toml(false), 20'000);
  const std::string path = tmp_journal("shadow_exact.fmj");
  rewrite_live(source, path, 1U << 18, additions(source));
  CHECK(std::filesystem::exists(JournalFileWriter::part_path(path, 1)));  // rotated
  Shadow shadow(shadow_opts(kVariants, 1));
  const ShadowSessionResult r = shadow.session(path);
  CHECK(r.late == 0);
  CHECK(r.late_start == 0);
  CHECK(r.seq_gaps == 0);
  CHECK(r.parts > 1);
  check_points(path, kVariants, r.results);
  // The update of half_spread_bps reached the point that does not set it, not those that do.
  CHECK(r.results[0].outbound_sha256 != r.results[2].outbound_sha256);
}

TEST_CASE("backtest.shadow.exact: two venues, on two threads") {
  const std::string source = record_session("shadow_two_src.fmj", session_toml(true), 20'000);
  const std::string path = tmp_journal("shadow_two.fmj");
  rewrite_live(source, path, 1U << 18, additions(source));
  Shadow shadow(shadow_opts(kVariants, 2));
  const ShadowSessionResult r = shadow.session(path);
  CHECK(r.late == 0);
  check_points(path, kVariants, r.results);
}

TEST_CASE("backtest.shadow.exact: a pool of accounts with the balances it reported") {
  const std::string source =
      record_session("shadow_pool_src.fmj", session_toml(false, true), 20'000);
  const std::string path = tmp_journal("shadow_pool.fmj");
  rewrite_live(source, path, 1U << 18, additions(source, true));
  Shadow shadow(shadow_opts(kVariants, 2));
  const ShadowSessionResult r = shadow.session(path);
  CHECK(r.late == 0);
  CHECK(r.late_start == 0);
  CHECK(finished_config(path, seconds(1)).balances_from_journal);
  CHECK(finished_config(path, seconds(1)).transport.pools.primary(VenueId{1}) == VenueId{0});
  check_points(path, kVariants, r.results);
}

TEST_CASE("backtest.shadow.follow: the stream of a finished journal is JournalSource's") {
  const std::string source = record_session("follow_src.fmj", session_toml(false), 8'000);
  const std::string path = tmp_journal("follow.fmj");
  rewrite_live(source, path, 1U << 18, additions(source));
  JournalSource whole(path, true);
  FollowOptions fo;
  fo.settle = seconds(3);
  fo.trailer_grace = seconds(10);
  FollowSource follow(path, fo);
  CHECK(follow.start_ts() == whole.start_ts());
  const auto a = stream_of(whole, true);
  const auto b = stream_of(follow, false);
  REQUIRE(a.size() == b.size());
  std::size_t differ = 0;
  for (std::size_t i = 0; i < a.size(); ++i) differ += a[i] != b[i] ? 1U : 0U;
  CHECK(differ == 0);
  CHECK(follow.late() == 0);
  CHECK(follow.md_events() >= a.size());
}

TEST_CASE("backtest.shadow.follow: a journal followed as it grows, parts and all") {
  const std::string source = record_session("grow_src.fmj", session_toml(false), 8'000);
  const std::string done = tmp_journal("grow_done.fmj");
  const std::vector<Added> added = additions(source);
  rewrite_live(source, done, 1U << 18, added);
  FollowOptions fo;
  fo.settle = seconds(3);
  fo.trailer_grace = seconds(10);
  fo.poll = microseconds(100);
  // With no settle at all much comes too late for what has gone out, and what the two streams hold
  // is still the same: it does not depend on how far the journal had been written.
  for (const Duration settle : {seconds(3), Duration{}}) {
    CAPTURE(settle.ns);
    fo.settle = settle;
    FollowSource finished(done, fo);
    const auto want = stream_of(finished, true);
    // The same records, written while a reader follows: with a pause now and then, the reader
    // catches the writer at every kind of boundary.
    const std::string grow = tmp_journal("grow.fmj");
    Xoshiro256ss rng(11);
    std::thread writer([&] {
      rewrite_live(source, grow, 1U << 16, added, [&](std::size_t n) {
        if ((n & 255U) == 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(rng.uniform(40)));
      });
    });
    // The file does not exist yet, or holds no market data yet: the source waits.
    FollowSource following(grow, fo);
    const auto got = stream_of(following, true);
    writer.join();
    REQUIRE(got.size() == want.size());
    std::size_t differ = 0;
    for (std::size_t i = 0; i < want.size(); ++i) differ += want[i] != got[i] ? 1U : 0U;
    CHECK(differ == 0);
    CHECK(following.late() == finished.late());
    CHECK(following.parts() > 1);
    CHECK(following.seq_gaps() == 0);
  }
}

TEST_CASE(
    "backtest.shadow.exact: a shadow that follows a growing journal gives the finished one's") {
  const std::string source = record_session("shadow_grow_src.fmj", session_toml(false), 12'000);
  const std::string done = tmp_journal("shadow_grow_done.fmj");
  const std::vector<Added> added = additions(source);
  rewrite_live(source, done, 1U << 20, added);
  for (const int threads : {1, 2}) {
    CAPTURE(threads);
    Shadow finished(shadow_opts(kVariants, threads));
    const ShadowSessionResult a = finished.session(done);
    const std::string grow = tmp_journal(fmt::format("shadow_grow_{}.fmj", threads));
    Xoshiro256ss rng(static_cast<std::uint64_t>(threads));
    std::thread writer([&] {
      rewrite_live(source, grow, 1U << 16, added, [&](std::size_t n) {
        if ((n & 255U) == 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(rng.uniform(40)));
      });
    });
    Shadow following(shadow_opts(kVariants, threads));
    const ShadowSessionResult b = following.session(grow);
    writer.join();
    REQUIRE(a.results.size() == b.results.size());
    for (std::size_t i = 0; i < a.results.size(); ++i) {
      CAPTURE(i);
      CHECK(result_difference(a.results[i], b.results[i]) == "");
    }
    CHECK(a.md_events == b.md_events);
    CHECK(finished.windows() == following.windows());
    // Every number of the report, its windows included, but the paths and the file names.
    std::string ra = settled(finished.report_json());
    std::string rb = settled(following.report_json());
    const auto no_path = [](std::string& s, const std::string& p) {
      for (std::size_t at = s.find(p); at != std::string::npos; at = s.find(p))
        s.erase(at, p.size());
    };
    no_path(ra, done);
    no_path(rb, grow);
    CHECK(ra == rb);
  }
}

TEST_CASE("backtest.shadow.report: forgetting the rows changes no number") {
  const std::string source = record_session("shadow_forget_src.fmj", session_toml(false), 12'000);
  const std::string path = tmp_journal("shadow_forget.fmj");
  rewrite_live(source, path, 0, additions(source));
  Shadow keeping(shadow_opts(kVariants, 1));
  static_cast<void>(keeping.session(path));
  ShadowOptions o = shadow_opts(kVariants, 1);
  o.keep_results = false;
  Shadow forgetting(o);
  const ShadowSessionResult f = forgetting.session(path);
  // End's result covers what was not forgotten: the rows are mostly gone.
  CHECK(settled(keeping.report_json()) == settled(forgetting.report_json()));
  CHECK(f.results[0].equity.size() < 4);
}

TEST_CASE(
    "backtest.shadow.report: at the session's own parameters a deterministic session has no "
    "error") {
  // A backtest's journal says nothing of its orders in its market data: the first point runs the
  // same backtest again, and its fills are the session's own, window by window.
  const std::string path = record_session("shadow_self.fmj", session_toml(false), 20'000);
  ShadowOptions o = shadow_opts(kVariants, 1);
  Shadow shadow(o);
  const ShadowSessionResult r = shadow.session(path);
  CHECK(r.results[0].fills.size() > 50);
  const std::string report = shadow.report_json();
  INFO(report);
  // The report's first comparison: the same notional, fills and net.
  const auto field = [&](std::string_view key) {
    const std::size_t all = report.find("\"all\":");
    const std::size_t at = report.find(std::string("\"") + std::string(key) + "\":", all);
    REQUIRE(at != std::string::npos);
    return std::strtod(report.c_str() + at + key.size() + 3, nullptr);
  };
  CHECK(std::fabs(field("net_error")) < 1e-6);
  CHECK(field("notional_ratio") == doctest::Approx(1.0));
  CHECK(field("fills_ratio") == doctest::Approx(1.0));
}

TEST_CASE("backtest.shadow.watch: stopping some runs changes nothing for the others") {
  const std::string path = record_session("shadow_watch.fmj", session_toml(false), 12'000);
  BacktestConfig cfg = finished_config(path, seconds(1));
  const BacktestConfig start = shared_start(cfg);
  std::vector<BacktestConfig> cfgs;
  cfgs.push_back(point_config(start, ParamMap{}));
  for (const ParamMap& p : kVariants) cfgs.push_back(point_config(start, p));
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    return std::make_unique<JournalSource>(path);
  };
  const std::vector<BacktestResult> plain = run_multiverse(cfgs, "basic_mm", factory, 1);
  std::size_t ticks = 0;
  MultiverseWatch watch;
  watch.every = seconds(1);
  watch.drop = [&](std::size_t index) { return ticks >= 2 && index >= 3; };
  watch.tick = [&](Timestamp, std::span<const WatchedRun> runs) {
    ++ticks;
    CHECK(runs.size() == cfgs.size());
  };
  const std::vector<BacktestResult> watched =
      run_multiverse(cfgs, "basic_mm", factory, 1, {}, &watch);
  CHECK(ticks >= 3);
  for (std::size_t i = 0; i < 3; ++i) CHECK(result_difference(plain[i], watched[i]) == "");
  // The stopped ones have what they had when they stopped.
  for (std::size_t i = 3; i < cfgs.size(); ++i)
    CHECK(watched[i].fills.size() <= plain[i].fills.size());
  CHECK(watched[3].outbound_sha256 != plain[3].outbound_sha256);
  // More configurations than one wave holds cannot be watched: the data is read once.
  std::vector<BacktestConfig> many(kMaxRunsPerPass + 1, cfgs[0]);
  CHECK_THROWS_AS(static_cast<void>(run_multiverse(many, "basic_mm", factory, 1, {}, &watch)),
                  std::invalid_argument);
}

TEST_CASE("backtest.shadow.exact: operator commands reach the runs at their times") {
  const std::string source = record_session("shadow_ctl_src.fmj", session_toml(false), 20'000);
  const std::string with = tmp_journal("shadow_ctl.fmj");
  rewrite_live(source, with, 0, additions(source));
  // The quotes pulled for a tenth of the session: fewer orders go out than without the command.
  BacktestConfig cfg = finished_config(with, seconds(1));
  cfg = point_config(shared_start(cfg), ParamMap{});
  cfg.keep_orders = true;
  JournalSource a(with, true);
  const BacktestResult pulled = run_backtest(cfg, "basic_mm", &a);
  cfg.controls_from_journal = false;
  JournalSource b(with, true);
  const BacktestResult free_running = run_backtest(cfg, "basic_mm", &b);
  CHECK(pulled.outbound_sha256 != free_running.outbound_sha256);
  // Between the pull and the resume, new orders go out only without the commands.
  std::int64_t pull = 0;
  std::int64_t resume = 0;
  for (const sim::RecordedSession* rs = a.recorded(); const auto& bytes : rs->controls) {
    const auto& m = *reinterpret_cast<const ControlMsg*>(bytes.data());
    (m.command == ControlCommand::PullQuotes ? pull : resume) = m.hdr.recv_ts.ns;
  }
  REQUIRE(resume > pull);
  const auto new_orders = [&](const BacktestResult& r) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < r.orders.ts.size(); ++i) {
      if (r.orders.kind[i] == kOrderKindNew && r.orders.ts[i] > pull + 1'000'000 &&
          r.orders.ts[i] < resume)
        ++n;
    }
    return n;
  };
  CHECK(new_orders(pulled) == 0);
  CHECK(new_orders(free_running) > 0);
}

TEST_CASE(
    "backtest.shadow.exact: a sweep's point keeps what it sets against the journal's updates") {
  const std::string source = record_session("shadow_pin_src.fmj", session_toml(false), 20'000);
  const std::string path = tmp_journal("shadow_pin.fmj");
  rewrite_live(source, path, 0, additions(source));
  // half_spread_bps = 1.4 is what the update sets: after it the point set to 1.0 at the start and
  // following the update would quote as this one does, pinned at 1.4 from the start.
  BacktestConfig cfg = finished_config(path, seconds(1));
  const BacktestConfig start = shared_start(cfg);
  const BacktestConfig pinned = point_config(start, {{"half_spread_bps", "2.0"}});
  CHECK(pinned.params_pinned == std::vector<std::string>{"half_spread_bps"});
  BacktestConfig unpinned = pinned;
  unpinned.params_pinned.clear();
  JournalSource a(path, true);
  JournalSource b(path, true);
  CHECK(run_backtest(pinned, "basic_mm", &a).outbound_sha256 !=
        run_backtest(unpinned, "basic_mm", &b).outbound_sha256);
}

TEST_CASE("backtest.shadow.start: a session's first checkpoint is not reached for from its start") {
  sim::RecordedSession s;
  s.from_session_start = true;
  sim::Checkpoint c;
  c.at = Timestamp{50};
  c.strategy = "later";
  c.before_orders = false;
  s.checkpoints.push_back(c);
  CHECK_FALSE(s.start().has_state);
  s.checkpoints[0].before_orders = true;  // a warm standby's take-over
  CHECK(s.start().has_state);
  s.checkpoints[0].before_orders = false;
  s.from_session_start = false;  // a part from the middle of a session
  CHECK(s.start().has_state);
}

TEST_CASE("backtest.shadow.intervals: simultaneous over the series, and of the right width") {
  constexpr std::size_t kWindows = 60;
  constexpr std::size_t kSeries = 20;
  constexpr int kTrials = 200;
  Xoshiro256ss rng(17);
  const auto gauss = [&] {
    // Sum of uniforms: near enough for a coverage check.
    double x = -6.0;
    for (int i = 0; i < 12; ++i) x += static_cast<double>(rng.uniform(1'000'000)) / 1e6;
    return x;
  };
  int family_covered = 0;
  int single_covered = 0;
  for (int t = 0; t < kTrials; ++t) {
    std::vector<std::vector<double>> series(kSeries, std::vector<double>(kWindows));
    for (auto& s : series) {
      for (double& x : s) x = gauss();
    }
    const std::vector<PairedSum> sums =
        simultaneous_sums(series, 0.95, 400, static_cast<std::uint64_t>(t) + 1);
    bool all = true;
    for (const PairedSum& p : sums) all = all && p.lo <= 0.0 && p.hi >= 0.0;
    family_covered += all ? 1 : 0;
    const std::vector<PairedSum> one =
        simultaneous_sums({series[0]}, 0.95, 400, static_cast<std::uint64_t>(t) + 1);
    single_covered += one[0].lo <= 0.0 && one[0].hi >= 0.0 ? 1 : 0;
  }
  // All 20 true sums inside, in nearly every trial; a single interval is narrower and covers one.
  CHECK(family_covered >= kTrials * 80 / 100);
  CHECK(single_covered >= kTrials * 85 / 100);
  // Too few windows: no interval.
  const std::vector<PairedSum> few = simultaneous_sums({std::vector<double>(3, 1.0)});
  CHECK(few[0].sum == doctest::Approx(3.0));
  CHECK(std::isnan(few[0].lo));
}
