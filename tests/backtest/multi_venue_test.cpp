// Backtests across venues: each venue with its own latency, cancel-replace and STP, recorded
// feeds of several venues merged by event time, equity per instrument.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::int64_t kStartNs = 1'789'344'931'000'000'000LL;
constexpr std::int64_t kStepNs = 20'000'000;  // 20 ms
constexpr int kSteps = 1000;                  // 20 s

// Venue `a` (id 0) trades AAA, venue `b` (id 1) trades BBB. Latencies differ per venue.
constexpr const char* kConfig = R"(
[engine]
rng_seed = 5
supports_replace = true
min_requote_interval_ms = 0

[venues.a]
kind = "sim"
[venues.a.fees]
maker_bps = 0.0
taker_bps = 2.0

[venues.b]
kind = "sim"
[venues.b.fees]
maker_bps = 1.0
taker_bps = 4.0

[[instruments]]
venue = "a"
symbol = "AAA"
tick = "0.01"
lot = "0.001"

[[instruments]]
venue = "b"
symbol = "BBB"
tick = "0.01"
lot = "0.001"

[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1.0
skew_bps_per_unit = 0.0
quote_qty = 0.01
max_inventory = 0.2
requote_threshold_ticks = 1
pull_on_stale_ms = 0

[risk]
max_order_qty = "1"
max_order_notional = "1000"
max_position = "1"
max_open_orders = 16
stale_md_ms = 0
stp = true

[backtest]
fill_model = "l2_queue"
latency_fixed_us = 100
latency_jitter_us = 0
equity_bar_s = 1
markout_horizons_s = ""

[backtest.venues.b]
latency_fixed_us = 2500
latency_md_us = 1000
supports_replace = false
)";

std::filesystem::path tmp_file(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p;
}

// One recorded feed per venue as CSV (`ts_ns,type,inst,side,price,qty,seq`): a two-sided snapshot
// every step around a random walk, a trade through one side every fourth step. Venue b's events
// are `offset_ns` later than venue a's.
std::string feed_csv(std::uint32_t inst, std::int64_t offset_ns, double mid0, std::uint64_t seed) {
  std::string out = "ts_ns,type,inst,side,price,qty,seq\n";
  Xoshiro256ss rng(seed);
  auto mid_ticks = static_cast<std::int64_t>(mid0 * 100.0);
  for (int k = 0; k < kSteps; ++k) {
    const std::int64_t ts = kStartNs + k * kStepNs + offset_ns;
    mid_ticks += static_cast<std::int64_t>(rng.next() % 3U) - 1;
    const auto seq = static_cast<std::uint64_t>(k) + 1;
    const std::string bid = decimal((mid_ticks - 2) * 1'000'000);
    const std::string ask = decimal((mid_ticks + 2) * 1'000'000);
    out += fmt::format("{0},S,{1},B,{2},1,{4}\n{0},S,{1},A,{3},1,{4}\n", ts, inst, bid, ask, seq);
    if (k % 4 == 3) {
      const bool sell = (rng.next() & 1U) != 0;
      out += fmt::format(
          "{},T,{},{},{},2,{}\n", ts + 1'000'000, inst, sell ? "A" : "B", sell ? bid : ask, seq);
    }
  }
  return out;
}

struct Feeds {
  std::string a;
  std::string b;
  [[nodiscard]] std::string spec() const { return "csv:" + a + ",venue=0; csv:" + b + ",venue=1"; }
};

// `tag` keeps the files of test cases that ctest runs in parallel apart.
Feeds write_feeds(const std::string& tag) {
  const auto a = tmp_file("multi_venue_" + tag + "_a.csv");
  const auto b = tmp_file("multi_venue_" + tag + "_b.csv");
  std::ofstream(a) << feed_csv(0, 0, 100.0, 11);
  std::ofstream(b) << feed_csv(1, 7'000'000, 101.0, 12);
  return {a.string(), b.string()};
}

BacktestConfig two_venue_config(const std::string& extra = "",
                                std::string_view b_latency = "latency_fixed_us = 2500") {
  std::string text = kConfig;
  text.replace(text.find("latency_fixed_us = 2500"), 23, b_latency);
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(text + extra));
  cfg.measure_wall_clock = false;
  return cfg;
}

