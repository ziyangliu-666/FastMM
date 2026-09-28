// fastmm-gateway with two strategies placing and cancelling orders through one connector, every
// cancel's answer lost: the connector's order shadows are swept for both strategies at the next
// open-order snapshot, so its table stays bounded however long the session runs. The strategies
// are raw GatewayClients in this process (exact orders); the gateway is a real child on the
// simulator (Binance Spot connector).
#include "gateway_util.hpp"

#include "fastmm/live/gateway.hpp"

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

struct RawClient {
  std::unique_ptr<live::GatewayClient> client;
  InstrumentId inst{};
  std::uint32_t seq = 0;
  std::size_t acks = 0;

  RawClient(const GatewayProcess& g, const std::string& engine, const std::string& symbol) {
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = engine;
    req.instruments = {{"sim", symbol}};
    client = live::GatewayClient::attach(g.socket, req, &err);
    REQUIRE_MESSAGE(client != nullptr, err);
    const Instrument* i = client->instruments().find(VenueId{0}, symbol);
    REQUIRE(i != nullptr);
    inst = i->id;
  }

  template <class M>
  void push(const M& m) {
    REQUIRE(
        wait_until([&] { return client->venues()[0].outbound->try_push(&m, m.hdr.len); }, 5000));
    live::GatewayClient::wake_venue(client.get(), VenueId{0});
  }
  // A buy far below the market: it rests.
  ClientOrderId place() {
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, inst, VenueId{0});
    m.cl_ord_id = make_cl_ord_id(client->session_epoch(), ++seq);
    m.side = Side::Buy;
    m.type = OrderType::Limit;
    m.tif = TimeInForce::Gtc;
    m.price = Price::from_decimal("1000").value();
    m.qty = Qty::from_decimal("0.01").value();
    push(m);
    return m.cl_ord_id;
  }
  void cancel(ClientOrderId id) {
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, inst, VenueId{0});
    m.cl_ord_id = id;
    push(m);
  }
  void reconcile() {
    ControlMsg m{};
    init_header(m, EventType::Control, InstrumentId::invalid(), VenueId{0});
    m.command = ControlCommand::Reconcile;
    push(m);
  }
  // Reads what the gateway sent it (market data is left: the gateway drops it for this client).
  void drain() {
    ShmRing& ring = *client->venues()[0].order;
    while (const std::byte* p = ring.try_peek()) {
      const auto& h = *reinterpret_cast<const EventHeader*>(p);
      if (h.type == EventType::OrderAck) ++acks;
      ring.release();
    }
  }
};

// The connector's order shadows, from the gateway's last once-a-second venue line.
std::optional<std::size_t> shadows_logged(const GatewayProcess& g) {
  const std::string text = fastmm::test::read_file(g.log);
  const std::size_t line = text.rfind("[sim] md=");
  if (line == std::string::npos) return std::nullopt;
  const std::size_t at = text.find(" shadows=", line);
  if (at == std::string::npos || at > text.find('\n', line)) return std::nullopt;
  std::size_t n = 0;
  const char* first = text.data() + at + 9;
  if (std::from_chars(first, text.data() + text.size(), n).ec != std::errc{}) return std::nullopt;
  return n;
}

// Waits until a venue line logged after `mark` (bytes of the log) shows `n` shadows.
bool wait_shadows(const GatewayProcess& g, std::size_t n, std::size_t* mark) {
  return wait_until(
      [&] {
        const std::size_t size = fastmm::test::read_file(g.log).size();
        if (size <= *mark) return false;
        const auto s = shadows_logged(g);
        if (!s || *s != n) return false;
        *mark = size;
        return true;
      },
      15000);
}

}  // namespace

TEST_CASE(
    "gateway: two strategies' orders whose end was lost are swept from the connector, the table "
    "stays bounded") {
  sim::server::SimServerConfig quiet = two_markets();
  quiet.generator.market_rate_per_s = 0.0;  // nothing trades with the resting orders
  ServerFixture fx(quiet);
  const Configs c = write_configs(fx, "gw-shadows");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  RawClient a(g, c.a_name, "BTCUSDT");
  RawClient b(g, c.b_name, "BTCUSDC");
  REQUIRE(a.client->session_epoch() != b.client->session_epoch());

  constexpr std::size_t kPerRound = 60;  // each; the simulator holds 200 open orders a symbol
  constexpr int kRounds = 4;
  std::size_t mark = 0;
  for (int round = 0; round < kRounds; ++round) {
    INFO("round " << round);
    // Both place, interleaved: b is the last to send, so the watermark is one of b's ids.
    std::vector<ClientOrderId> ids_a;
    std::vector<ClientOrderId> ids_b;
    for (std::size_t k = 0; k < kPerRound; ++k) {
      ids_a.push_back(a.place());
      ids_b.push_back(b.place());
    }
    // Resting at the simulator, and answered: the gateway passed the acks on.
    const std::size_t placed = static_cast<std::size_t>(round + 1) * kPerRound;
    REQUIRE_MESSAGE(wait_until(
                        [&] {
                          a.drain();
                          b.drain();
                          return a.acks >= placed && b.acks >= placed &&
                                 fx.server.open_client_order_ids().size() == 2 * kPerRound;
                        },
                        20000),
                    fastmm::test::read_file(g.log));

    // Every cancel is carried out and its answer lost: no reply, no execution report.
    fx.server.set_user_stream_muted(true);
    fx.server.swallow_next_ws_api_responses(2 * kPerRound);
    for (std::size_t k = 0; k < kPerRound; ++k) {
      a.cancel(ids_a[k]);
      b.cancel(ids_b[k]);
    }
    REQUIRE(wait_until([&] { return fx.server.open_client_order_ids().empty(); }, 20000));
    fx.server.set_user_stream_muted(false);
    // The connector still holds a shadow for each of them.
    REQUIRE_MESSAGE(wait_shadows(g, 2 * kPerRound, &mark), fastmm::test::read_file(g.log));

    // A snapshot names none of them, all were answered before it: every one is swept, a's too.
    a.reconcile();
    CHECK_MESSAGE(wait_shadows(g, 0, &mark),
                  "shadows " << shadows_logged(g).value_or(SIZE_MAX) << ": "
                             << fastmm::test::read_file(g.log));
    a.drain();
    b.drain();
  }

  a.client.reset();
  b.client.reset();
  stop_gateway(g);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
