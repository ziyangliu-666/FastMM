// fastmm-sim-exchange with a pool of sub-accounts under one master: each account's orders,
// balances, order windows and user streams on their own, internal transfers signed with the
// master key (POST / GET /sapi/v1/sub-account/universalTransfer) with their user-stream events,
// refusals and faults, and FastMM's TransferClient against it.
#include "integration_util.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/net/url.hpp"
#include "fastmm/sim/server/binance_json.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_transfer.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using sim::server::SimAccountConfig;
using sim::server::SimBalanceConfig;
using sim::server::SimServerConfig;

namespace {

constexpr const char* kMasterKey = "master-key";
constexpr const char* kMasterSecret = "master-secret";
constexpr const char* kTransfer = "/sapi/v1/sub-account/universalTransfer";

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

Qty q(std::string_view text) {
  return Qty::from_decimal(text).value();
}

SimAccountConfig pool_account(std::string name,
                              std::vector<SimBalanceConfig> balances,
                              bool ed25519 = false) {
  SimAccountConfig a;
  a.api_key = name + "-key";
  if (ed25519) {
    a.ed25519_public_key_pem = fastmm::test::fixture("binance/ed25519-test-public.pem");
  } else {
    a.api_secret = name + "-secret";
  }
  a.email = name + "@pool.test";
  a.name = std::move(name);
  a.balances = std::move(balances);
  return a;
}

// Two accounts, `a` (HMAC) and `b` (Ed25519), and a master with an HMAC key and some USDT.
SimServerConfig pool_config() {
  SimServerConfig c = test_server_config();
  c.accounts.push_back(pool_account("a", {{"BTC", q("2")}, {"USDT", q("100000")}}));
  c.accounts.push_back(pool_account("b", {{"BTC", q("1")}, {"USDT", q("50000")}}, true));
  c.master.api_key = kMasterKey;
  c.master.api_secret = kMasterSecret;
  c.master.balances = {{"USDT", q("1000")}};
  return c;
}

Signer hmac_signer(std::string key, std::string secret) {
  Credentials c;
  c.api_key = std::move(key);
  c.secret.value = std::move(secret);
  return Signer(std::move(c));
}

Signer ed25519_signer(std::string key) {
  Credentials c;
  c.api_key = std::move(key);
  c.type = binance::KeyType::Ed25519;
  c.private_key_pem.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  return Signer(std::move(c));
}

using Params = std::vector<std::pair<std::string, std::string>>;

// A signed REST request as BinanceVenue sends it: the parameters, recvWindow, timestamp and the
// signature in the query.
struct SignedRest {
  const ServerFixture& fx;
  BlockingHttp http;
  explicit SignedRest(const ServerFixture& f, BlockingHttpOptions o = {})
      : fx(f), http(f.http(), o) {}

