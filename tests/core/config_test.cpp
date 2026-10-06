#include "fastmm/config/config.hpp"

#include "test_support.hpp"

#include "fastmm/config/env_subst.hpp"

#include <cstdlib>
#include <filesystem>

using namespace fastmm;

namespace {
std::filesystem::path configs_dir() {
  return fastmm::test::fixtures_dir().parent_path().parent_path() / "configs";
}

const char* kMinimal = R"(
[engine]
name = "t"
[venues.sim]
kind = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
tick = "0.01"
lot = 0.001
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 5
levels = 2
[risk]
max_position = "0.5"
max_open_orders = 4
)";
}  // namespace

TEST_CASE("core.config: shipped configs parse, validate and build instruments") {
  setenv("FASTMM_SIM_API_KEY", "k", 1);
  setenv("FASTMM_SIM_API_SECRET", "s", 1);
  for (const char* name : {"sim-local.toml", "backtest-example.toml"}) {
    CAPTURE(name);
    const Config cfg = Config::load((configs_dir() / name).string());
    CHECK(cfg.warnings.empty());
    CHECK(cfg.strategy.name == "basic_mm");
    REQUIRE(cfg.venues.size() == 1);
    REQUIRE(cfg.instruments.size() == 1);
    CHECK(cfg.instruments[0].tick == "0.01");
    const InstrumentTable t = load_instruments(cfg);
    REQUIRE(t.size() == 1);
    CHECK(t.get(InstrumentId{0}).tick == Price::from_decimal("0.01").value());
    CHECK(t.get(InstrumentId{0}).symbol == "BTCUSDT");
    CHECK(t.get(InstrumentId{0}).enabled());
    const RiskLimits l = cfg.risk_limits();
    CHECK(l.max_order_qty == Qty::from_decimal("0.01").value());
    CHECK(l.stp);
    const QuoteParams q = cfg.quote_params();
    CHECK(q.min_requote_ticks == cfg.engine.min_requote_ticks);
    CHECK(cfg.hash != 0);
  }
  const Config sim = Config::load((configs_dir() / "sim-local.toml").string());
  CHECK(sim.venues[0].api_key == "k");
  CHECK(sim.venues[0].api_secret == "s");
  CHECK(sim.venues[0].fees.maker_bps == doctest::Approx(1.0));
  CHECK(sim.sim.get_int("seed") == 7);
  CHECK(sim.sim.get_string("start_mid") == "60000");
  CHECK(sim.sim.get_double("p_drop") == doctest::Approx(0.0));
  CHECK(sim.log_level() == LogLevel::Info);
  CHECK(sim.spin_mode() == SpinMode::Adaptive);
  CHECK(sim.venue_id("sim") == VenueId{0});
  CHECK_FALSE(sim.venue_id("nope").valid());
  const Config bt = Config::load((configs_dir() / "backtest-example.toml").string());
  CHECK(bt.backtest.get_string("fill_model") == "l2_queue");
  CHECK(bt.backtest.get_double("queue_conservatism") == doctest::Approx(0.5));
  CHECK(bt.venues[0].fees.maker_bps == doctest::Approx(10.0));  // Binance spot VIP 0
  CHECK(bt.strategy.params.at("level_step_ticks") == "5");
  CHECK(bt.strategy.params.at("half_spread_bps") == "0.01");  // shortest round-trip form
}

TEST_CASE("core.config: minimal parse, float decimals stringified exactly, defaults") {
  const Config cfg = Config::parse(kMinimal);
  CHECK(cfg.instruments[0].lot == "0.001");
  CHECK(cfg.strategy.params.at("half_spread_bps") == "5");
  CHECK(cfg.strategy.params.at("levels") == "2");
  CHECK(cfg.engine.max_events_per_step == 64);
  CHECK(cfg.engine.feed_budget_per_ring == 64);
  CHECK_FALSE(cfg.engine.net_spin_dedicated);
  CHECK(cfg.risk.max_open_orders == 4);
  CHECK(cfg.risk_limits().max_position == Qty::from_decimal("0.5").value());
  const InstrumentTable t = load_instruments(cfg);
  CHECK(t.get(InstrumentId{0}).lot == Qty::from_decimal("0.001").value());
  CHECK(t.get(InstrumentId{0}).min_qty == Qty::from_decimal("0.001").value());  // defaults to lot
}

TEST_CASE("core.config: feed_budget_per_ring and net_spin_dedicated") {
  const Config cfg =
      Config::parse("[engine]\nfeed_budget_per_ring = 1\nnet_spin_dedicated = true\n");
  CHECK(cfg.engine.feed_budget_per_ring == 1);
  CHECK(cfg.engine.net_spin_dedicated);
  CHECK(cfg.effective_toml().find("feed_budget_per_ring = 1") != std::string::npos);
  CHECK(cfg.effective_toml().find("net_spin_dedicated = true") != std::string::npos);
  CHECK_THROWS_WITH_AS(Config::parse("[engine]\nfeed_budget_per_ring = 0\n"),
                       doctest::Contains("feed_budget_per_ring must be >= 1"),
                       ConfigError);
}

