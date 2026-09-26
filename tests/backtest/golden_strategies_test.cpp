// Golden outbound hashes for the built-in strategies on fixed seeds (ADR-0012, step 1).
//
// Each case runs one strategy through the template path run_backtest<S>() and pins the SHA-256 of
// the outbound stream. Strategy API changes must keep these hashes. A change that alters behaviour
// on purpose re-baselines them in a commit of its own that names the reason (the intended
// differences are listed in docs/adr/0012-strategy-developer-experience.md).
//
// FASTMM_PRINT_GOLDEN=1 prints the hashes, message and fill counts of the current code.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/options/black76.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/options_mm.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

void check_golden(const std::string& name, const BacktestResult& r, const std::string& expected) {
  const char* print = std::getenv("FASTMM_PRINT_GOLDEN");
  if (print != nullptr && std::string(print) == "1") {
    MESSAGE(name << " outbound_sha256 " << r.outbound_sha256 << " messages " << r.outbound_messages
                 << " fills " << r.metrics.fills);
  }
  CAPTURE(name);
  CHECK(r.outbound_sha256 == expected);
}

constexpr std::int64_t kStartNs = 1'789'344'931'096LL * 1'000'000;  // 2026-09-13T22:15:31Z
constexpr std::int64_t kDayNs = 86'400LL * 1'000'000'000;

// Two inverse BTC options (call and put, K = 77000) on venue 0.
InstrumentTable option_table() {
  InstrumentTable t;
  for (const OptionType type : {OptionType::Call, OptionType::Put}) {
    Instrument i{};
    i.symbol = type == OptionType::Call ? "BTC-13OCT26-77000-C" : "BTC-13OCT26-77000-P";
    i.venue = VenueId{0};
    i.asset_class = AssetClass::Option;
    i.option_type = type;
    i.strike = px("77000");
    i.expiry_ns = kStartNs + 30 * kDayNs;
    i.flags = Instrument::kEnabled | Instrument::kCoinQuoted;
    i.tick = px("0.0001");
    i.lot = qt("0.1");
    i.min_qty = qt("0.1");
    i.contract_multiplier = qt("1");
    REQUIRE(t.add(i));
  }
  return t;
}

// A scripted options market. Every 50 ms each option gets a two-sided snapshot five ticks either
// side of its Black-76 value and a venue ticker; every fourth step a trade prints through one side,
// which fills resting strategy quotes under the L2 queue model. The put's ticker pauses for two
// seconds in the middle, so the stale-ticker timer pulls its quotes.
class ScriptedOptionsSource final : public MdSource {
 public:
  explicit ScriptedOptionsSource(const InstrumentTable& table) : table_(table) {}

  const EventHeader* next() override {
    while (st_.step < kSteps) {
      const std::int64_t ts = kStartNs + static_cast<std::int64_t>(st_.step) * 50'000'000;
      const int step = st_.step;
      const auto id = InstrumentId{static_cast<std::uint32_t>(st_.inst)};
      const int phase = st_.phase;
      const Instrument& inst = table_.get(id);
      const std::int64_t tick = inst.tick.raw;
      const double t = options::year_fraction(inst.expiry_ns, ts);
      const options::CallPut cp =
          inst.option_type == OptionType::Put ? options::CallPut::Put : options::CallPut::Call;
      const double theo =
          options::black76(cp, st_.forward, inst.strike.to_double(), t, st_.iv).price / st_.forward;
      const std::int64_t mid_ticks = std::llround(theo / inst.tick.to_double());
      const Price bid = Price::from_raw((mid_ticks - 5) * tick);
      const Price ask = Price::from_raw((mid_ticks + 5) * tick);
      advance_cursor();
      if (phase == 0) {
        auto& d = buf_.as<BookDeltaMsg>();
        init_header(d, EventType::BookSnapshot, id, VenueId{0}, BookDeltaMsg::size_for(1, 1));
        d.hdr.flags |= EventHeader::kSnapshot;
        stamp(d.hdr, ts);
        d.bid_count = d.ask_count = 1;
        d.first_update_id = d.last_update_id = ++st_.seq;
        d.prev_update_id = 0;
        d.levels()[0] = Level{bid, qt("5")};
        d.levels()[1] = Level{ask, qt("5")};
        return &d.hdr;
      }
      if (phase == 1) {
        if (id.value == 1 && step >= 500 && step < 540) continue;  // the put's ticker pauses
        auto& m = buf_.as<OptionTickerMsg>();
        init_header(m, EventType::OptionTicker, id, VenueId{0});
        stamp(m.hdr, ts);
        m.mark_price = Price::from_raw(mid_ticks * tick);
        m.underlying_price = Price::from_double(st_.forward);
        m.index_price = m.underlying_price;
        m.mark_iv = st_.iv;
        m.bid_iv = st_.iv - 0.01;
        m.ask_iv = st_.iv + 0.01;
        m.interest_rate = 0.0;
        return &m.hdr;
      }
      if (step % 4 != 0) continue;
      auto& tr = buf_.as<TradeMsg>();
      init_header(tr, EventType::Trade, id, VenueId{0});
      stamp(tr.hdr, ts);
      const bool sell = (st_.rng.next() & 1U) != 0;
      tr.aggressor = sell ? Side::Sell : Side::Buy;
      tr.price = sell ? bid : ask;
      tr.qty = qt("1");
      tr.trade_id = ++st_.seq;
      return &tr.hdr;
    }
    return nullptr;
  }
  void reset() override { st_ = State{}; }
  [[nodiscard]] Timestamp start_ts() const override { return Timestamp{kStartNs}; }

