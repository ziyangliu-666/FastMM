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
  CHECK(bt.venues[0].fees.maker_bps == doctest::Approx(-0.5));
  CHECK(bt.strategy.params.at("level_step_ticks") == "5");
  CHECK(bt.strategy.params.at("half_spread_bps") == "0.01");  // shortest round-trip form
}

TEST_CASE("core.config: minimal parse, float decimals stringified exactly, defaults") {
  const Config cfg = Config::parse(kMinimal);
  CHECK(cfg.instruments[0].lot == "0.001");
  CHECK(cfg.strategy.params.at("half_spread_bps") == "5");
  CHECK(cfg.strategy.params.at("levels") == "2");
  CHECK(cfg.engine.max_events_per_step == 64);
  CHECK(cfg.risk.max_open_orders == 4);
  CHECK(cfg.risk_limits().max_position == Qty::from_decimal("0.5").value());
  const InstrumentTable t = load_instruments(cfg);
  CHECK(t.get(InstrumentId{0}).lot == Qty::from_decimal("0.001").value());
  CHECK(t.get(InstrumentId{0}).min_qty == Qty::from_decimal("0.001").value());  // defaults to lot
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

TEST_CASE("core.config: validation errors carry line numbers, unknown keys warn") {
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
  const Config w = Config::parse(
      "[engine]\nbogus = 1\n[weird]\nx = 1\n[venues.v]\nkind = \"sim\"\nfoo = \"bar\"\n");
  REQUIRE(w.warnings.size() == 3);
  CHECK(w.warnings[0].find("[weird]") != std::string::npos);
  CHECK(w.warnings[1].find("engine.bogus") != std::string::npos);
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

TEST_CASE("core.config: connector-specific venue keys pass through to extra without warnings") {
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
  CHECK(v.extra.count("not_a_real_key") == 1);  // unknown keys are still forwarded
  REQUIRE(cfg.warnings.size() == 1);            // but only they warn
  CHECK(cfg.warnings[0].find("not_a_real_key") != std::string::npos);
  CHECK_THROWS_AS(static_cast<void>(Config::parse(R"(
[venues.b]
kind = "binance_spot"
stale_ms = "soon"
)")),
                  ConfigError);  // passthrough keys are still type-checked
}

TEST_CASE("core.config: shipped venue configs load without warnings") {
  ConfigLoadOptions opts;
  opts.substitute_env = false;  // keep ${FASTMM_*} references; no secrets needed to validate
  for (const char* name :
       {"binance-testnet.toml", "bybit-testnet.toml", "sim-local.toml", "sim-local-tls.toml"}) {
    const Config cfg = Config::load((configs_dir() / name).string(), opts);
    INFO(name);
    for (const auto& w : cfg.warnings) MESSAGE(w);
    CHECK(cfg.warnings.empty());
  }
}
