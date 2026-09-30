// The account's balance behind fastmm-gateway: strategy a quotes BTCUSDT and b quotes ETHUSDT on
// the simulator, and both spend the one USDT balance. The gateway's balance guard covers their
// orders together: b's bid does not fit what a's resting bid left, and the gateway refuses it
// before the venue has to. b runs avellaneda_stoikov, which does not size its quotes to the
// balance, with its own check off ([risk] check_balance = false), so the bid reaches the gateway.
// Everything is a real child process.
#include "gateway_util.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

std::size_t refused(const SessionFiles& f, RejectReason why) {
  std::size_t n = 0;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) == 0 && h->type == EventType::OrderReject &&
          msg_cast<OrderRejectMsg>(h).reason == why)
        ++n;
    });
  }
  return n;
}

// The gateway's balance of `asset` on its first venue, once its status shows it reported.
std::optional<StatusBalance> gateway_balance(const std::string& gw_name, std::string_view asset) {
  const auto s = gateway_status(gw_name);
  if (!s) return std::nullopt;
  for (std::uint32_t i = 0; i < s->balance_count; ++i) {
    const StatusBalance& b = s->balances[i];
    if (b.known != 0 && std::string_view(b.asset) == asset) return b;
  }
  return std::nullopt;
}

std::uint64_t gateway_refused_balance(const std::string& gw_name) {
  const auto s = gateway_status(gw_name);
  return s ? s->gateway.venues[0].refused[10] : 0;
}

}  // namespace

TEST_CASE("gateway balance: two strategies spend one USDT balance, the gateway refuses the rest") {
  static_assert(kStatusGatewayRefusalReasons[10] == RejectReason::GatewayBalanceShort);
  sim::server::SimServerConfig sc = two_markets(kEthUsdt);
  sc.generator.market_rate_per_s = 0.0;  // nothing fills
  // One 0.001 bid at about 60000 holds about 60 USDT: 100 covers one strategy's bid, not both.
  sc.balances = {
      {"USDT", Qty::from_int(100)}, {"BTC", Qty::from_int(1)}, {"ETH", Qty::from_int(1)}};
  ServerFixture fx(sc);
  const Configs c = write_configs(fx, "gw-bal", {}, kEthUsdt);
  rewrite(c.b.config, [](std::string& t) {
    replace_first(t, R"(name = "basic_mm")", R"(name = "avellaneda_stoikov")");
    replace_first(t,
                  "half_spread_bps = 5.0\nskew_bps_per_unit = 1.0\nquote_qty = 0.001\n"
                  "max_inventory = 0.01\nrequote_threshold_ticks = 1\npull_on_stale_ms = 2000\n"
                  "levels = 1\n",
                  "quote_qty = 0.001\n");
    replace_first(t, "[risk]\n", "[risk]\ncheck_balance = false\n");
  });
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  REQUIRE(wait_until([&] { return open_of(fx, ea) == 2; }, 30000));
  // The account knows the balance with a's bid in it.
  REQUIRE(wait_until(
      [&] {
        const auto usdt = gateway_balance("gw-bal-gw", "USDT");
        return usdt && usdt->free_raw < Qty::from_int(41).raw && usdt->locked_raw > 0;
      },
      30000));

  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  REQUIRE(wait_until([&] { return gateway_refused_balance("gw-bal-gw") >= 1; }, 30000));
  // b's ask rests (1 ETH covers it); its bid never reached the venue.
  CHECK(wait_until([&] { return open_of(fx, eb) == 1; }, 10000));
  CHECK(open_of(fx, ea) == 2);
  CHECK(fx.server.stats().orders_rejected == 0);
  const auto usdt = gateway_balance("gw-bal-gw", "USDT");
  REQUIRE(usdt.has_value());
  CHECK(usdt->free_raw >= 0);

  stop_strategy(a);
  stop_strategy(b);
  stop_gateway(g);
  CHECK(refused(c.b, RejectReason::GatewayBalanceShort) >= 1);
  CHECK(refused(c.a, RejectReason::GatewayBalanceShort) == 0);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