BacktestResult run(const BacktestConfig& cfg, const Feeds& feeds) {
  std::unique_ptr<MdSource> src = open_data(feeds.spec(), &cfg.instruments);
  REQUIRE(src != nullptr);
  return run_backtest(cfg, "basic_mm", src.get());
}

bool is_order_event(EventType t) {
  return t == EventType::OrderAck || t == EventType::OrderFill || t == EventType::OrderReject ||
         t == EventType::OrderCancelAck || t == EventType::OrderCancelReject ||
         t == EventType::OrderExpired;
}
bool is_md(EventType t) {
  return t == EventType::BookDelta || t == EventType::BookSnapshot || t == EventType::Trade;
}

}  // namespace

TEST_CASE("backtest.multi_venue: config reads per-venue latency, replace and STP") {
  const BacktestConfig cfg = two_venue_config();
  const sim::SimTransportConfig& t = cfg.transport;
  REQUIRE(t.venues.size() == 1);
  const sim::SimVenueConfig a = t.venue_config(VenueId{0});
  const sim::SimVenueConfig b = t.venue_config(VenueId{1});
  CHECK(a.order_out.fixed == microseconds(100));
  CHECK(a.ack_in.fixed == microseconds(100));
  CHECK(a.md_in.fixed == Duration{});
  CHECK(a.supports_replace);
  CHECK(a.stp == sim::StpMode::CancelTaker);
  CHECK(b.venue == VenueId{1});
  CHECK(b.order_out.fixed == microseconds(2500));
  CHECK(b.ack_in.fixed == microseconds(2500));
  CHECK(b.order_out.jitter == Duration{});  // [backtest] latency_jitter_us
  CHECK(b.md_in.fixed == microseconds(1000));
  CHECK_FALSE(b.supports_replace);
  CHECK(b.stp == sim::StpMode::CancelTaker);
  CHECK(t.replace_mask() == (~std::uint64_t{0} & ~std::uint64_t{2}));

  const BacktestConfig no_stp = two_venue_config("stp = false\n");
  CHECK(no_stp.transport.venue_config(VenueId{1}).stp == sim::StpMode::None);
  CHECK(no_stp.transport.venue_config(VenueId{0}).stp == sim::StpMode::CancelTaker);

  // The simulated venues answer with their own settings.
  const SimClock clock(Timestamp{kStartNs});
  sim::SimTransport transport(clock, cfg.instruments, cfg.transport);
  REQUIRE(transport.venue_count() == 2);
  CHECK(transport.venue_at(0) == VenueId{0});
  CHECK(transport.venue_at(1) == VenueId{1});
  CHECK(transport.supports_replace(VenueId{0}));
  CHECK_FALSE(transport.supports_replace(VenueId{1}));
  CHECK(transport.latency(VenueId{1}).order_out_params().fixed == microseconds(2500));
  CHECK(transport.latency(VenueId{0}).order_out_params().fixed == microseconds(100));
}

