// Config::effective_toml(): complete, secret-free, deterministic, and parsing it gives the same
// configuration back (journals embed it and hash it as config_hash).
#include "test_support.hpp"

#include "fastmm/config/config.hpp"

#include <string>

using namespace fastmm;

namespace {

Config load_repo_config(const char* name) {
  Config::LoadOptions lo;
  lo.substitute_env = false;
  return Config::load((fastmm::test::fixtures_dir() / ".." / ".." / "configs" / name).string(), lo);
}

Config reparse(const std::string& text) {
  Config::LoadOptions lo;
  lo.substitute_env = false;
  return Config::parse(text, lo, "effective");
}

}  // namespace

TEST_CASE("core.config: effective TOML round-trips and omits secrets") {
  for (const char* name : {"sim-local.toml", "backtest-example.toml", "binance-testnet.toml"}) {
    CAPTURE(name);
    Config cfg = load_repo_config(name);
    if (!cfg.venues.empty()) cfg.venues[0].api_key = "literal-key-that-must-not-leak";
    const std::string text = cfg.effective_toml();
    CHECK(text.find("literal-key-that-must-not-leak") == std::string::npos);
    CHECK(text.find("api_secret") == std::string::npos);
    const Config back = reparse(text);
    CHECK(back.warnings.empty());
    // Idempotent: the parsed text serializes to the same text, hence the same hash.
    CHECK(back.effective_toml() == text);
    CHECK(back.effective_hash() == cfg.effective_hash());
    CHECK(back.engine.rng_seed == cfg.engine.rng_seed);
    CHECK(back.engine.min_requote_interval_ms == cfg.engine.min_requote_interval_ms);
    CHECK(back.strategy.name == cfg.strategy.name);
    CHECK(back.strategy.params == cfg.strategy.params);
    CHECK(back.risk.max_order_qty == cfg.risk.max_order_qty);
    CHECK(back.risk.stale_md_ms == cfg.risk.stale_md_ms);
    REQUIRE(back.venues.size() == cfg.venues.size());
    for (std::size_t i = 0; i < cfg.venues.size(); ++i) {
      CHECK(back.venues[i].name == cfg.venues[i].name);
      CHECK(back.venues[i].supports_replace == cfg.venues[i].supports_replace);
      CHECK(back.venues[i].extra == cfg.venues[i].extra);
      CHECK(back.venues[i].fees.maker_bps == cfg.venues[i].fees.maker_bps);
    }
    REQUIRE(back.instruments.size() == cfg.instruments.size());
    for (std::size_t i = 0; i < cfg.instruments.size(); ++i) {
      CHECK(back.instruments[i].symbol == cfg.instruments[i].symbol);
      CHECK(back.instruments[i].tick == cfg.instruments[i].tick);
      CHECK(back.instruments[i].min_notional == cfg.instruments[i].min_notional);
    }
    CHECK(back.sim.values == cfg.sim.values);
    CHECK(back.backtest.values == cfg.backtest.values);
  }
}

TEST_CASE("core.config: the effective hash follows changes, not secrets or formatting") {
  const Config a = load_repo_config("sim-local.toml");
  Config b = a;
  b.venues[0].api_key = "other";
  b.venues[0].api_secret = "other";
  CHECK(b.effective_hash() == a.effective_hash());
  b.strategy.params["half_spread_bps"] = "7";
  CHECK(b.effective_hash() != a.effective_hash());
  // Comments and key order do not matter; the raw-text hash does see them.
  const Config c = reparse("# comment\n" + a.effective_toml());
  CHECK(c.effective_hash() == a.effective_hash());
  CHECK(c.hash != reparse(a.effective_toml()).hash);
}