TEST_CASE("core.config: instance_lock, lock_file and handoff_timeout_ms") {
  const Config defaults = Config::parse("[engine]\n");
  CHECK_FALSE(defaults.engine.instance_lock);
  CHECK(defaults.engine.lock_file.empty());
  CHECK(defaults.engine.handoff_timeout_ms == 75'000);
  const Config cfg = Config::parse(
      "[engine]\ninstance_lock = true\nlock_file = \"/run/mm.lock\"\nhandoff_timeout_ms = 5000\n");
  CHECK(cfg.engine.instance_lock);
  CHECK(cfg.engine.lock_file == "/run/mm.lock");
  CHECK(cfg.engine.handoff_timeout_ms == 5000);
  CHECK(cfg.effective_toml().find("instance_lock = true") != std::string::npos);
  CHECK(cfg.effective_toml().find("handoff_timeout_ms = 5000") != std::string::npos);
  CHECK_THROWS_WITH_AS(Config::parse("[engine]\nhandoff_timeout_ms = 0\n"),
                       doctest::Contains("handoff_timeout_ms must be > 0"),
                       ConfigError);
}

TEST_CASE("core.config: env substitution only in venues, missing var is an error") {
  setenv("FASTMM_T_URL", "wss://x", 1);
  unsetenv("FASTMM_MISSING");
  const Config ok = Config::parse(R"(
[venues.v]
kind = "bybit"
ws_url = "${FASTMM_T_URL}/public"
[strategy]
name = "${FASTMM_T_URL}"
)");
  CHECK(ok.venues[0].ws_url == "wss://x/public");
  CHECK(ok.strategy.name == "${FASTMM_T_URL}");  // not substituted outside [venues.*]
  CHECK_THROWS_WITH_AS(
      Config::parse("[venues.v]\nkind = \"bybit\"\napi_key = \"${FASTMM_MISSING}\"\n"),
      doctest::Contains("FASTMM_MISSING"),
      ConfigError);
  Config::LoadOptions no_env;
  no_env.substitute_env = false;
  const Config raw =
      Config::parse("[venues.v]\nkind = \"bybit\"\nws_url = \"${FASTMM_T_URL}\"\n", no_env);
  CHECK(raw.venues[0].ws_url == "${FASTMM_T_URL}");
  CHECK(substitute_env("a${FASTMM_T_URL}b").value() == "awss://xb");
  CHECK(substitute_env("${").error().find("unterminated") != std::string::npos);
  CHECK(has_env_reference("${X}"));
  CHECK_FALSE(has_env_reference("plain"));
}

TEST_CASE("core.config: inline secret guard and redaction") {
  const std::string inline_secret =
      "[venues.v]\nkind = \"binance_spot\"\napi_secret = "
      "\"0123456789abcdef0123456789abcdef0123456789\"\n";
  CHECK_THROWS_WITH_AS(
      Config::parse(inline_secret), doctest::Contains("inline secret"), ConfigError);
  Config::LoadOptions allow;
  allow.allow_inline_secrets = true;
  const Config cfg = Config::parse(inline_secret, allow);
  CHECK(cfg.venues[0].api_secret.size() == 42);
  const std::string dump = cfg.redacted();
  CHECK(dump.find("0123456789abcdef") == std::string::npos);
  CHECK(dump.find("api_secret = \"***\"") != std::string::npos);
  CHECK(dump.find("[venues.v]") != std::string::npos);
  // short values are not treated as secrets
  const Config short_ok = Config::parse("[venues.v]\nkind = \"sim\"\napi_key = \"short\"\n");
  CHECK(short_ok.venues[0].api_key == "short");
}

TEST_CASE("core.config: api_passphrase is a credential: substituted, masked, not journaled") {
  setenv("FASTMM_T_PASS", "pass-phrase-1", 1);
  const Config cfg = Config::parse(
      "[venues.o]\nkind = \"okx\"\napi_key = \"k\"\napi_passphrase = \"${FASTMM_T_PASS}\"\n");
  REQUIRE(cfg.venues.size() == 1);
  CHECK(cfg.venues[0].api_passphrase == "pass-phrase-1");
  CHECK(cfg.venues[0].extra.count("api_passphrase") == 0);  // generic, not the connector's
  const std::string dump = cfg.redacted();
  CHECK(dump.find("pass-phrase-1") == std::string::npos);
  CHECK(dump.find("api_passphrase = \"***\"") != std::string::npos);
  CHECK(cfg.effective_toml().find("pass-phrase-1") == std::string::npos);
  CHECK(cfg.effective_toml().find("api_passphrase") == std::string::npos);
  CHECK_THROWS_WITH_AS(Config::parse("[venues.o]\nkind = \"okx\"\napi_passphrase = "
                                     "\"0123456789abcdef0123456789abcdef0123\"\n"),
                       doctest::Contains("inline secret"),
                       ConfigError);
}

TEST_CASE("core.config: [strategy] state_file and state_interval_s round-trip") {
  const Config plain = Config::parse("[strategy]\nname = \"basic_mm\"\n");
  CHECK(plain.strategy.state_file.empty());
  CHECK(plain.strategy.state_interval_s == 300);
  CHECK(plain.effective_toml().find("state_") == std::string::npos);
  const Config cfg = Config::parse(
      "[strategy]\nname = \"basic_mm\"\nstate_file = \"runs/mm.state\"\nstate_interval_s = 60\n");
  CHECK(cfg.strategy.state_file == "runs/mm.state");
  CHECK(cfg.strategy.state_interval_s == 60);
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.strategy.state_file == "runs/mm.state");
  CHECK(again.strategy.state_interval_s == 60);
  CHECK_THROWS_WITH_AS(Config::parse("[strategy]\nname = \"basic_mm\"\nstate_interval_s = 0\n"),
                       doctest::Contains("state_interval_s must be > 0"),
                       ConfigError);
  CHECK(plain.strategy.state_snapshot_interval_s == 0);
  const Config snap =
      Config::parse("[strategy]\nname = \"basic_mm\"\nstate_snapshot_interval_s = 30\n");
  CHECK(Config::parse(snap.effective_toml()).strategy.state_snapshot_interval_s == 30);
  CHECK_THROWS_WITH_AS(
      Config::parse("[strategy]\nname = \"basic_mm\"\nstate_snapshot_interval_s = -1\n"),
      doctest::Contains("state_snapshot_interval_s must be >= 0"),
      ConfigError);
}

