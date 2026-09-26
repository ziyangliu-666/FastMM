// The gateway's account across settlement currencies ([accounting]): BTCUSDT settles in USDT and
// ETHBTC in BTC on the simulator, BTCUSDT's mid prices BTC, and the account's limits are in USDT.
// Everything is a real child process.
#include "gateway_util.hpp"

#include "fastmm/core/session_state.hpp"

#include <chrono>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

// Same filters and prices as BTCUSDT: only the currencies differ.
constexpr Market kEthBtc{"ETHBTC", "ETH", "BTC"};

constexpr std::string_view kAccounting =
    "\n[accounting]\nreporting_currency = \"USDT\"\n[accounting.fx]\nBTC = \"sim:BTCUSDT\"\n";

// The gateway's and b's configurations convert; b's lists BTCUSDT, its currency's source, without
// trading it (the gateway's table has it, a trades it).
Configs fx_configs(const ServerFixture& fx, const std::string& stem, const std::string& gateway) {
  Configs c = write_configs(fx, stem, gateway + std::string(kAccounting), kEthBtc);
  rewrite(c.b.config, [](std::string& t) {
    std::string source = second_instrument(Market{"BTCUSDT", "BTC", "USDT"});
    replace_first(source, "enabled = true", "enabled = false");
    replace_first(t, "\n[strategy]\n", source + "\n[strategy]\n");
    t += kAccounting;
  });
  return c;
}

// The last "gateway: account net_pnl=..." line's words as numbers, and the line.
std::map<std::string, double> account_numbers(const GatewayProcess& g, std::string* line) {
  const std::string text = fastmm::test::read_file(g.log);
  const std::size_t complete = text.rfind('\n');
  std::map<std::string, double> out;
  if (complete == std::string::npos) return out;
  const std::size_t at = text.rfind("gateway: account net_pnl=", complete);
  if (at == std::string::npos) return out;
  const std::size_t end = text.find('\n', at);
  *line = text.substr(at, end - at);
  std::istringstream in(*line);
  for (std::string w; in >> w;) {
    const std::size_t eq = w.find('=');
    if (eq == std::string::npos || eq + 1 >= w.size()) continue;
    const char ch = w[eq + 1];
    if (ch != '-' && (ch < '0' || ch > '9')) continue;
    out[w.substr(0, eq)] = std::strtod(w.c_str() + eq + 1, nullptr);
  }
  return out;
}

// How often one session's journal saw its orders refused for `why`, and acknowledged.
struct Outcomes {
  std::size_t refused = 0;
  std::size_t acked = 0;
};
Outcomes outcomes(const SessionFiles& f, RejectReason why) {
  Outcomes out;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) != 0) return;
      if (h->type == EventType::OrderReject && msg_cast<OrderRejectMsg>(h).reason == why)
        ++out.refused;
      if (h->type == EventType::OrderAck) ++out.acked;
    });
  }
  return out;
}

sim::server::SimServerConfig eth_btc_markets() {
  return two_markets(kEthBtc);
}

}  // namespace

TEST_CASE(
    "gateway fx: the account limits start over two settlement currencies with a source for "
    "each, and are refused without one, as without [accounting]") {
  ServerFixture fx(eth_btc_markets());
  Configs c = write_configs(fx, "gw-fx-refuse", "\n[gateway]\nmax_loss = \"3\"\n", kEthBtc);
  {
    const GatewayProcess g = spawn_gateway(c.gw);
    CHECK(reap(g.pid) == live::kExitConfig);
  }
  rewrite(c.gw.config,
          [](std::string& t) { t += "\n[accounting]\nreporting_currency = \"USDT\"\n"; });
  {
    const GatewayProcess g = spawn_gateway(c.gw);
    CHECK(reap(g.pid) == live::kExitConfig);
  }
  rewrite(c.gw.config, [](std::string& t) { t += "[accounting.fx]\nBTC = \"sim:BTCUSDT\"\n"; });
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  stop_gateway(g);
}

TEST_CASE(
    "gateway fx: an ETHBTC order is measured in USDT against max_gross_notional, and BTCUSDT "
    "trades on") {
  ServerFixture fx(eth_btc_markets());
  // b quotes 0.0002 ETH at 60000 BTC: 12 BTC, about 720000 USDT, past the 100000 cap from flat.
  // In BTC it would be 12 against 100000. a's 0.001 BTC is about 60 USDT.
  Configs c = fx_configs(fx, "gw-fx-gross", "\n[gateway]\nmax_gross_notional = \"100000\"\n");
  rewrite(c.b.config,
          [](std::string& t) { replace_first(t, "quote_qty = 0.001", "quote_qty = 0.0002"); });
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  // The gateway's once-a-second counters: "refused: ... gross_notional=<n>" (the journals are read
  // once the strategies stopped).
  const auto refused = [&] {
    const std::string text = fastmm::test::read_file(g.log);
    const std::size_t at = text.rfind("gross_notional=");
    return at == std::string::npos ? 0L : std::strtol(text.c_str() + at + 15, nullptr, 10);
  };
  CHECK_MESSAGE(wait_until([&] { return refused() >= 2; }, 30000),
                "b: " << fastmm::test::read_file(c.b.config + ".log"));
  const std::uint64_t fa = fills(fx, 0);
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == ea) {
      static_cast<void>(fx.server.fill_open_order(id));
      break;
    }
  }
  CHECK(wait_until([&] { return fills(fx, 0) > fa; }, 10000));
  stop_strategy(a);
  stop_strategy(b);
  stop_gateway(g);
  CHECK(open_of(fx, ea) == 0);
  const Outcomes ob = outcomes(c.b, RejectReason::GatewayGrossNotional);
  CHECK(ob.refused >= 2);
  CHECK(ob.acked == 0);  // none of b's orders reached the venue
  CHECK(outcomes(c.a, RejectReason::GatewayGrossNotional).acked > 0);
  const std::string log = fastmm::test::read_file(g.log);
  CHECK(log.find("accounting: BTC converts to USDT at the mid of sim:BTCUSDT") !=
        std::string::npos);
}