  HttpReply operator()(const Signer& s,
                       std::string_view method,
                       std::string_view path,
                       const Params& params = {}) {
    net::QueryBuilder<4096> qb;
    for (const auto& [k, v] : params) qb.add(k, v);
    qb.add("recvWindow", std::int64_t{5000});
    qb.add("timestamp", fx.server.server_time_ms());
    REQUIRE(s.sign_query(qb));
    return http.request(
        method, std::string(path) + "?" + std::string(qb.view()), api_key_header(s.api_key()));
  }
};

Params transfer_params(std::string_view from,
                       std::string_view to,
                       std::string_view asset,
                       std::string_view amount,
                       std::string_view client_id) {
  Params p = {{"amount", std::string(amount)},
              {"asset", std::string(asset)},
              {"fromAccountType", "SPOT"},
              {"toAccountType", "SPOT"}};
  if (!client_id.empty()) p.emplace_back("clientTranId", std::string(client_id));
  if (!from.empty()) p.emplace_back("fromEmail", std::string(from));
  if (!to.empty()) p.emplace_back("toEmail", std::string(to));
  return p;
}

Params limit_maker(std::string_view side, std::string_view price, std::string_view qty) {
  return {{"symbol", "BTCUSDT"},
          {"side", std::string(side)},
          {"type", "LIMIT_MAKER"},
          {"quantity", std::string(qty)},
          {"price", std::string(price)},
          {"newClientOrderId", "same-id"},
          {"newOrderRespType", "ACK"}};
}

Qty free_of(const std::vector<sim::server::SimExchangeServer::AssetBalance>& bs,
            std::string_view asset) {
  for (const auto& b : bs) {
    if (b.asset == asset) return b.free;
  }
  return Qty{};
}
Qty locked_of(const std::vector<sim::server::SimExchangeServer::AssetBalance>& bs,
              std::string_view asset) {
  for (const auto& b : bs) {
    if (b.asset == asset) return b.locked;
  }
  return Qty{};
}

// Waits until `ms` milliseconds or more are left in the server's current `window_ms` window, so a
// test that counts within one window does not straddle its reset.
void wait_for_window_room(const ServerFixture& fx, std::int64_t window_ms, std::int64_t ms) {
  REQUIRE(
      wait_until([&] { return window_ms - fx.server.server_time_ms() % window_ms >= ms; }, 70'000));
}

}  // namespace

TEST_CASE("sim_pool: two accounts keep their own orders, balances and order windows") {
  SimServerConfig cfg = pool_config();
  cfg.account_orders_limit_per_10s = 2;
  ServerFixture fx(cfg);
  CHECK(fx.server.account_names() == std::vector<std::string>{"a", "b"});
  const Signer a = hmac_signer("a-key", "a-secret");
  const Signer b = ed25519_signer("b-key");
  SignedRest rest(fx);
  const Price bid = fx.server.stats().best_bid.price;
  REQUIRE(bid.is_positive());
  std::string px;
  sim::server::append_decimal(px, bid - Price::from_int(500));
  wait_for_window_room(fx, 10'000, 5'000);

  // The same client order id on both accounts: two orders, one per account.
  HttpReply r = rest(a, "POST", "/api/v3/order", limit_maker("BUY", px, "0.01"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  CHECK(r.header("X-MBX-ORDER-COUNT-10S") == "1");
  r = rest(a, "POST", "/api/v3/order", limit_maker("BUY", px, "0.01"));
  CHECK(r.status == 400);
  CHECK(contains(r.body, "Duplicate order sent."));
  r = rest(b, "POST", "/api/v3/order", limit_maker("BUY", px, "0.02"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  CHECK(r.header("X-MBX-ORDER-COUNT-10S") == "1");  // b's window, not a's
  CHECK(fx.server.stats().open_orders == 2);

  r = rest(a, "GET", "/api/v3/openOrders", {{"symbol", "BTCUSDT"}});
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"("origQty":"0.01000000")"));
  CHECK_FALSE(contains(r.body, R"("origQty":"0.02000000")"));

  // Each account's lock is its own.
  const Notional lock_a = mul(bid - Price::from_int(500), q("0.01"));
  const auto bal_a = fx.server.balances("a");
  const auto bal_b = fx.server.balances("b");
  CHECK(locked_of(bal_a, "USDT").raw == lock_a.raw);
  CHECK(free_of(bal_a, "USDT").raw == q("100000").raw - lock_a.raw);
  CHECK(locked_of(bal_b, "USDT").raw == 2 * lock_a.raw);
  CHECK(free_of(bal_b, "BTC") == q("1"));
  CHECK(fx.server.balances().size() == bal_a.size());  // empty name: the first account
  CHECK(fx.server.balances("nobody").empty());
  r = rest(b, "GET", "/api/v3/account", {{"omitZeroBalances", "true"}});
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"({"asset":"BTC","free":"1.00000000","locked":"0.00000000"})"));

  // a reaches its 10 s window (2); b still has room in its own.
  Params second = limit_maker("BUY", px, "0.01");
  second[5].second = "second";
  r = rest(a, "POST", "/api/v3/order", second);
  REQUIRE_MESSAGE(r.status == 200, r.body);
  second[5].second = "third";
  r = rest(a, "POST", "/api/v3/order", second);
  CHECK(r.status == 429);
  CHECK(contains(r.body, R"("code":-1015)"));
  r = rest(b, "POST", "/api/v3/order", second);
  CHECK_MESSAGE(r.status == 200, r.body);
  r = rest(b, "GET", "/api/v3/rateLimit/order");
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"("interval":"SECOND","intervalNum":10,"limit":2,"count":2)"));

