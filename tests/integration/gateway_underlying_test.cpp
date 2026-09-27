// The gateway's net position per underlying ([gateway.underlying]): strategy a quotes BTCUSDT and
// b quotes BTCUSDC on the simulator, and the account's BTC is one number over both. Everything is
// a real child process.
#include "gateway_util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

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

// The last number after `key` in the gateway's log, or -1.
long last_count(const GatewayProcess& g, const std::string& key) {
  const std::string text = fastmm::test::read_file(g.log);
  const std::size_t at = text.rfind(key);
  return at == std::string::npos ? -1L : std::strtol(text.c_str() + at + key.size(), nullptr, 10);
}

}  // namespace

TEST_CASE(
    "gateway underlying: a's and b's BTC orders count together, and a's position lets b quote the "
    "side that nets it") {
  sim::server::SimServerConfig sc = two_markets();
  sc.generator.market_rate_per_s = 0.0;  // only the test fills an order
  ServerFixture fx(sc);
  // Each strategy quotes 0.001 a side. With a's two orders working, b's 0.001 would take either
  // side to 0.002 BTC.
  Configs c = write_configs(fx, "gw-und", "\n[gateway.underlying.BTC]\nmax_net = \"0.0015\"\n");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  CHECK_MESSAGE(wait_until([&] { return last_count(g, "underlying=") >= 2; }, 30000),
                "b: " << fastmm::test::read_file(c.b.config + ".log"));
  CHECK(open_of(fx, ea) == 2);

  // One of a's orders fills: the account holds 0.001 BTC one way, and b's order on the other side
  // takes it to zero, so it goes to the venue.
  bool filled = false;
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == ea) {
      filled = fx.server.fill_open_order(id).is_positive();
      break;
    }
  }
  REQUIRE(filled);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  CHECK(open_of(fx, eb) >= 1);
  CHECK(wait_until(
      [&] {
        const std::string log = fastmm::test::read_file(g.log);
        return log.find("gateway: account underlying BTC net=0.001") != std::string::npos ||
               log.find("gateway: account underlying BTC net=-0.001") != std::string::npos;
      },
      10000));
  stop_strategy(a);
  stop_strategy(b);
  stop_gateway(g);
  const Outcomes ob = outcomes(c.b, RejectReason::GatewayUnderlyingNet);
  CHECK(ob.refused >= 2);
  CHECK(ob.acked >= 1);
  const std::string log = fastmm::test::read_file(g.log);
  CHECK(log.find("gateway: underlying BTC max_net=0.0015 over sim:BTCUSDT, sim:BTCUSDC") !=
        std::string::npos);
}

TEST_CASE("gateway underlying: a base asset no instrument of the gateway has is a config error") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-und-cfg", "\n[gateway.underlying.SOL]\nmax_net = 1\n");
  const GatewayProcess g = spawn_gateway(c.gw);
  CHECK(reap(g.pid) == live::kExitConfig);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