TEST_CASE("core.config: validation errors carry line numbers, unknown keys are errors") {
  auto line_of = [](const char* toml) {
    try {
      Config::parse(toml);
    } catch (const ConfigError& e) {
      return e.line();
    }
    return -1;
  };
  CHECK(line_of("[engine]\nname = 5\n") == 2);          // wrong type
  CHECK(line_of("[venues.v]\nws_url = \"x\"\n") == 1);  // missing required kind
  CHECK(line_of("[venues.v]\nkind = \"sim\"\n[[instruments]]\nvenue = \"nope\"\nsymbol = "
                "\"X\"\ntick = \"1\"\nlot = \"1\"\n") == 4);
  CHECK(line_of("[engine]\nspin_mode = \"turbo\"\n") == 2);
  CHECK(line_of("[engine\n") == 1);                         // syntax error
  CHECK(line_of("[engine]\nmd_ring_bytes = 1000\n") == 1);  // not a power of two
  CHECK(line_of("[logging]\nlevel = \"loud\"\n") == 0);     // semantic error without position
  CHECK_THROWS_AS(Config::parse("[logging]\nlevel = \"loud\"\n"), ConfigError);
  CHECK_THROWS_AS(Config::load("/nonexistent/file.toml"), ConfigError);
  // An unknown key or section is an error with its line: a misspelled key would leave the real
  // one at its default.
  CHECK(line_of("[engine]\nbogus = 1\n") == 2);
  CHECK(line_of("[weird]\nx = 1\n") == 1);
  CHECK_THROWS_WITH_AS(Config::parse("[engine]\nbogus = 1\n"),
                       doctest::Contains("unknown key 'engine.bogus'"),
                       ConfigError);
  // `venues.v.foo` belongs to the connector, which checks it when it validates the section
  // (fastmm::venues::validate_venues).
  const Config w = Config::parse("[venues.v]\nkind = \"sim\"\nfoo = \"bar\"\n");
  CHECK(w.warnings.empty());
  CHECK(w.venues[0].extra.at("foo") == "bar");
  // instrument loader errors
  CHECK_THROWS_WITH_AS(
      load_instruments(Config::parse("[venues.v]\nkind = \"sim\"\n[[instruments]]\nvenue = "
                                     "\"v\"\nsymbol = \"X\"\ntick = \"0\"\nlot = \"1\"\n")),
      doctest::Contains("tick must be > 0"),
      ConfigError);
  CHECK_THROWS_WITH_AS(
      load_instruments(Config::parse("[venues.v]\nkind = \"sim\"\n[[instruments]]\nvenue = "
                                     "\"v\"\nsymbol = \"X\"\ntick = \"abc\"\nlot = \"1\"\n")),
      doctest::Contains("not a valid decimal"),
      ConfigError);
  CHECK_THROWS_WITH_AS(load_instruments(Config::parse(
                           "[venues.v]\nkind = \"sim\"\n[[instruments]]\nvenue = \"v\"\nsymbol = "
                           "\"X\"\ntick = \"1\"\nlot = \"1\"\n[[instruments]]\nvenue = "
                           "\"v\"\nsymbol = \"X\"\ntick = \"1\"\nlot = \"1\"\n")),
                       doctest::Contains("duplicate symbol"),
                       ConfigError);
  const Config opt = Config::parse(
      "[venues.v]\nkind = \"sim\"\n[[instruments]]\nvenue = \"v\"\nsymbol = "
      "\"BTC-27DEC24-100000-C\"\ntick = \"0.5\"\nlot = \"1\"\nasset_class = \"option\"\nstrike = "
      "100000\noption_type = \"call\"\nexpiry = \"2024-12-27T08:00:00Z\"\ncontract_multiplier = "
      "\"0.01\"\n");
  const InstrumentTable t = load_instruments(opt);
  CHECK(t.get(InstrumentId{0}).option_type == OptionType::Call);
  CHECK(t.get(InstrumentId{0}).strike == Price::from_int(100000));
  CHECK(t.get(InstrumentId{0}).expiry_ns == 1735286400LL * 1'000'000'000LL);
  CHECK(t.get(InstrumentId{0}).contract_multiplier == Qty::from_decimal("0.01").value());
  CHECK(t.get(InstrumentId{0}).is_derivative());
}

TEST_CASE("core.config: venue keys the generic parser does not read are kept for their connector") {
  const Config cfg = Config::parse(R"(
[venues.b]
kind = "binance_spot"
stale_ms = 10000
order_api = "rest"
cancel_on_order_channel_loss = false
not_a_real_key = 1

[[instruments]]
venue = "b"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.00001"
)");
  const VenueSection& v = cfg.venues.at(0);
  CHECK(v.extra.at("stale_ms") == "10000");
  CHECK(v.extra.at("order_api") == "rest");
  CHECK(v.extra.at("cancel_on_order_channel_loss") == "false");
  CHECK(v.extra.count("not_a_real_key") == 1);
  // The line of every one of them, so the connector can report it the way the schema does.
  CHECK(v.extra_lines.at("stale_ms") == 4);
  CHECK(v.extra_lines.at("not_a_real_key") == 7);
  // The central schema knows none of these keys, so it neither warns nor type-checks: the venue
  // does both (tests/venues/registry_test.cpp).
  CHECK(cfg.warnings.empty());
  CHECK_NOTHROW(static_cast<void>(
      Config::parse("[venues.b]\nkind = \"binance_spot\"\nstale_ms = \"soon\"\n")));
}

TEST_CASE("core.config: shipped venue configs load without warnings") {
  ConfigLoadOptions opts;
  opts.substitute_env = false;  // keep ${FASTMM_*} references; no secrets needed to validate
  for (const char* name : {"binance-testnet.toml",
                           "bybit-testnet.toml",
                           "deribit-testnet.toml",
                           "gemini-sandbox.toml",
                           "okx-demo.toml",
                           "sim-local.toml",
                           "sim-local-tls.toml"}) {
    const Config cfg = Config::load((configs_dir() / name).string(), opts);
    INFO(name);
    for (const auto& w : cfg.warnings) MESSAGE(w);
    CHECK(cfg.warnings.empty());
  }
}

TEST_CASE("core.config: engine tsc_recalibrate_s defaults to 10, 0 disables, negative rejected") {
  CHECK(Config::parse("[engine]\nname = \"x\"\n").engine.tsc_recalibrate_s == 10);
  CHECK(Config::parse("[engine]\ntsc_recalibrate_s = 0\n").engine.tsc_recalibrate_s == 0);
  CHECK(Config::parse("[engine]\ntsc_recalibrate_s = 3\n").warnings.empty());
  CHECK_THROWS_AS(Config::parse("[engine]\ntsc_recalibrate_s = -1\n"), ConfigError);
}

TEST_CASE("config: engine net_backend selects the network reactor") {
  CHECK(Config::parse("[engine]\n").engine.net_backend == "epoll");
  const Config c = Config::parse("[engine]\nnet_backend = \"io_uring\"\n");
  CHECK(c.engine.net_backend == "io_uring");
  CHECK(c.warnings.empty());
  CHECK(c.redacted().find("net_backend = \"io_uring\"") != std::string::npos);
  int line = -1;
  try {
    static_cast<void>(Config::parse("[engine]\nnet_backend = \"kqueue\"\n"));
  } catch (const ConfigError& e) {
    line = e.line();
  }
  CHECK(line == 2);
}

TEST_CASE("core.config: [engine] on_kill defaults to exit, accepts stay and rejects other values") {
  CHECK(Config::parse("[engine]\nname = \"x\"\n").engine.on_kill == "exit");
  const Config stay = Config::parse("[engine]\non_kill = \"stay\"\n");
  CHECK(stay.engine.on_kill == "stay");
  CHECK(stay.warnings.empty());
  // The effective configuration (journal header) carries it and round-trips.
  CHECK(Config::parse(stay.effective_toml()).engine.on_kill == "stay");
  CHECK(stay.effective_hash() != Config::parse("[engine]\n").effective_hash());
  CHECK_THROWS_WITH_AS(Config::parse("[engine]\non_kill = \"restart\"\n"),
                       doctest::Contains("on_kill must be exit|stay"),
                       ConfigError);
}

TEST_CASE("core.config: [engine] threading is split or single, and single takes one venue") {
  CHECK(Config::parse("[engine]\n").engine.threading == "split");
  CHECK_FALSE(Config::parse("[engine]\n").single_threaded());
  const Config single = Config::parse("[engine]\nthreading = \"single\"\n");
  CHECK(single.single_threaded());
  CHECK(single.warnings.empty());
  CHECK(Config::parse(single.effective_toml()).single_threaded());
  CHECK_THROWS_WITH_AS(Config::parse("[engine]\nthreading = \"inline\"\n"),
                       doctest::Contains("threading must be split|single"),
                       ConfigError);
  const char* two = R"([engine]
threading = "single"
[venues.a]
kind = "binance"
[venues.b]
kind = "binance"
)";
  CHECK_THROWS_WITH_AS(
      Config::parse(two), doctest::Contains("threading = \"single\" runs one venue"), ConfigError);
}

