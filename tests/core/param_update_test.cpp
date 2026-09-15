// Parameter updates (ADR-0013): the ParamUpdate message, raw parameter values, ParamPublisher
// validation, the engine applying an update from its ring feed, max_param_age before events, and
// the v3 journal parameter table.
#include "test_support.hpp"

#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;
using Values = std::vector<ParamPublisher::ParamValue>;

namespace {

struct TestParams {
  FASTMM_PARAMS(TestParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels")
  FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
  FASTMM_PARAM(bool, hedge, false, false, true, "hedge fills")
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1_qty, "quantity per level")
  FASTMM_PARAM(Qty, max_inventory, 0.1_qty, 0_qty, 10_qty, "inventory cap")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 100_bps, "half spread")
  FASTMM_PARAM_MS(stale_ms, milliseconds(2000), milliseconds(0), milliseconds(60000), "stale book")

  [[nodiscard]] std::optional<std::string> validate() const {
    if (quote_qty > max_inventory) return "quote_qty must not exceed max_inventory";
    return std::nullopt;
  }
};

InstrumentTable one_instrument() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.flags = Instrument::kEnabled;
  i.tick = 0.01_px;
  i.lot = 0.001_qty;
  i.min_qty = 0.001_qty;
  REQUIRE(t.add(i));
  return t;
}

struct CountingTransport {
  std::size_t news = 0;
  std::size_t cancels = 0;
  bool send(const EventHeader& m) noexcept {
    if (m.type == EventType::OutNewOrder) ++news;
    if (m.type == EventType::OutCancel) ++cancels;
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
};

// Quotes on every book and records what it sees.
struct ParamSpy : StrategyBase<TestParams> {
  static constexpr std::string_view name() noexcept { return "param_spy"; }
  int params_calls = 0;
  Ratio spread_seen{};
  Qty qty_seen{};
  std::vector<bool> quoting;
  std::vector<bool> set_quotes_results;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    DesiredQuotes q;
    static_cast<void>(q.bid(book.best_bid().price - 0.01_px, params().quote_qty));
    static_cast<void>(q.ask(book.best_ask().price + 0.01_px, params().quote_qty));
    set_quotes_results.push_back(ctx.set_quotes(id, q));
  }
  template <class Ctx>
  void on_params(Ctx& /*ctx*/) noexcept {
    ++params_calls;
    spread_seen = params().half_spread_bps;
    qty_seen = params().quote_qty;
  }
  template <class Ctx>
  void on_quoting(Ctx& /*ctx*/, bool enabled) noexcept {
    quoting.push_back(enabled);
  }
};
static_assert(verify_strategy<ParamSpy>());

void push_book(InlineFeed& feed, const SimClock& clock, Price bid, Price ask) {
  std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
  REQUIRE(p != nullptr);
  auto* d = reinterpret_cast<BookDeltaMsg*>(p);
  init_header(
      *d, EventType::BookSnapshot, InstrumentId{0}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
  d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.recv_ts = clock.now();
  d->bid_count = d->ask_count = 1;
  d->levels()[0] = Level{bid, 1_qty};
  d->levels()[1] = Level{ask, 1_qty};
  feed.commit();
}

const EventHeader* front(MsgRing& ring) {
  return reinterpret_cast<const EventHeader*>(ring.try_peek());
}

}  // namespace

TEST_CASE("core.params: ParamUpdate message, event type and kill reason") {
  CHECK(static_cast<int>(EventType::ParamUpdate) == 26);
  CHECK(static_cast<int>(EventType::Count) == 27);
  CHECK(to_string(EventType::ParamUpdate) == "ParamUpdate");
  CHECK(static_cast<int>(KillReason::StrategyError) == 9);
  CHECK(to_string(KillReason::StrategyError) == "StrategyError");
  ParamUpdateMsg m{};
  init_header(m, EventType::ParamUpdate);
  CHECK(m.hdr.len == 448);
  CHECK(m.all_instruments());
  init_header(m, EventType::ParamUpdate, InstrumentId{3});
  CHECK_FALSE(m.all_instruments());
}