TEST_CASE("backtest.multi_venue: ack latency and md_arrival per venue, else the [backtest] ones") {
  // No ack key anywhere: each venue's acks follow its own order path.
  const BacktestConfig plain = two_venue_config();
  CHECK(plain.transport.venue_config(VenueId{0}).ack_in.fixed == microseconds(100));
  CHECK(plain.transport.venue_config(VenueId{1}).ack_in.fixed == microseconds(2500));
  CHECK_FALSE(plain.transport.venue_config(VenueId{1}).md_recorded_arrival);

  // [backtest] latency_ack_us applies to venue a directly and to b, which sets none of its own.
  std::string text = kConfig;
  text.replace(text.find("latency_jitter_us = 0\n"),
               22,
               "latency_jitter_us = 0\nlatency_ack_us = 700\nmd_arrival = \"recorded\"\n");
  const BacktestConfig g = BacktestConfig::from_config(Config::parse(text));
  CHECK(g.transport.venue_config(VenueId{0}).ack_in.fixed == microseconds(700));
  CHECK(g.transport.venue_config(VenueId{1}).ack_in.fixed == microseconds(700));
  CHECK(g.transport.venue_config(VenueId{1}).order_out.fixed == microseconds(2500));
  CHECK(g.transport.venue_config(VenueId{0}).md_recorded_arrival);
  CHECK(g.transport.venue_config(VenueId{1}).md_recorded_arrival);

  // A venue's own keys win.
  const BacktestConfig own = two_venue_config(
      "",
      "latency_fixed_us = 2500\nlatency_ack_us = 4000\nlatency_ack_jitter_us = 30\n"
      "md_arrival = \"recorded\"");
  const sim::SimVenueConfig b = own.transport.venue_config(VenueId{1});
  CHECK(b.ack_in.fixed == microseconds(4000));
  CHECK(b.ack_in.jitter == microseconds(30));
  CHECK(b.order_out.fixed == microseconds(2500));
  CHECK(b.md_recorded_arrival);
  CHECK_FALSE(own.transport.venue_config(VenueId{0}).md_recorded_arrival);
  CHECK(own.transport.venue_config(VenueId{0}).ack_in.fixed == microseconds(100));
  CHECK_THROWS_WITH_AS(
      static_cast<void>(two_venue_config("", "latency_fixed_us = 2500\nmd_arrival = \"late\"")),
      doctest::Contains("backtest.venues.b.md_arrival: 'late' is not venue or recorded"),
      ConfigError);

  // md_arrival = "recorded" on venue b only: b's market data arrives at its recorded recv_ts plus
  // its 1 ms md latency, a's at its venue time; each wire stays in order on its own.
  const SimClock clock(Timestamp{kStartNs});
  sim::SimTransport transport(clock, own.instruments, own.transport);
  InlineFeed feed(1U << 20);
  const auto trade = [&](std::uint32_t inst, std::int64_t exch, std::int64_t recv) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{inst}, VenueId{static_cast<std::uint8_t>(inst)});
    t.hdr.exch_ts = Timestamp{exch};
    t.hdr.recv_ts = Timestamp{recv};
    t.price = px("100");
    t.qty = qt("1");
    transport.on_source_event(t.hdr);
  };
  trade(0, kStartNs, kStartNs + 5'000'000);              // a: venue time
  trade(1, kStartNs, kStartNs + 5'000'000);              // b: recorded, 5 ms late
  trade(1, kStartNs + 1'000'000, kStartNs + 2'000'000);  // b: recorded earlier, waits
  std::vector<std::pair<std::uint32_t, std::int64_t>> got;
  while (transport.inbound_pending()) {
    REQUIRE(transport.deliver_next_inbound(feed) == EventType::Trade);
    const EventHeader* h = feed.next();
    REQUIRE(h != nullptr);
    got.emplace_back(h->instrument.value, h->recv_ts.ns);
    feed.release();
  }
  REQUIRE(got.size() == 3);
  CHECK(got[0] == std::pair<std::uint32_t, std::int64_t>{0, kStartNs});
  CHECK(got[1] == std::pair<std::uint32_t, std::int64_t>{1, kStartNs + 6'000'000});
  CHECK(got[2] == std::pair<std::uint32_t, std::int64_t>{1, kStartNs + 6'000'000});
}