TEST_CASE(
    "gateway fx: max_loss in USDT trips on a fee paid in BTC that is under the budget as a BTC "
    "number") {
  sim::server::SimServerConfig sc = eth_btc_markets();
  // A fill of b's 0.001 ETH at 60000 BTC is 60 BTC and costs 1%: 0.6 BTC, about 36000 USDT.
  sc.maker_bps = 100.0;
  sc.generator.market_rate_per_s = 0.5;
  ServerFixture fx(sc);
  Configs c = fx_configs(fx, "gw-fx-loss", "\n[gateway]\nmax_loss = \"3\"\n");
  // b's own budget is not what stops it.
  rewrite(c.b.config, [](std::string& t) {
    replace_first(t, R"(max_loss = "1000")", R"(max_loss = "1000000000")");
  });
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {});
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == eb) {
      static_cast<void>(fx.server.fill_open_order(id));
      break;
    }
  }
  const std::string latch_line = "account kill switch tripped";
  REQUIRE_MESSAGE(
      wait_until(
          [&] { return fastmm::test::read_file(g.log).find(latch_line) != std::string::npos; },
          10000),
      "the account never tripped: " << fastmm::test::read_file(g.log));
  CHECK(reap(b) == live::kExitKilled);
  const auto st = KillStateStore::load(c.gw.kill);
  REQUIRE(st.has_value());
  CHECK(st->latched);
  CHECK(st->reason == KillReason::GatewayMaxLoss);
  stop_gateway(g);
  std::string line;
  const std::map<std::string, double> n = account_numbers(g, &line);
  INFO("gateway: " << line);
  CHECK(line.find(" in=USDT") != std::string::npos);
  // The fee is 0.6 BTC: at about 60000 USDT a BTC, some 36000 USDT (the mid wanders a little).
  REQUIRE(n.contains("fees"));
  CHECK(n.at("fees") > 30000.0);
  CHECK(n.at("fees") < 42000.0);
  CHECK(st->carry().to_double() < -30000.0);
}

TEST_CASE(
    "gateway fx: an ETHBTC order goes back as GatewayFxRateUnknown while the gateway's BTCUSDT "
    "book is stale") {
  ServerFixture fx(eth_btc_markets());
  // BTCUSDT's book changes every 100 ms at the simulator; at 1 ms it is stale almost always.
  Configs c = fx_configs(fx, "gw-fx-stale", "\n[gateway]\nmax_loss = \"1000000000\"\n");
  rewrite(c.gw.config,
          [](std::string& t) { replace_first(t, "stale_md_ms = 2000", "stale_md_ms = 1"); });
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t b = spawn_strategy(c.b, g);
  // The gateway's once-a-second counters: "refused: ... fx_rate=<n>".
  const auto refused = [&] {
    const std::string text = fastmm::test::read_file(g.log);
    const std::size_t at = text.rfind("fx_rate=");
    return at == std::string::npos ? 0L : std::strtol(text.c_str() + at + 8, nullptr, 10);
  };
  CHECK_MESSAGE(wait_until([&] { return refused() >= 2; }, 30000),
                "b: " << fastmm::test::read_file(c.b.config + ".log"));
  stop_strategy(b);
  stop_gateway(g);
  CHECK(outcomes(c.b, RejectReason::GatewayFxRateUnknown).refused >= 2);
}

TEST_CASE(
    "fx: fastmm-live quotes instruments in two settlement currencies under max_loss with "
    "[accounting], and refuses them without") {
  ServerFixture fx(eth_btc_markets());
  // BTCUSDT and ETHBTC in one session, [risk] max_loss = 1000.
  const SessionFiles f = write_config(fx, "live-fx", "exit", "1000");
  remove_all_of({f.epoch, f.kill, f.journal_dir, f.config + ".log", f.status});
  rewrite(f.config, [](std::string& t) {
    replace_first(t, "\n[strategy]\n", second_instrument(kEthBtc) + "\n[strategy]\n");
  });
  CHECK(reap(spawn_live(f, 60)) == live::kExitConfig);
  rewrite(f.config, [](std::string& t) { t += kAccounting; });
  const pid_t p = spawn_live(f, 120);
  // Both sides of both instruments rest: ETHBTC's orders passed the rate check.
  CHECK_MESSAGE(wait_until([&] { return fx.server.stats().open_orders >= 4; }, 30000),
                fastmm::test::read_file(f.config + ".log"));
  stop_strategy(p);
  const std::string log = fastmm::test::read_file(f.config + ".log");
  CHECK(log.find("accounting: BTC converts to USDT at the mid of sim:BTCUSDT") !=
        std::string::npos);
  CHECK(log.find("the PnL totals are in USDT") != std::string::npos);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