namespace {
// ETHBTC settles in BTC, BTCUSDT in USDT; `accounting` is appended as it is.
std::string two_currencies(const std::string& accounting, const std::string& risk = "") {
  return R"([venues.sim]
kind = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "sim"
symbol = "ETHBTC"
base = "ETH"
quote = "BTC"
tick = "0.00001"
lot = "0.001"
[strategy]
name = "basic_mm"
[risk]
)" + risk +
         "\n" + accounting;
}
std::string plan_error(const Config& cfg) {
  std::string warning;
  const std::vector<std::string> venues{"sim"};
  const auto p = session_fx_plan(load_instruments(cfg),
                                 cfg.accounting,
                                 venues,
                                 false,
                                 cfg.risk_limits().reads_totals(),
                                 &warning);
  return p ? warning : p.error();
}
}  // namespace

TEST_CASE("core.config: [accounting] parses, round-trips and prices each currency") {
  const Config cfg = Config::parse(two_currencies(
      "[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nBTC = \"sim:BTCUSDT\"\n"));
  CHECK(cfg.warnings.empty());
  CHECK(cfg.accounting.reporting_currency == "USDT");
  CHECK(cfg.accounting.fx.at("BTC") == "sim:BTCUSDT");
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.accounting.fx == cfg.accounting.fx);
  CHECK(again.effective_hash() == cfg.effective_hash());
  CHECK(cfg.redacted().find("BTC = \"sim:BTCUSDT\"") != std::string::npos);
  const std::vector<std::string> venues{"sim"};
  const auto plan = build_fx_plan(load_instruments(cfg), cfg.accounting, venues);
  REQUIRE(plan.has_value());
  CHECK(plan->active());
  CHECK(plan->ccy[1] == 1);  // ETHBTC in BTC
  CHECK(plan->sources[1].instrument == InstrumentId{0});
  CHECK(plan->stale == seconds(60));  // stale_fx_ms by default
}