TEST_CASE("backtest.multi_venue: every order, ack and market-data event uses its venue's latency") {
  const Feeds feeds = write_feeds("latency");
  BacktestConfig cfg = two_venue_config();
  cfg.journal_out = tmp_file("multi_venue.fmj").string();
  const BacktestResult r = run(cfg, feeds);
  REQUIRE(r.outbound_messages > 0);

  // Engine -> venue: the orders' arrival at their venue.
  const Duration out_lat[2] = {microseconds(100), microseconds(2500)};
  std::size_t per_inst[2] = {0, 0};
  std::size_t replaces[2] = {0, 0};
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    const std::uint32_t inst = r.orders.instrument[i];
    REQUIRE(inst < 2);
    REQUIRE(r.orders.venue_ts[i] != 0);
    CAPTURE(i);
    CHECK(r.orders.venue_ts[i] - r.orders.ts[i] == out_lat[inst].ns);
    ++per_inst[inst];
    if (r.orders.kind[i] == kOrderKindReplace) ++replaces[inst];
  }
  CHECK(per_inst[0] > 10);
  CHECK(per_inst[1] > 10);
  // Venue a replaces its quotes in place; b, without replace, cancels and sends new ones.
  CHECK(replaces[0] > 10);
  CHECK(replaces[1] == 0);

  // Venue -> engine, as the engine consumed it (journal): acks and fills after the venue's ack
  // latency, market data after its md latency, each stamped with its instrument's venue.
  const Duration ack_lat[2] = {microseconds(100), microseconds(2500)};
  const Duration md_lat[2] = {Duration{}, microseconds(1000)};
  JournalReader jr;
  REQUIRE(jr.open(cfg.journal_out));
  std::size_t acks[2] = {0, 0};
  std::size_t md[2] = {0, 0};
  std::size_t fills[2] = {0, 0};
  jr.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) != 0) return;
    const bool order = is_order_event(e->type);
    if (!order && !is_md(e->type)) return;
    if (!e->instrument.valid()) return;  // a cancel reject of an unknown order
    const std::uint32_t inst = e->instrument.value;
    REQUIRE(inst < 2);
    CHECK(e->venue == VenueId{static_cast<std::uint8_t>(inst)});
    const std::int64_t lat = e->recv_ts.ns - e->exch_ts.ns;
    if (order) {
      CHECK(lat == ack_lat[inst].ns);
      ++acks[inst];
      if (e->type == EventType::OrderFill) ++fills[inst];
    } else {
      CHECK(lat == md_lat[inst].ns);
      ++md[inst];
    }
  });
  CHECK(acks[0] > 10);
  CHECK(acks[1] > 10);
  CHECK(fills[0] > 0);
  CHECK(fills[1] > 0);
  CHECK(md[0] > 1000);
  CHECK(md[1] > 1000);

  // Equity per instrument: the totals are the sum of the instruments (no [accounting]).
  REQUIRE(r.equity.by_instrument.size() == 2);
  const std::size_t last = r.equity.size() - 1;
  const std::int64_t total = r.equity.equity(last);
  CHECK(r.equity.by_instrument[0].pnl[last] + r.equity.by_instrument[1].pnl[last] == total);
  CHECK(r.equity.by_instrument[1].mid[last] > r.equity.by_instrument[0].mid[last]);
  const std::string csv = r.equity_csv();
  CHECK(csv.starts_with(
      "ts_ns,equity,realized,unrealized,fees,position,mid,quoted,pnl_0,position_0,mid_0,quoted_0,"
      "pnl_1,position_1,mid_1,quoted_1\n"));
}

TEST_CASE("backtest.multi_venue: the same run twice gives the same outbound hash") {
  const Feeds feeds = write_feeds("determinism");
  const BacktestConfig cfg = two_venue_config();
  const BacktestResult r1 = run(cfg, feeds);
  const BacktestResult r2 = run(cfg, feeds);
  REQUIRE(r1.outbound_messages > 0);
  CHECK(r1.outbound_sha256 == r2.outbound_sha256);
  CHECK(r1.outbound_messages == r2.outbound_messages);
  CHECK(r1.fills.size() == r2.fills.size());
  CHECK(r1.equity.equity(r1.equity.size() - 1) == r2.equity.equity(r2.equity.size() - 1));

  // Another venue latency changes the run.
  const BacktestResult r3 = run(two_venue_config("", "latency_fixed_us = 400"), feeds);
  CHECK(r3.outbound_sha256 != r1.outbound_sha256);
}

// Clears or sets kHeaderReplacePerVenue in a journal's header.
void set_replace_per_venue(const std::string& path, bool on) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  JournalFileHeader h{};
  REQUIRE(f.read(reinterpret_cast<char*>(&h), sizeof h));
  h.header_flags = static_cast<std::uint8_t>(on ? (h.header_flags | kHeaderReplacePerVenue)
                                                : (h.header_flags & ~kHeaderReplacePerVenue));
  h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
  f.seekp(0);
  REQUIRE(f.write(reinterpret_cast<const char*>(&h), sizeof h));
}