  // Cancel-all is per account too.
  r = rest(a, "DELETE", "/api/v3/openOrders", {{"symbol", "BTCUSDT"}});
  REQUIRE(r.ok());
  CHECK(fx.server.stats().open_orders == 2);  // b's two
  CHECK(locked_of(fx.server.balances("a"), "USDT").is_zero());
}

TEST_CASE("sim_pool: a transfer shows on both accounts' user streams as BinanceVenue reads them") {
  ServerFixture fx(pool_config());
  BinanceVenueConfig va = venue_config(fx);
  va.credentials.api_key = "a-key";
  va.credentials.secret.value = "a-secret";
  va.position_from_balance = true;
  BinanceVenueConfig vb = venue_config(fx);
  vb.credentials.api_key = "b-key";
  vb.credentials.secret.value.clear();
  vb.credentials.type = binance::KeyType::Ed25519;
  vb.credentials.private_key_pem.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  vb.position_from_balance = true;
  VenueHarness ha(std::move(va));
  VenueHarness hb(std::move(vb));
  REQUIRE(ha.venue->load_reference_data(ha.instruments));
  REQUIRE(hb.venue->load_reference_data(hb.instruments));
  ha.connect();
  hb.connect();
  auto both = [&](auto pred) {
    return pump_until(
        ha.reactor,
        [&] {
          hb.reactor.run_once(0);
          ha.oc.take(ha.orders);
          hb.oc.take(hb.orders);
          ha.mdc.take(ha.md);
          hb.mdc.take(hb.md);
          return pred();
        },
        15'000);
  };
  REQUIRE(both([&] { return ha.live_order_channels() >= 2 && hb.live_order_channels() >= 2; }));
  CHECK(fx.server.stats().user_subscriptions == 2);

  SignedRest rest(fx);
  const Signer master = hmac_signer(kMasterKey, kMasterSecret);
  const HttpReply r = rest(master,
                           "POST",
                           kTransfer,
                           transfer_params("a@pool.test", "b@pool.test", "BTC", "0.75", "t1"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  CHECK(contains(r.body, R"("clientTranId":"t1")"));
  CHECK(contains(r.body, R"("tranId":)"));
  CHECK_FALSE(r.header("X-SAPI-USED-UID-WEIGHT-1M").empty());
  CHECK(r.header("X-SAPI-USED-UID-WEIGHT-1M") == "360");

  auto flow_then_position = [](const Collected& c, std::int64_t amount_raw, Qty total) {
    std::size_t flow_at = c.all.size();
    for (std::size_t i = 0; i < c.all.size(); ++i) {
      if (Collected::type_of(c.all[i]) != EventType::AccountFlow) continue;
      const auto& f = Collected::as<AccountFlowMsg>(c.all[i]);
      if (f.asset.view() == "BTC" && f.amount.raw == amount_raw) flow_at = i;
    }
    if (flow_at == c.all.size()) return false;
    for (std::size_t i = flow_at + 1; i < c.all.size(); ++i) {
      if (Collected::type_of(c.all[i]) == EventType::PositionUpdate &&
          Collected::as<PositionUpdateMsg>(c.all[i]).qty == total)
        return true;
    }
    return false;
  };
  REQUIRE(both([&] {
    return flow_then_position(ha.oc, -Notional::from_decimal("0.75").value().raw, q("1.25")) &&
           flow_then_position(hb.oc, Notional::from_decimal("0.75").value().raw, q("1.75"));
  }));
  CHECK(free_of(fx.server.balances("a"), "BTC") == q("1.25"));
  CHECK(free_of(fx.server.balances("b"), "BTC") == q("1.75"));
  const sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.transfers_done == 1);
  CHECK(st.transfer_requests == 1);
  CHECK_FALSE(ha.venue->fatal());
  CHECK_FALSE(hb.venue->fatal());
}

TEST_CASE("sim_pool: refusals, duplicate clientTranId and the history") {
  SimServerConfig cfg = pool_config();
  cfg.accounts[0].balances = {{"BTC", q("1")}, {"USDT", q("100000")}};
  ServerFixture fx(cfg);
  SignedRest rest(fx);
  const Signer master = hmac_signer(kMasterKey, kMasterSecret);
  const Signer a = hmac_signer("a-key", "a-secret");

  // 0.6 BTC of a's 1 is locked by a resting sell: 0.4 is free.
  const Price ask = fx.server.stats().best_ask.price;
  std::string px;
  sim::server::append_decimal(px, ask + Price::from_int(500));
  HttpReply r = rest(a, "POST", "/api/v3/order", limit_maker("SELL", px, "0.6"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  r = rest(
      master, "POST", kTransfer, transfer_params("a@pool.test", "b@pool.test", "BTC", "0.5", "x1"));
  CHECK(r.status == 400);
  CHECK(r.body == R"({"code":-9000,"msg":"user have no available amount"})");
  r = rest(
      master, "POST", kTransfer, transfer_params("a@pool.test", "b@pool.test", "BTC", "0.4", "x1"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  CHECK(free_of(fx.server.balances("a"), "BTC").is_zero());
  CHECK(locked_of(fx.server.balances("a"), "BTC") == q("0.6"));

  // The same clientTranId again: refused, nothing moves.
  r = rest(
      master, "POST", kTransfer, transfer_params("b@pool.test", "a@pool.test", "BTC", "0.1", "x1"));
  CHECK(r.status == 400);
  CHECK(contains(r.body, "Duplicate clientTranId."));
  CHECK(free_of(fx.server.balances("b"), "BTC") == q("1.4"));

  // From the master's wallet (no fromEmail) to b.
  r = rest(master, "POST", kTransfer, transfer_params("", "b@pool.test", "USDT", "250", "m1"));
  REQUIRE_MESSAGE(r.status == 200, r.body);
  CHECK(free_of(fx.server.master_balances(), "USDT") == q("750"));
  CHECK(free_of(fx.server.balances("b"), "USDT") == q("50250"));

  // History by clientTranId: the payer's view, the receiver's view and the master's.
  r = rest(master, "GET", kTransfer, {{"clientTranId", "x1"}, {"fromEmail", "a@pool.test"}});
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"("fromEmail":"a@pool.test","toEmail":"b@pool.test","asset":"BTC")"));
  CHECK(contains(r.body, R"("amount":"0.40000000")"));
  CHECK(contains(r.body, R"("status":"SUCCESS","clientTranId":"x1"}],"totalCount":1})"));
  r = rest(master, "GET", kTransfer, {{"toEmail", "b@pool.test"}});
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"("totalCount":2)"));
  r = rest(master, "GET", kTransfer, {{"clientTranId", "m1"}});
  REQUIRE(r.ok());
  CHECK(contains(r.body, R"("fromEmail":"master@sim.local")"));
  r = rest(master, "GET", kTransfer, {{"clientTranId", "nope"}});
  REQUIRE(r.ok());
  CHECK(r.body == R"({"totalCount":0})");
  r = rest(master, "GET", kTransfer, {{"fromEmail", "a@pool.test"}, {"toEmail", "b@pool.test"}});
  CHECK(r.status == 400);

  // Bad requests.
  r = rest(master, "POST", kTransfer, transfer_params("a@pool.test", "nobody@x", "USDT", "1", ""));
  CHECK(r.status == 400);
  CHECK(contains(r.body, "Invalid toEmail."));
  r = rest(
      master, "POST", kTransfer, transfer_params("a@pool.test", "a@pool.test", "USDT", "1", ""));
  CHECK(r.status == 400);
  Params futures = transfer_params("a@pool.test", "b@pool.test", "USDT", "1", "");
  futures[3].second = "USDT_FUTURE";
  r = rest(master, "POST", kTransfer, futures);
  CHECK(r.status == 400);
  CHECK(contains(r.body, "only SPOT to SPOT"));
  const sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.transfers_done == 2);
  CHECK(st.transfers_refused == 5);
}

TEST_CASE("sim_pool: the master key trades nothing and a trading key transfers nothing") {
  ServerFixture fx(pool_config());
  SignedRest rest(fx);
  const Signer master = hmac_signer(kMasterKey, kMasterSecret);
  const Signer a = hmac_signer("a-key", "a-secret");
  HttpReply r = rest(master, "POST", "/api/v3/order", limit_maker("BUY", "50000", "0.01"));
  CHECK(r.status == 401);
  CHECK(contains(r.body, R"("code":-2015)"));
  r = rest(master, "GET", "/api/v3/account");
  CHECK(r.status == 401);
  r = rest(a, "POST", kTransfer, transfer_params("a@pool.test", "b@pool.test", "USDT", "1", ""));
  CHECK(r.status == 401);
  CHECK(contains(r.body, R"("code":-2015)"));
  CHECK(fx.server.stats().orders_accepted == 0);

  // Without [sim.master] every transfer is refused.
  SimServerConfig no_master = pool_config();
  no_master.master = {};
  ServerFixture fx2(no_master);
  SignedRest rest2(fx2);
  r = rest2(
      master, "POST", kTransfer, transfer_params("a@pool.test", "b@pool.test", "USDT", "1", ""));
  CHECK(r.status == 401);
}

TEST_CASE("sim_pool: transfer faults") {
  const Signer master = hmac_signer(kMasterKey, kMasterSecret);
  const Params a_to_b = transfer_params("a@pool.test", "b@pool.test", "USDT", "10", "");
  auto with_id = [&](std::string id) {
    Params p = a_to_b;
    p.emplace_back("clientTranId", std::move(id));
    return p;
  };
  auto history_status = [&](SignedRest& rest, std::string_view id) {
    const HttpReply h = rest(master,
                             "GET",
                             kTransfer,
                             {{"clientTranId", std::string(id)}, {"fromEmail", "a@pool.test"}});
    REQUIRE(h.ok());
    for (const char* s : {"SUCCESS", "PROCESS", "FAILURE"}) {
      if (contains(h.body, std::string(R"("status":")") + s + "\"")) return std::string(s);
    }
    return std::string("none");
  };

  SUBCASE("transfer_p_fail: refused, nothing moves, not in the history") {
    SimServerConfig cfg = pool_config();
    cfg.faults.transfer.p_fail = 1.0;
    ServerFixture fx(cfg);
    SignedRest rest(fx);
    const HttpReply r = rest(master, "POST", kTransfer, with_id("f1"));
    CHECK(r.status == 400);
    CHECK(contains(r.body, R"("code":-1000)"));
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("100000"));
    CHECK(history_status(rest, "f1") == "none");
    // At run time: off again, and the same id goes through.
    fx.server.set_transfer_faults({});
    CHECK(rest(master, "POST", kTransfer, with_id("f1")).ok());
    CHECK(history_status(rest, "f1") == "SUCCESS");
  }

  SUBCASE("transfer_p_lost: carried out; the answer is dropped, then a 503") {
    SimServerConfig cfg = pool_config();
    cfg.faults.transfer.p_lost = 1.0;
    ServerFixture fx(cfg);
    BlockingHttpOptions o;
    o.timeout_ms = 400;
    SignedRest rest(fx, o);
    HttpReply r = rest(master, "POST", kTransfer, with_id("l1"));
    CHECK(r.status == 0);  // no answer: the client times out
    r = rest(master, "POST", kTransfer, with_id("l2"));
    CHECK(r.status == 503);
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("99980"));
    CHECK(free_of(fx.server.balances("b"), "USDT") == q("50020"));
    CHECK(history_status(rest, "l1") == "SUCCESS");
    CHECK(history_status(rest, "l2") == "SUCCESS");
    CHECK(fx.server.stats().transfers_lost == 2);
  }

  SUBCASE("transfer_delay_ms: PROCESS, then SUCCESS or FAILURE when its turn comes") {
    SimServerConfig cfg = pool_config();
    cfg.faults.transfer.delay_ms = 300;
    ServerFixture fx(cfg);
    SignedRest rest(fx);
    HttpReply r = rest(master, "POST", kTransfer, with_id("d1"));
    REQUIRE(r.ok());
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("100000"));
    CHECK(history_status(rest, "d1") == "PROCESS");
    REQUIRE(wait_until([&] { return history_status(rest, "d1") == "SUCCESS"; }, 3000));
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("99990"));

    // Taken while the free balance covered it; an order locks the balance meanwhile.
    Params all = transfer_params("b@pool.test", "a@pool.test", "BTC", "1", "d2");
    REQUIRE(rest(master, "POST", kTransfer, all).ok());
    const Price ask = fx.server.stats().best_ask.price;
    std::string px;
    sim::server::append_decimal(px, ask + Price::from_int(500));
    REQUIRE(rest(ed25519_signer("b-key"), "POST", "/api/v3/order", limit_maker("SELL", px, "0.5"))
                .ok());
    REQUIRE(wait_until(
        [&] {
          const HttpReply h = rest(
              master, "GET", kTransfer, {{"clientTranId", "d2"}, {"fromEmail", "b@pool.test"}});
          return contains(h.body, R"("status":"FAILURE")");
        },
        3000));
    CHECK(free_of(fx.server.balances("b"), "BTC") == q("0.5"));
    CHECK(fx.server.stats().transfers_failed_late == 1);
  }

  SUBCASE("transfer_ban_after: 429 / -1003 past N transfers in a minute") {
    SimServerConfig cfg = pool_config();
    cfg.faults.transfer.ban_after = 2;
    ServerFixture fx(cfg);
    SignedRest rest(fx);
    wait_for_window_room(fx, 60'000, 10'000);
    CHECK(rest(master, "POST", kTransfer, a_to_b).ok());
    CHECK(rest(master, "POST", kTransfer, a_to_b).ok());
    const HttpReply r = rest(master, "POST", kTransfer, a_to_b);
    CHECK(r.status == 429);
    CHECK(contains(r.body, R"("code":-1003)"));
    CHECK_FALSE(r.header("Retry-After").empty());
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("99980"));
    CHECK(fx.server.stats().transfers_rate_limited == 1);
  }

  SUBCASE("the master's UID weight: 360 a transfer against the limit") {
    SimServerConfig cfg = pool_config();
    cfg.uid_weight_limit_per_minute = 720;
    ServerFixture fx(cfg);
    SignedRest rest(fx);
    wait_for_window_room(fx, 60'000, 10'000);
    HttpReply r = rest(master, "POST", kTransfer, a_to_b);
    CHECK(r.ok());
    r = rest(master, "POST", kTransfer, a_to_b);
    CHECK(r.header("X-SAPI-USED-UID-WEIGHT-1M") == "720");
    r = rest(master, "POST", kTransfer, a_to_b);
    CHECK(r.status == 429);
    CHECK(contains(r.body, "current limit of UID is 720"));
  }

  SUBCASE("transfer_refused_assets: always -9000") {
    SimServerConfig cfg = pool_config();
    cfg.faults.transfer.refused_assets = {"BTC"};
    ServerFixture fx(cfg);
    SignedRest rest(fx);
    const HttpReply r = rest(
        master, "POST", kTransfer, transfer_params("a@pool.test", "b@pool.test", "BTC", "0.1", ""));
    CHECK(r.status == 400);
    CHECK(contains(r.body, R"("code":-9000)"));
    CHECK(rest(master, "POST", kTransfer, a_to_b).ok());
  }

  SUBCASE("the draws follow the seed") {
    auto outcomes = [&](std::uint64_t seed) {
      SimServerConfig cfg = pool_config();
      cfg.seed = seed;
      cfg.faults.transfer.p_fail = 0.5;
      ServerFixture fx(cfg);
      SignedRest rest(fx);
      std::string out;
      for (int i = 0; i < 24; ++i) out += rest(master, "POST", kTransfer, a_to_b).ok() ? '1' : '0';
      return out;
    };
    const std::string first = outcomes(11);
    CHECK(first == outcomes(11));
    CHECK(first.find('0') != std::string::npos);
    CHECK(first.find('1') != std::string::npos);
  }
}