TEST_CASE("core.config: [accounting] stale_fx_ms sets the FX rates' age limit and round-trips") {
  const std::string text = two_currencies(
      "[accounting]\nreporting_currency = \"USDT\"\nstale_fx_ms = 0\n[accounting.fx]\nBTC = "
      "\"sim:BTCUSDT\"\n");
  const Config cfg = Config::parse(text);
  CHECK(cfg.warnings.empty());
  CHECK(cfg.accounting.stale_fx_ms == 0);
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.accounting.stale_fx_ms == 0);
  CHECK(again.effective_hash() == cfg.effective_hash());
  const std::vector<std::string> venues{"sim"};
  const auto plan = build_fx_plan(load_instruments(cfg), cfg.accounting, venues);
  REQUIRE(plan.has_value());
  CHECK(plan->stale.ns == 0);
  CHECK_THROWS_AS(static_cast<void>(Config::parse(two_currencies(
                      "[accounting]\nreporting_currency = \"USDT\"\nstale_fx_ms = -1\n"))),
                  ConfigError);
}

TEST_CASE("core.config: a single-currency configuration needs no [accounting] and is unchanged") {
  const Config cfg = Config::parse(kMinimal);
  CHECK_FALSE(cfg.accounting.configured());
  CHECK(cfg.effective_toml().find("accounting") == std::string::npos);
  CHECK(cfg.redacted().find("accounting") == std::string::npos);
  const std::vector<std::string> venues{"sim"};
  const auto plan = build_fx_plan(load_instruments(cfg), cfg.accounting, venues);
  REQUIRE(plan.has_value());
  CHECK_FALSE(plan->active());
}

TEST_CASE("core.config: [accounting] sources are checked at load") {
  const auto err = [](const std::string& text) -> std::string {
    try {
      static_cast<void>(Config::parse(text));
    } catch (const ConfigError& e) {
      return e.what();
    }
    return "";
  };
  CHECK(err(two_currencies("[accounting.fx]\nBTC = \"sim:BTCUSDT\"\n"))
            .find("needs [accounting] reporting_currency") != std::string::npos);
  CHECK(
      err(two_currencies(
              "[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nBTC = \"sim:XYZ\"\n"))
          .find("XYZ is not in [[instruments]]") != std::string::npos);
  CHECK(err(two_currencies("[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nBTC = "
                           "\"other:BTCUSDT\"\n"))
            .find("unknown venue 'other'") != std::string::npos);
  CHECK(
      err(two_currencies(
              "[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nBTC = \"BTCUSDT\"\n"))
          .find("expected \"venue:symbol\"") != std::string::npos);
  CHECK(err(two_currencies("[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nUSDT = "
                           "\"sim:BTCUSDT\"\n"))
            .find("needs no source") != std::string::npos);
  CHECK(err(two_currencies("[accounting]\nreporting_currency = \"TOOLONGCCY\"\n"))
            .find("1 to 8 characters") != std::string::npos);
}

TEST_CASE("core.config: a settlement currency without a source is refused while a limit reads it") {
  const std::string no_source = "[accounting]\nreporting_currency = \"USDT\"\n";
  for (const char* limit :
       {"max_loss = \"100\"", "max_gross_notional = \"100\"", "max_net_notional = \"100\""}) {
    INFO(limit);
    const std::string e = plan_error(Config::parse(two_currencies(no_source, limit)));
    CHECK(e.find("ETHBTC settles in BTC and [accounting.fx] has no source for BTC") !=
          std::string::npos);
  }
  // Without one the totals are only reported: a warning, and nothing converts.
  CHECK(plan_error(Config::parse(two_currencies(no_source))).find("not converted") !=
        std::string::npos);
}

TEST_CASE("core.config: [risk.underlying] and [gateway.underlying] parse and round-trip") {
  const Config cfg = Config::parse(two_currencies(
      "[risk.underlying.BTC]\nmax_net = 0.5\n[risk.underlying.eth]\nmax_net = \"0\"\n"
      "[gateway.underlying.BTC]\nmax_net = \"2\"\n"));
  CHECK(cfg.warnings.empty());
  CHECK(cfg.risk.underlying.max_net.at("BTC") == "0.5");
  CHECK(cfg.risk.underlying.max_net.at("eth") == "0");
  CHECK(cfg.gateway.underlying.max_net.at("BTC") == "2");
  CHECK(cfg.gateway.any());
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.risk.underlying.max_net == cfg.risk.underlying.max_net);
  CHECK(again.gateway.underlying.max_net == cfg.gateway.underlying.max_net);
  CHECK(again.effective_hash() == cfg.effective_hash());
  CHECK(cfg.redacted().find("[risk.underlying.BTC]\nmax_net = \"0.5\"") != std::string::npos);
  const auto plan = build_underlying_plan(load_instruments(cfg), cfg.risk.underlying);
  REQUIRE(plan.has_value());
  CHECK(plan->count == 2);
  CHECK(plan->max_net[static_cast<std::size_t>(plan->find("BTC"))] ==
        Qty::from_decimal("0.5").value());

  // Absent, nothing about it is in the effective configuration: its text and hash are unchanged.
  const Config plain = Config::parse(kMinimal);
  CHECK_FALSE(plain.risk.underlying.configured());
  CHECK(plain.effective_toml().find("underlying") == std::string::npos);
  CHECK(plain.redacted().find("underlying") == std::string::npos);
}