TEST_CASE("core.params: every parameter type converts to a raw value and back") {
  TestParams a;
  const ParamMap values{{"levels", "3"},
                        {"gamma", "2.5"},
                        {"hedge", "true"},
                        {"quote_qty", "0.02"},
                        {"half_spread_bps", "7.25"},
                        {"stale_ms", "1500"}};
  REQUIRE_FALSE(a.apply(values).has_value());
  const ParamSchema& s = TestParams::schema();
  TestParams b;
  for (const ParamDesc& d : s) d.set_raw(&b, d.get_raw(&a));
  CHECK(b.describe() == a.describe());
  CHECK(s.find("levels")->get_raw(&a) == 3);
  CHECK(std::bit_cast<double>(s.find("gamma")->get_raw(&a)) == 2.5);
  CHECK(s.find("hedge")->get_raw(&a) == 1);
  CHECK(s.find("quote_qty")->get_raw(&a) == 2'000'000);
  CHECK(s.find("half_spread_bps")->get_raw(&a) == 72'500);
  CHECK(s.find("stale_ms")->get_raw(&a) == 1'500'000'000);
}

TEST_CASE("core.params: the publisher rejects invalid updates and refuses when the ring is full") {
  MsgRing ring(1U << 12);
  const TestParams initial;
  ParamPublisher pub(ParamSink::to_ring(ring), initial);
  const auto one = [&](const char* name, const char* value) {
    return pub.publish({{name, value}});
  };

  CHECK_THROWS_WITH_AS(one("nope", "1"), "unknown parameter 'nope'", std::invalid_argument);
  CHECK_THROWS_WITH_AS(
      one("levels", "9"), "parameter 'levels': value 9 outside [1, 8]", std::invalid_argument);
  CHECK_THROWS_WITH_AS(one("gamma", "abc"),
                       "parameter 'gamma': cannot parse 'abc' as double (a finite number)",
                       std::invalid_argument);
  const Values crossed{{"quote_qty", "0.5"}, {"max_inventory", "0.2"}};
  CHECK_THROWS_WITH_AS(
      pub.publish(crossed), "quote_qty must not exceed max_inventory", std::invalid_argument);
  const Values twice{{"levels", "2"}, {"levels", "3"}};
  CHECK_THROWS_WITH_AS(pub.publish(twice), "parameter 'levels' given twice", std::invalid_argument);
  const Values many(ParamUpdateMsg::kMaxFields + 1, {"levels", "2"});
  CHECK_THROWS_WITH_AS(
      pub.publish(many), "at most 32 parameters per update, got 33", std::invalid_argument);
  const Values levels{{"levels", "2"}};
  CHECK_THROWS_WITH_AS(pub.publish(levels, InstrumentId{0}),
                       "the strategy keeps one parameter set for all instruments; publish without "
                       "an instrument",
                       std::invalid_argument);
  CHECK(ring.empty_approx());
  CHECK(pub.published() == 0);

  // A valid update: one message with the schema index and raw value.
  REQUIRE(one("quote_qty", "0.05"));
  REQUIRE(front(ring) != nullptr);
  {
    const auto& m = msg_cast<ParamUpdateMsg>(front(ring));
    CHECK(m.hdr.type == EventType::ParamUpdate);
    CHECK(m.all_instruments());
    CHECK(m.publish_seq == 1);
    REQUIRE(m.count == 1);
    CHECK(m.field[0] == 3);
    CHECK(m.value[0] == 5'000'000);
  }
  // The copy follows the sent update: max_inventory may no longer go below quote_qty.
  CHECK_THROWS_WITH_AS(one("max_inventory", "0.04"),
                       "quote_qty must not exceed max_inventory",
                       std::invalid_argument);

  // Full ring: refused, and the refused value does not reach the copy.
  std::uint64_t sent = 1;
  while (one("levels", "2")) ++sent;
  CHECK(sent > 2);
  CHECK(pub.published() == sent);
  CHECK(pub.refused() == 1);
  CHECK_FALSE(one("levels", "5"));
  CHECK(pub.describe().find("levels=2") != std::string::npos);
  ring.release();
  CHECK(one("levels", "5"));
  CHECK(pub.describe().find("levels=5") != std::string::npos);

  pub.close();
  while (front(ring) != nullptr) ring.release();
  CHECK_FALSE(one("levels", "3"));

  // One parameter set per instrument.
  MsgRing ring2(1U << 12);
  ParamPublisher per(ParamSink::to_ring(ring2), initial, 2);
  CHECK(per.publish(levels, InstrumentId{1}));
  CHECK(msg_cast<ParamUpdateMsg>(front(ring2)).hdr.instrument == InstrumentId{1});
  CHECK_THROWS_WITH_AS(per.publish(levels, InstrumentId{2}),
                       "instrument 2 is not in the instrument table",
                       std::invalid_argument);
  const Values big_qty{{"quote_qty", "0.2"}};
  CHECK_THROWS_WITH_AS(per.publish(big_qty),
                       "instrument 0: quote_qty must not exceed max_inventory",
                       std::invalid_argument);
  CHECK(per.describe(InstrumentId{1}).find("levels=2") != std::string::npos);
  CHECK(per.describe(InstrumentId{0}).find("levels=1") != std::string::npos);
}

TEST_CASE(
    "core.params: an update from a feed ring applies at one event, is journaled, then on_params "
    "runs") {
  const InstrumentTable table = one_instrument();
  SimClock clock{Timestamp{seconds(1000).ns}};
  CountingTransport transport;
  MsgRing md_ring(1U << 16);
  MsgRing param_ring(1U << 16);
  MsgRing journal_ring(1U << 20);
  RingFeed feed;
  REQUIRE(feed.add_ring(&md_ring));
  REQUIRE(feed.add_ring(&param_ring));
  ParamSpy strategy;
  const ParamMap configured{{"half_spread_bps", "5"}};
  REQUIRE_FALSE(strategy.configure(configured).has_value());
  ParamPublisher pub(ParamSink::to_ring(param_ring), strategy.params());
  using E = Engine<ParamSpy, SimClock, CountingTransport, RingFeed>;
  auto engine =
      std::make_unique<E>(EngineConfig{}, table, clock, transport, feed, strategy, &journal_ring);
  engine->warm_up();
  engine->start();

  const Values values{{"half_spread_bps", "7.5"}, {"quote_qty", "0.02"}};
  REQUIRE(pub.publish(values));
  CHECK(strategy.params().half_spread_bps == 5_bps);  // the engine has not consumed it yet
  clock.advance(milliseconds(1));
  CHECK(engine->step() == 1);
  CHECK(strategy.params_calls == 1);
  CHECK(strategy.spread_seen == 7.5_bps);  // both values were in place when on_params ran
  CHECK(strategy.qty_seen == 0.02_qty);
  CHECK(engine->stats().param_updates == 1);

  bool journaled = false;
  while (const EventHeader* h = front(journal_ring)) {
    if (h->type == EventType::ParamUpdate) {
      journaled = true;
      CHECK((h->flags & EventHeader::kEngineTime) != 0);
      const auto& m = msg_cast<ParamUpdateMsg>(h);
      CHECK(m.count == 2);
      CHECK(m.value[0] == 75'000);
      CHECK(m.value[1] == 2'000'000);
    }
    journal_ring.release();
  }
  CHECK(journaled);
}

TEST_CASE(
    "core.params: max_param_age disables quoting before the first update and on an event past the "
    "deadline") {
  const InstrumentTable table = one_instrument();
  SimClock clock{Timestamp{seconds(1000).ns}};
  CountingTransport transport;
  InlineFeed feed{1U << 20};
  ParamSpy strategy;
  EngineConfig cfg;
  cfg.max_param_age = milliseconds(100);
  cfg.quotes.min_requote_interval = Duration{};
  using E = Engine<ParamSpy, SimClock, CountingTransport, InlineFeed>;
  auto engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
  engine->warm_up();
  engine->start();
  CHECK_FALSE(engine->quoting_enabled());
  CHECK(engine->params_stale());

  push_book(feed, clock, 100.00_px, 100.02_px);
  engine->step();
  REQUIRE(strategy.set_quotes_results.size() == 1);
  CHECK_FALSE(strategy.set_quotes_results[0]);  // no update yet
  CHECK(strategy.quoting.empty());              // the initial state is not reported

  ParamPublisher pub(ParamSink{}, strategy.params());
  ParamUpdateMsg update{};
  const Values none;
  REQUIRE_FALSE(pub.build(none, ParamPublisher::kAllInstruments, update).has_value());
  REQUIRE(feed.push(update.hdr));
  engine->step();
  CHECK(strategy.quoting == std::vector<bool>{true});
  push_book(feed, clock, 100.00_px, 100.04_px);
  engine->step();
  CHECK(strategy.set_quotes_results.back());
  CHECK(transport.news == 2);

  // An event past the deadline is consumed before the step polls the timers: the parameters expire
  // first, so its hook cannot quote, and the quotes are pulled.
  clock.advance(milliseconds(150));
  push_book(feed, clock, 100.02_px, 100.06_px);
  engine->step();
  CHECK_FALSE(strategy.set_quotes_results.back());
  CHECK(strategy.quoting == std::vector<bool>{true, false});
  CHECK(engine->stats().param_expiries == 1);

  REQUIRE(feed.push(update.hdr));
  engine->step();
  CHECK(strategy.quoting == std::vector<bool>{true, false, true});
  CHECK_FALSE(engine->params_stale());
}

TEST_CASE("core.journal v3: the parameter table round-trips and older files still open") {
  const auto path = (tmp_dir() / "v3_params.fmj").string();
  const InstrumentTable table = one_instrument();
  const std::string toml = "[engine]\nrng_seed = 1\n";
  MsgRing ring(1U << 16);
  {
    JournalSessionInfo info;
    info.strategy = "param_spy";
    info.instruments = &table;
    info.config_toml = toml;
    info.params = &TestParams::schema();
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    ParamUpdateMsg m{};
    init_header(m, EventType::ParamUpdate);
    m.count = 1;
    m.field[0] = 5;
    m.value[0] = 75'000;
    REQUIRE(w.record(m.hdr));
    fw.drain_once();
    fw.stop();
  }
  std::size_t table_bytes = 0;
  for (const ParamDesc& d : TestParams::schema())
    table_bytes += 2 + std::string_view(d.name).size();
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.version() == 3);
  CHECK(r.config_text() == toml);
  REQUIRE(r.params().size() == TestParams::schema().size());
  CHECK(r.params()[0].name == "levels");
  CHECK(r.params()[5].name == "half_spread_bps");
  CHECK(r.params()[5].type == static_cast<std::uint8_t>(ParamType::Bps));
  CHECK(r.params()[6].type == static_cast<std::uint8_t>(ParamType::Millis));
  CHECK(r.header().param_table_bytes == table_bytes);
  CHECK(r.header().header_bytes ==
        sizeof(JournalFileHeader) + sizeof(Instrument) + 64 + ((table_bytes + 63) / 64) * 64);
  REQUIRE(r.message_count() == 1);
  r.for_each([](const EventHeader* h) {
    CHECK(h->type == EventType::ParamUpdate);
    CHECK(msg_cast<ParamUpdateMsg>(h).value[0] == 75'000);
  });

  // A damaged table byte fails the table checksum.
  {
    const int fd = ::open(path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    const char x = 'X';
    const auto off = static_cast<off_t>(sizeof(JournalFileHeader) + sizeof(Instrument) + 64 + 2);
    REQUIRE(::pwrite(fd, &x, 1, off) == 1);
    ::close(fd);
  }
  JournalReader bad;
  CHECK(bad.open(path).error() == JournalError::HeaderCorrupt);

  // A version 2 file: no parameter table, whatever the v3 header bytes hold.
  const auto v2_path = (tmp_dir() / "v2_no_params.fmj").string();
  {
    JournalSessionInfo info;
    info.instruments = &table;
    info.config_toml = toml;
    MsgRing ring2(1U << 16);
    JournalFileWriter fw(ring2, v2_path, info);
    REQUIRE(fw.ok());
    fw.stop();
  }
  {
    JournalReader w;
    REQUIRE(w.open(v2_path));
    JournalFileHeader h = w.header();
    h.version = 2;
    h.param_count = 3;
    h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
    const int fd = ::open(v2_path.c_str(), O_WRONLY);
    REQUIRE(fd >= 0);
    REQUIRE(::pwrite(fd, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h));
    ::close(fd);
  }
  JournalReader v2;
  REQUIRE(v2.open(v2_path));
  CHECK(v2.version() == 2);
  CHECK(v2.params().empty());
  CHECK(v2.config_text() == toml);

  // The committed golden journal is format v1.
  JournalReader v1;
  REQUIRE(v1.open(FASTMM_FIXTURES_DIR "/journals/sample_1000.fmj"));
  CHECK(v1.version() == 1);
  CHECK(v1.params().empty());
  CHECK(v1.message_count() == 1000);
}