 private:
  static constexpr int kSteps = 1200;  // 60 s
  struct State {
    int step = 0;
    int inst = 0;
    int phase = 0;  // 0 snapshot, 1 ticker, 2 trade
    std::uint64_t seq = 0;
    double forward = 76900.0;
    double iv = 0.5;
    Xoshiro256ss rng{2026};
  };

  static void stamp(EventHeader& h, std::int64_t ts) noexcept {
    h.exch_ts = Timestamp{ts};
    h.recv_ts = Timestamp{ts};
  }
  void advance_cursor() noexcept {
    if (++st_.phase < 3) return;
    st_.phase = 0;
    if (++st_.inst < 2) return;
    st_.inst = 0;
    ++st_.step;
    const std::uint64_t r = st_.rng.next();
    st_.forward += static_cast<double>(static_cast<int>(r % 5U) - 2) * 25.0;
    st_.iv += static_cast<double>(static_cast<int>((r >> 8U) % 3U) - 1) * 0.002;
    if (st_.iv < 0.3) st_.iv = 0.3;
    if (st_.iv > 0.7) st_.iv = 0.7;
  }

  const InstrumentTable& table_;
  State st_{};
  EventBuf buf_{};
};

}  // namespace

TEST_CASE("backtest.golden: BasicMM outbound hash, coupled matching market, two levels") {
  BacktestConfig cfg = synthetic_config(21, seconds(20));
  cfg.params["levels"] = "2";
  cfg.params["level_step_ticks"] = "2";
  const BacktestResult r = run_backtest<BasicMM>(cfg);
  REQUIRE(r.metrics.fills > 0);
  check_golden(
      "basic_mm/coupled", r, "a4c5af9990a2beeb5a83624670328153b468f8e17db75f8736a896e819644c2f");
}

TEST_CASE("backtest.golden: BasicMM outbound hash, L2 queue model with stale-book pulls") {
  BacktestConfig cfg = synthetic_config(22, seconds(15));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  cfg.params["pull_on_stale_ms"] = "20";
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.md = cfg.transport.md;
  sc.seed = 22;
  sc.duration = seconds(15);
  SyntheticSource src(sc);
  const BacktestResult r = run_backtest<BasicMM>(cfg, &src);
  REQUIRE(r.engine.timers_fired > 0);
  check_golden(
      "basic_mm/l2_queue", r, "ccca7a9994caae732092ece59323b4c3bd379c45b45db8dcce1a97c4a8253394");
}

TEST_CASE("backtest.golden: AvellanedaStoikov outbound hash on a fixed seed") {
  BacktestConfig cfg = synthetic_config(23, seconds(15));
  cfg.params = {{"gamma", "0.1"},
                {"kappa", "100"},
                {"estimate_kappa", "1"},
                {"infinite_horizon", "0"},
                {"horizon_s", "5"},
                {"quote_qty", "0.002"},
                {"max_inventory", "0.02"}};
  const BacktestResult r = run_backtest<AvellanedaStoikov>(cfg);
  REQUIRE(r.metrics.fills > 0);
  check_golden("avellaneda_stoikov/coupled",
               r,
               "baf2eb80251aed06d148c5a7f04f6a48928440f7c4e3a6fcef7e3f7a756b1cb8");
}

TEST_CASE("backtest.golden: OptionsMM outbound hash on a scripted options market") {
  BacktestConfig cfg;
  cfg.instruments = option_table();
  cfg.set_seed(24);
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.fees = sim::FeeModel::from_bps(0.0, 3.0);
  cfg.engine.quotes.min_requote_interval = milliseconds(50);
  cfg.engine.risk.max_open_orders = 8;
  cfg.params = {{"quote_qty", "0.1"},
                {"max_position", "0.5"},
                {"max_delta", "0.3"},
                {"delta_skew_ticks", "5"},
                {"inventory_skew_ticks", "1"},
                {"max_vega", "0"},
                {"pull_on_stale_ms", "1000"}};
  cfg.measure_wall_clock = false;
  ScriptedOptionsSource src(cfg.instruments);
  const BacktestResult r = run_backtest<OptionsMM>(cfg, &src);
  REQUIRE(r.metrics.fills > 0);
  REQUIRE(r.engine.timers_fired > 0);
  check_golden(
      "options_mm/scripted", r, "bc9b046d97880a8d092c25c6e4f664741ecb90a17a42d4f34efc35b5ed5c6317");
}