TEST_CASE("core.config: [risk.underlying] errors") {
  const auto err = [](const std::string& text) -> std::string {
    try {
      static_cast<void>(Config::parse(two_currencies(text)));
    } catch (const ConfigError& e) {
      return e.what();
    }
    return "";
  };
  CHECK(err("[risk.underlying]\nBTC = 0.5\n").find("risk.underlying.BTC must be a table") !=
        std::string::npos);
  CHECK(err("[risk.underlying.BTC]\nmax_position = 1\n")
            .find("[risk.underlying.BTC] needs max_net") != std::string::npos);
  CHECK(err("[risk.underlying.BTC]\nmax_net = -1\n").find("not a non-negative decimal") !=
        std::string::npos);
  CHECK(err("[risk.underlying.BTC]\nmax_net = \"lots\"\n").find("'lots' is not") !=
        std::string::npos);
  CHECK(err("[risk.underlying.BTC]\nmax_net = 0.000000001\n").find("not a non-negative decimal") !=
        std::string::npos);
  CHECK(err("[risk.underlying.TOOLONGNAME]\nmax_net = 1\n").find("1 to 8 characters") !=
        std::string::npos);
  CHECK(err("[risk.underlying.BTC]\nmax_net = 1\n[risk.underlying.btc]\nmax_net = 2\n")
            .find("named already") != std::string::npos);
  std::string nine;
  for (int i = 0; i < 9; ++i) nine += "[risk.underlying.C" + std::to_string(i) + "]\nmax_net = 1\n";
  CHECK(err(nine).find("at most 8 base assets") != std::string::npos);
  CHECK(err("[gateway.underlying.BTC]\nmax_net = -2\n").find("gateway.underlying.BTC.max_net") !=
        std::string::npos);
  // `underlying` as a plain key of [risk] (two_currencies' [risk] is open: the risk text goes
  // there).
  bool threw = false;
  try {
    static_cast<void>(Config::parse(two_currencies("", "underlying = 1")));
  } catch (const ConfigError& e) {
    threw = true;
    CHECK(std::string(e.what()).find("'risk.underlying' has the wrong type (expected table)") !=
          std::string::npos);
  }
  CHECK(threw);

  // An unknown key is an error, as in every other section.
  CHECK(err("[risk.underlying.BTC]\nmax_net = 1\nmax_gross = 2\n")
            .find("unknown key 'risk.underlying.*.max_gross'") != std::string::npos);

  // A base asset no instrument has is refused once the table is known.
  const Config sol = Config::parse(two_currencies("[risk.underlying.SOL]\nmax_net = 1\n"));
  const auto plan = build_underlying_plan(load_instruments(sol), sol.risk.underlying);
  REQUIRE_FALSE(plan.has_value());
  CHECK(plan.error() == "[risk.underlying.SOL]: no instrument of this session has base SOL");
}

TEST_CASE("core.config: [gateway.shared] parses, round-trips and names instruments it has") {
  const Config cfg = Config::parse(two_currencies(
      "[gateway.shared.\"sim:BTCUSDT\"]\nprimary = \"mm-a\"\n[gateway.shared.\"sim:ETHBTC\"]\n"));
  CHECK(cfg.warnings.empty());
  REQUIRE(cfg.gateway.shared.size() == 2);
  CHECK(cfg.gateway.shared.at("sim:BTCUSDT") == "mm-a");
  CHECK(cfg.gateway.shared.at("sim:ETHBTC").empty());
  CHECK(cfg.gateway.any());
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.gateway.shared == cfg.gateway.shared);
  CHECK(again.effective_hash() == cfg.effective_hash());
  CHECK(cfg.redacted().find("[gateway.shared.\"sim:BTCUSDT\"]\nprimary = \"mm-a\"") !=
        std::string::npos);
  CHECK(Config::parse(kMinimal).effective_toml().find("shared") == std::string::npos);

  const auto err = [](const std::string& text) -> std::string {
    try {
      static_cast<void>(Config::parse(two_currencies(text)));
    } catch (const ConfigError& e) {
      return e.what();
    }
    return "";
  };
  CHECK(err("[gateway.shared]\n\"sim:BTCUSDT\" = \"mm-a\"\n").find("must be a table") !=
        std::string::npos);
  CHECK(err("[gateway.shared.BTCUSDT]\n").find("expected \"venue:symbol\"") != std::string::npos);
  CHECK(err("[gateway.shared.\"other:BTCUSDT\"]\n").find("unknown venue 'other'") !=
        std::string::npos);
  CHECK(err("[gateway.shared.\"sim:SOLUSDT\"]\n").find("SOLUSDT is not in [[instruments]]") !=
        std::string::npos);
  CHECK(err("[gateway.shared.\"sim:BTCUSDT\"]\nprimary = 3\n").find("wrong type") !=
        std::string::npos);
  CHECK(err("[gateway.shared.\"sim:BTCUSDT\"]\nowner = \"mm-a\"\n")
            .find("unknown key 'gateway.shared.*.owner'") != std::string::npos);
}

// ---- account pools ([venues.<x>] pool_of, core/account_pool.hpp) --------------------------------

namespace {
// A primary with one instrument and the member `tail` appended.
std::string pool_toml(const char* tail) {
  return std::string(R"(
[venues.binance]
kind = "sim"
[venues.binance_b]
kind = "sim"
pool_of = "binance"
[[instruments]]
venue = "binance"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.001"
)") + tail;
}
}  // namespace

TEST_CASE("core.config: a pool member names its primary and takes no instruments of its own") {
  const Config cfg = Config::parse(pool_toml(""));
  REQUIRE(cfg.venues.size() == 2);
  CHECK(cfg.venues[1].pool_of == "binance");
  const PoolPlan plan = cfg.pool_plan();
  CHECK(plan.active);
  CHECK(plan.primary(VenueId{1}) == VenueId{0});
  CHECK(plan.primary(VenueId{0}) == VenueId{0});
  CHECK(plan.is_member(VenueId{1}));
  CHECK_FALSE(plan.is_member(VenueId{0}));
  CHECK(plan.pooled(VenueId{0}));
  CHECK(plan.pooled(VenueId{1}));
  const PoolMembers m = plan.members(VenueId{0});
  REQUIRE(m.size() == 2);
  CHECK(m[0] == VenueId{0});
  CHECK(m[1] == VenueId{1});
  CHECK(plan.admits(VenueId{0}, VenueId{1}));
  CHECK_FALSE(plan.admits(VenueId{1}, VenueId{0}));
  CHECK(plan.members(VenueId{1}).size() == 1);
  // The member stays a member through the effective configuration (journals, replay).
  const Config back = Config::parse(cfg.effective_toml());
  CHECK(back.venues[1].pool_of == "binance");
  CHECK(back.effective_toml() == cfg.effective_toml());
  CHECK(cfg.redacted().find("pool_of = \"binance\"") != std::string::npos);
  // A configuration without pools has an inactive plan.
  CHECK_FALSE(Config::parse(kMinimal).pool_plan().active);
  // The instrument table is the primary's alone.
  const InstrumentTable t = load_instruments(cfg);
  REQUIRE(t.size() == 1);
  CHECK(t.get(InstrumentId{0}).venue == VenueId{0});
}