TEST_CASE(
    "backtest.multi_venue: a replay reproduces replace on one venue, cancel and new on the "
    "other") {
  const Feeds feeds = write_feeds("replay");
  BacktestConfig cfg = two_venue_config();
  cfg.journal_out = tmp_file("multi_venue_replay.fmj").string();
  const BacktestResult rec = run(cfg, feeds);
  REQUIRE(rec.outbound_messages > 0);
  JournalReader jr;
  REQUIRE(jr.open(cfg.journal_out));
  CHECK((jr.header().header_flags & kHeaderReplacePerVenue) != 0);
  CHECK(jr.header().replace_venues == cfg.transport.replace_mask());
  const ReplayResult rp = replay_journal(cfg.journal_out, two_venue_config());
  CHECK(rp.ok());
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.outbound_messages == rec.outbound_messages);

  // A journal of the engine before replace was per venue (no flag): replace only if every venue
  // could. Recorded that way, it replays exactly; claiming per-venue replace, it does not.
  BacktestConfig old = two_venue_config();
  old.engine.quotes.replace_all_venues = true;
  old.journal_out = tmp_file("multi_venue_replay_old.fmj").string();
  const BacktestResult rec_old = run(old, feeds);
  CHECK(rec_old.outbound_sha256 != rec.outbound_sha256);
  set_replace_per_venue(old.journal_out, false);
  const ReplayResult rp_old = replay_journal(old.journal_out, two_venue_config());
  CHECK(rp_old.ok());
  CHECK(rp_old.outbound_sha256 == rec_old.outbound_sha256);
  set_replace_per_venue(old.journal_out, true);
  CHECK_FALSE(replay_journal(old.journal_out, two_venue_config()).ok());
}

TEST_CASE("backtest.multi_venue: venue settings equal to the defaults change nothing") {
  // One venue, stated explicitly or not: the same run.
  const Feeds feeds = write_feeds("defaults");
  const std::string one = R"(
[venues.a]
kind = "sim"
[[instruments]]
venue = "a"
symbol = "AAA"
tick = "0.01"
lot = "0.001"
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1.0
quote_qty = 0.01
max_inventory = 0.2
pull_on_stale_ms = 0
[risk]
max_order_qty = "1"
max_order_notional = "1000"
max_position = "1"
stale_md_ms = 0
[backtest]
fill_model = "l2_queue"
latency_fixed_us = 300
latency_jitter_us = 40
markout_horizons_s = ""
)";
  BacktestConfig plain = BacktestConfig::from_config(Config::parse(one));
  BacktestConfig stated = BacktestConfig::from_config(
      Config::parse(one + "[backtest.venues.a]\nlatency_fixed_us = 300\nlatency_jitter_us = 40\n"));
  plain.measure_wall_clock = stated.measure_wall_clock = false;
  REQUIRE(stated.transport.venues.size() == 1);
  const std::string a_only = "csv:" + feeds.a + ",venue=0";
  std::unique_ptr<MdSource> s1 = open_data(a_only, &plain.instruments);
  std::unique_ptr<MdSource> s2 = open_data(a_only, &stated.instruments);
  const BacktestResult r1 = run_backtest(plain, "basic_mm", s1.get());
  const BacktestResult r2 = run_backtest(stated, "basic_mm", s2.get());
  REQUIRE(r1.outbound_messages > 0);
  CHECK(r1.outbound_sha256 == r2.outbound_sha256);
  CHECK(r1.equity.by_instrument.size() == 1);
  CHECK(r1.equity_csv().starts_with("ts_ns,equity,realized,unrealized,fees,position,mid,quoted\n"));
}