TEST_CASE("sim_pool: FastMM's TransferClient moves and looks up transfers, HMAC and Ed25519") {
  for (const bool ed25519 : {false, true}) {
    CAPTURE(ed25519);
    SimServerConfig cfg = pool_config();
    TransferSettings s;
    s.venue = "sim";
    s.credentials.api_key = kMasterKey;
    if (ed25519) {
      cfg.master.api_secret.clear();
      cfg.master.ed25519_public_key_pem = fastmm::test::fixture("binance/ed25519-test-public.pem");
      s.credentials.type = binance::KeyType::Ed25519;
      s.credentials.private_key_pem.value =
          fastmm::test::fixture("binance/ed25519-test-private.pem");
    } else {
      s.credentials.secret.value = kMasterSecret;
    }
    cfg.faults.transfer.p_lost = 0.0;
    ServerFixture fx(cfg);
    s.rest_url = fx.http();
    s.http_timeout_ms = 500;
    TransferClient client(s);
    REQUIRE(client.usable());

    TransferRequest req;
    req.client_id = ed25519 ? "edpool1" : "hmacpool1";
    req.from_account = "a@pool.test";
    req.to_account = "b@pool.test";
    req.asset = "USDT";
    req.amount = Notional::from_decimal("1234.5").value();
    req.created_ms = fx.server.server_time_ms();
    const TransferResult sent = client.transfer(req);
    REQUIRE_MESSAGE(sent.state == TransferState::Pending, sent.detail);
    CHECK_FALSE(sent.venue_ref.empty());
    const TransferResult done = client.status(req);
    CHECK(done.state == TransferState::Done);
    CHECK(done.venue_ref == sent.venue_ref);
    CHECK(free_of(fx.server.balances("a"), "USDT") == q("98765.5"));
    CHECK(free_of(fx.server.balances("b"), "USDT") == q("51234.5"));

    // The master's wallet pays (no email), and an id never sent is not found.
    TransferRequest from_master = req;
    from_master.client_id += "m";
    from_master.from_account.clear();
    from_master.amount = Notional::from_int(100);
    CHECK(client.transfer(from_master).state == TransferState::Pending);
    CHECK(client.status(from_master).state == TransferState::Done);
    TransferRequest never = req;
    never.client_id = "neversent";
    CHECK(client.status(never).state == TransferState::NotFound);

    // Refused for balance: Failed.
    TransferRequest too_much = req;
    too_much.client_id += "x";
    too_much.amount = Notional::from_int(10'000'000);
    const TransferResult refused = client.transfer(too_much);
    CHECK(refused.state == TransferState::Failed);
    CHECK(contains(refused.detail, "-9000"));

    // A lost answer: Unknown, and the history says it happened.
    sim::server::TransferFaultConfig lose;
    lose.p_lost = 1.0;
    fx.server.set_transfer_faults(lose);
    for (int i = 0; i < 2; ++i) {
      TransferRequest lost = req;
      lost.client_id += "l" + std::to_string(i);
      lost.amount = Notional::from_int(1);
      CHECK(client.transfer(lost).state == TransferState::Unknown);
      CHECK(client.status(lost).state == TransferState::Done);
    }
    CHECK(fx.server.stats().signature_errors == 0);
  }
}