TEST_CASE("core.config: pool members are checked against their primary") {
  // An instrument on a member.
  CHECK_THROWS_WITH_AS(Config::parse(pool_toml("[[instruments]]\nvenue = \"binance_b\"\n"
                                               "symbol = \"ETHUSDT\"\ntick = \"0.01\"\n"
                                               "lot = \"0.001\"\n")),
                       doctest::Contains("venues.binance_b: a pool member has no instruments"),
                       ConfigError);
  // A member of a member.
  CHECK_THROWS_WITH_AS(
      Config::parse(pool_toml("[venues.binance_c]\nkind = \"sim\"\npool_of = \"binance_b\"\n")),
      doctest::Contains("'binance_b' is itself a member of pool 'binance'"),
      ConfigError);
  // Another kind.
  CHECK_THROWS_WITH_AS(
      Config::parse(pool_toml("[venues.okx_b]\nkind = \"okx\"\npool_of = \"binance\"\n")),
      doctest::Contains("kind 'okx' differs from the primary's 'sim'"),
      ConfigError);
  // Market data only.
  CHECK_THROWS_WITH_AS(Config::parse(pool_toml("[venues.binance_c]\nkind = \"sim\"\npool_of = "
                                               "\"binance\"\npublic_only = true\n")),
                       doctest::Contains("venues.binance_c: a pool member takes orders"),
                       ConfigError);
  // Its own primary, and one that does not exist.
  CHECK_THROWS_WITH_AS(
      Config::parse(pool_toml("[venues.binance_c]\nkind = \"sim\"\npool_of = \"binance_c\"\n")),
      doctest::Contains("a venue cannot be its own primary"),
      ConfigError);
  CHECK_THROWS_WITH_AS(
      Config::parse(pool_toml("[venues.binance_c]\nkind = \"sim\"\npool_of = \"nowhere\"\n")),
      doctest::Contains("venues.binance_c.pool_of: unknown venue 'nowhere'"),
      ConfigError);
  // The error carries the line of the key.
  try {
    static_cast<void>(
        Config::parse(pool_toml("[venues.okx_b]\nkind = \"okx\"\npool_of = \"binance\"\n")));
    FAIL("a member of another kind passed");
  } catch (const ConfigError& e) {
    CHECK(e.line() == 14);
  }
}