TEST_CASE("backtest.multi_venue: two recorded feeds merge by event time") {
  const auto a = tmp_file("merge_a.csv");
  const auto b = tmp_file("merge_b.csv");
  // Venue a: 10, 30, 50, 50; venue b: 20, 30, 40, 50. Ties go to the first source.
  std::ofstream(a) << "ts_ns,type,inst,side,price,qty,seq\n"
                      "10,T,0,B,100,1,1\n30,T,0,B,100,1,2\n50,T,0,B,100,1,3\n50,T,0,A,100,1,4\n";
  std::ofstream(b) << "ts_ns,type,inst,side,price,qty,seq\n"
                      "20,T,1,B,101,1,1\n30,T,1,B,101,1,2\n40,T,1,B,101,1,3\n50,T,1,B,101,1,4\n";
  std::unique_ptr<MdSource> src =
      open_data("csv:" + a.string() + ",venue=0 ;  csv:" + b.string() + ",venue=1", nullptr);
  REQUIRE(src != nullptr);
  struct Seen {
    std::int64_t ts;
    std::uint8_t venue;
    std::uint64_t id;
  };
  const std::vector<Seen> want = {{10, 0, 1},
                                  {20, 1, 1},
                                  {30, 0, 2},
                                  {30, 1, 2},
                                  {40, 1, 3},
                                  {50, 0, 3},
                                  {50, 0, 4},
                                  {50, 1, 4}};
  for (int pass = 0; pass < 2; ++pass) {  // and again after reset()
    std::vector<Seen> got;
    while (const EventHeader* h = src->next()) {
      REQUIRE(h->type == EventType::Trade);
      CHECK(h->venue.value == h->instrument.value);
      got.push_back({h->exch_ts.ns, h->venue.value, msg_cast<TradeMsg>(h).trade_id});
    }
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
      CAPTURE(i);
      CHECK(got[i].ts == want[i].ts);
      CHECK(got[i].venue == want[i].venue);
      CHECK(got[i].id == want[i].id);
    }
    CHECK(src->start_ts() == Timestamp{10});
    src->reset();
  }

  CHECK_THROWS_WITH_AS(static_cast<void>(open_data("csv:" + a.string() + ";;", nullptr)),
                       doctest::Contains("must name a data source"),
                       std::runtime_error);
}

TEST_CASE("backtest.multi_venue: configuration errors name the venue or key") {
  const auto err = [](const std::string& extra) {
    try {
      static_cast<void>(two_venue_config(extra));
    } catch (const ConfigError& e) {
      return std::string(e.what());
    }
    return std::string("(no error)");
  };
  CHECK(err("[backtest.venues.nope]\nlatency_fixed_us = 5\n")
            .find("backtest.venues.nope: no venue 'nope' in [venues] (known: a, b)") !=
        std::string::npos);
  CHECK(err("[backtest.venues.a]\nlatency_fixd_us = 5\n")
            .find("backtest.venues.a.latency_fixd_us: unknown key") != std::string::npos);
  CHECK(err("[backtest.venues.a]\nlatency_fixed_us = -1\n")
            .find("backtest.venues.a.latency_fixed_us must be >= 0") != std::string::npos);
  CHECK(err("[backtest.venues.a]\np_drop = 1.5\n").find("backtest.venues.a.p_drop must be in") !=
        std::string::npos);

  // A venue no instrument trades on.
  const std::string lonely = std::string(kConfig) + "[venues.c]\nkind = \"sim\"\n" +
                             "[backtest.venues.c]\nlatency_fixed_us = 5\n";
  CHECK_THROWS_WITH_AS(
      static_cast<void>(BacktestConfig::from_config(Config::parse(lonely))),
      doctest::Contains("backtest.venues.c: no [[instruments]] trade on venue 'c'"),
      ConfigError);

  // [backtest] source as a TOML list.
  BacktestConfig cfg = two_venue_config();
  cfg.source = "[csv:a.csv, csv:b.csv]";
  CHECK_THROWS_WITH_AS(static_cast<void>(open_source(cfg)),
                       doctest::Contains("separated by ';'"),
                       std::runtime_error);

  // Settings in code for a venue no instrument trades on.
  cfg = two_venue_config();
  sim::SimVenueConfig ghost = cfg.transport.venue_config(VenueId{5});
  cfg.transport.venues.push_back(ghost);
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest(cfg, "basic_mm", nullptr)),
                       doctest::Contains("settings for venue 5, which no instrument trades on"),
                       std::invalid_argument);
}