TEST_CASE("sim_pool: [sim.accounts.*], [sim.master] and the transfer faults from TOML") {
  const std::string text = R"(
[venues.sim]
kind = "sim"

[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
asset_class = "spot"
tick = "0.01"
lot = "0.00001"

[sim.accounts.beta]
api_key = "kb"
api_secret = "sb"

[sim.accounts.alpha]
api_key = "ka"
api_secret = "sa"
email = "alpha@desk.test"
balances.USDT = "500"

[sim.master]
api_key = "km"
api_secret = "sm"
balances.BTC = "3"

[sim.limits]
uid_weight_per_minute = 3600

[sim.faults]
transfer_p_fail = 0.1
transfer_p_lost = 0.2
transfer_delay_ms = 50
transfer_ban_after = 9
transfer_refused_assets = ["BNB", "ETH"]
)";
  Config::LoadOptions opts;
  opts.substitute_env = false;
  const SimServerConfig c = SimServerConfig::from_config(Config::parse(text, opts));
  REQUIRE(c.accounts.size() == 2);
  CHECK(c.accounts[0].name == "alpha");
  CHECK(c.accounts[0].email == "alpha@desk.test");
  REQUIRE(c.accounts[0].balances.size() == 1);
  CHECK(c.accounts[0].balances[0].amount == q("500"));
  CHECK(c.accounts[1].name == "beta");
  CHECK(c.accounts[1].email.empty());
  CHECK(c.master.api_key == "km");
  CHECK(c.master.balances.size() == 1);
  CHECK(c.uid_weight_limit_per_minute == 3600);
  CHECK(c.faults.transfer.p_fail == doctest::Approx(0.1));
  CHECK(c.faults.transfer.p_lost == doctest::Approx(0.2));
  CHECK(c.faults.transfer.delay_ms == 50);
  CHECK(c.faults.transfer.ban_after == 9);
  CHECK(c.faults.transfer.refused_assets == std::vector<std::string>{"BNB", "ETH"});
  sim::server::SimExchangeServer server(c);
  CHECK(server.account_names() == std::vector<std::string>{"alpha", "beta"});
  CHECK(free_of(server.balances("beta"), "BTC") == q("100"));  // the default balances

  // [sim.account] with a pool, a typo, a probability out of range: refused.
  std::string out_of_range = text;
  out_of_range.replace(out_of_range.find("= 0.1"), 5, "= 1.5");
  for (const std::string& bad : {text + "[sim.account]\napi_key = \"x\"\n",
                                 text + "[sim.accounts.gamma]\napi_kye = \"x\"\n",
                                 out_of_range}) {
    CAPTURE(bad);
    CHECK_THROWS_AS(SimServerConfig::from_config(Config::parse(bad, opts)), std::invalid_argument);
  }

  // The example pool loads and serves five accounts.
  const SimServerConfig pool =
      SimServerConfig::load((repo_root() / "configs" / "sim-pool.toml").string());
  sim::server::SimExchangeServer pool_server(pool);
  CHECK(pool_server.account_names().size() == 5);
}