TEST_CASE("core.config: [venues.<primary>.treasury] is typed, defaulted and round-trips") {
  const Config cfg = Config::parse(pool_toml(R"(
[engine]
name = "mm"
journal_dir = "runs"
[venues.binance.treasury]
enabled = true
asset = "USDT"
weights = { binance = 3, binance_b = 1 }
min_free = { binance_b = "250.5" }
threshold = 0.1
max_amount = 1000
step = "1"
max_per_hour = 4
)"));
  const TreasurySection& s = cfg.venues[0].treasury;
  CHECK(s.configured);
  CHECK(s.enabled);
  CHECK_FALSE(cfg.venues[1].treasury.configured);
  const TreasuryConfig t = cfg.treasury_config("binance");
  CHECK(t.enabled);
  CHECK_FALSE(t.dry_run);
  CHECK(t.asset == "USDT");
  REQUIRE(t.members.size() == 2);
  CHECK(t.names[0] == "binance");
  CHECK(t.names[1] == "binance_b");
  CHECK(t.weight[0] == 3.0);
  CHECK(t.weight[1] == 1.0);
  CHECK(t.min_free[0] == Notional{});
  CHECK(t.min_free[1] == *Notional::parse("250.5"));
  CHECK(t.threshold == 0.1);
  CHECK(t.max_amount == *Notional::parse("1000"));
  CHECK(t.step == *Notional::parse("1"));
  CHECK(t.max_per_hour == 4);
  CHECK(t.min_interval_ns == 60'000'000'000);  // the defaults
  CHECK(t.cooldown_ns == 300'000'000'000);
  CHECK(t.state_file == "runs/mm.binance.treasury");
  CHECK_FALSE(cfg.treasury_config("binance_b").enabled);
  const Config back = Config::parse(cfg.effective_toml());
  CHECK(back.effective_toml() == cfg.effective_toml());
  CHECK(back.treasury_config("binance").min_free[1] == *Notional::parse("250.5"));
  CHECK(cfg.redacted().find("treasury = { enabled = true") != std::string::npos);
  // Without the table: disabled, and nothing in the effective configuration.
  const Config none = Config::parse(pool_toml(""));
  CHECK_FALSE(none.treasury_config("binance").enabled);
  CHECK(none.effective_toml().find("treasury") == std::string::npos);
}

TEST_CASE("core.config: a treasury belongs to a pool's primary and names its accounts") {
  const auto err = [](const char* tail) {
    try {
      static_cast<void>(Config::parse(pool_toml(tail)));
    } catch (const ConfigError& e) {
      return std::string(e.what());
    }
    return std::string();
  };
  CHECK(err("[venues.binance_b.treasury]\nasset = \"USDT\"\n")
            .find("the treasury goes on the "
                  "primary") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nenabled = true\n").find("enabled needs asset") !=
        std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\nweights = { other = 1 }\n")
            .find("'other' is not an account of the pool") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\nmin_free = { binance_b = \"-1\" }\n")
            .find("not a non-negative decimal") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\nthreshold = 2.0\n")
            .find("threshold must be 0 to 1") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\ninterval_s = 0\n")
            .find("interval_s must be at least 1") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\nweights = { binance = 0, binance_b = 0 "
            "}\n")
            .find("all zero") != std::string::npos);
  CHECK(err("[venues.binance.treasury]\nasset = \"USDT\"\nspeed = 1\n")
            .find("unknown key 'venues.*.treasury.speed'") != std::string::npos);
  // A venue without a pool.
  CHECK_THROWS_WITH_AS(Config::parse(R"(
[venues.solo]
kind = "sim"
[venues.solo.treasury]
asset = "USDT"
)"),
                       doctest::Contains("solo has no pool"),
                       ConfigError);
}

TEST_CASE("core.pool_plan: at most eight accounts, each venue in one pool") {
  PoolPlan plan;
  CHECK_FALSE(plan.add(VenueId{0}, VenueId{0}));
  CHECK_FALSE(plan.add(VenueId{1}, VenueId{9}));
  for (std::uint8_t v = 1; v < 8; ++v) CHECK(plan.add(VenueId{v}, VenueId{0}));
  CHECK(plan.members(VenueId{0}).size() == 8);
  CHECK_FALSE(plan.add(VenueId{1}, VenueId{2}));  // already a member
  CHECK_FALSE(plan.add(VenueId{0}, VenueId{3}));  // a primary cannot join another pool
  PoolPlan small;
  CHECK(small.add(VenueId{1}, VenueId{0}));
  CHECK_FALSE(small.add(VenueId{2}, VenueId{1}));  // a member is nobody's primary
  CHECK(small.members(VenueId{0}).contains(VenueId{1}));
  CHECK_FALSE(small.members(VenueId{0}).contains(VenueId{2}));
}

// ---- fill audit ([venues.<x>] fill_audit_*, live/fill_auditor.hpp) ------------------------------

namespace {
std::string audit_toml(const char* keys) {
  return std::string("[venues.binance]\nkind = \"sim\"\n") + keys +
         "[[instruments]]\nvenue = \"binance\"\nsymbol = \"BTCUSDT\"\ntick = \"0.01\"\n"
         "lot = \"0.001\"\n";
}
}  // namespace

TEST_CASE("core.config: the fill audit is off by default and checked when set") {
  const Config off = Config::parse(audit_toml(""));
  CHECK(off.venues[0].fill_audit_interval_s == 0);
  CHECK(off.venues[0].fill_audit_lag_s == 180);
  CHECK(off.venues[0].fill_audit_mode == "report");
  // Off, the effective configuration does not name it.
  CHECK(off.effective_toml().find("fill_audit") == std::string::npos);

  const Config on = Config::parse(audit_toml(
      "fill_audit_interval_s = 600\nfill_audit_lag_s = 120\nfill_audit_mode = \"book\"\n"));
  CHECK(on.venues[0].fill_audit_interval_s == 600);
  CHECK(on.venues[0].fill_audit_lag_s == 120);
  CHECK(on.venues[0].fill_audit_mode == "book");
  const Config back = Config::parse(on.effective_toml());
  CHECK(back.venues[0].fill_audit_interval_s == 600);
  CHECK(back.venues[0].fill_audit_mode == "book");
  CHECK(on.redacted().find("fill_audit_interval_s = 600") != std::string::npos);

  CHECK_THROWS_WITH_AS(Config::parse(audit_toml("fill_audit_interval_s = 5\n")),
                       doctest::Contains("fill_audit_interval_s must be 0 (off) or at least 10"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(Config::parse(audit_toml("fill_audit_lag_s = -1\n")),
                       doctest::Contains("fill_audit_lag_s must be >= 0"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(Config::parse(audit_toml("fill_audit_mode = \"fix\"\n")),
                       doctest::Contains(R"(fill_audit_mode must be "report" or "book")"),
                       ConfigError);
}

TEST_CASE("core.config: source_ip and source_interface are checked as written") {
  const Config none = Config::parse(audit_toml(""));
  CHECK(none.venues[0].source_ip.empty());
  CHECK(none.effective_toml().find("source_") == std::string::npos);
  const Config ip = Config::parse(audit_toml("source_ip = \"10.0.0.7\"\n"));
  CHECK(ip.venues[0].source_ip == "10.0.0.7");
  CHECK(Config::parse(ip.effective_toml()).venues[0].source_ip == "10.0.0.7");
  CHECK(ip.redacted().find("source_ip = \"10.0.0.7\"") != std::string::npos);
  CHECK(Config::parse(audit_toml("source_ip = \"fe80::1\"\n")).venues[0].source_ip == "fe80::1");
  const Config nic = Config::parse(audit_toml("source_interface = \"eth1\"\n"));
  CHECK(nic.venues[0].source_interface == "eth1");
  CHECK(Config::parse(nic.effective_toml()).venues[0].source_interface == "eth1");
  CHECK_THROWS_WITH_AS(Config::parse(audit_toml("source_ip = \"10.0.0.300\"\n")),
                       doctest::Contains("is not an IPv4 or IPv6 address"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(
      Config::parse(audit_toml("source_ip = \"10.0.0.7\"\nsource_interface = \"eth1\"\n")),
      doctest::Contains("source_ip and source_interface are exclusive"),
      ConfigError);
  CHECK_THROWS_WITH_AS(Config::parse(audit_toml("source_interface = \"a-name-far-too-long\"\n")),
                       doctest::Contains("longer than an interface name"),
                       ConfigError);
}
