// Several strategy processes attached to one fastmm-gateway at once, each trading its own
// instrument: the simulator lists two BTC markets (BTCUSDT and BTCUSDC, same filters), strategy
// "a" trades the first, strategy "b" the second. The gateway and the strategies are real children.
#include "gateway_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/live/gateway.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

sim::server::SimServerConfig two_markets() {
  sim::server::SimServerConfig c = test_server_config();
  sim::server::SimSymbolConfig s = c.symbols.front();
  s.symbol = "BTCUSDC";
  s.quote_asset = "USDC";
  c.symbols.push_back(s);
  return c;
}

constexpr const char* kUsdcInstrument = R"(
[[instruments]]
venue = "sim"
symbol = "BTCUSDC"
base = "BTC"
quote = "USDC"
asset_class = "spot"
tick = "0.01"
lot = "0.00001"
min_qty = "0.00001"
max_qty = "100"
min_notional = "5"
enabled = true
)";

template <class Edit>
void rewrite(const std::string& path, Edit&& edit) {
  std::string text = fastmm::test::read_file(path);
  edit(text);
  std::ofstream out(path, std::ios::trunc);
  REQUIRE(out.good());
  out << text;
}

void replace_first(std::string& text, std::string_view from, std::string_view to) {
  const std::size_t p = text.find(from);
  REQUIRE(p != std::string::npos);
  text.replace(p, from.size(), to);
}

// The gateway's configuration lists both markets (and `gateway_extra`); a's lists BTCUSDT, b's
// BTCUSDC.
struct Configs {
  SessionFiles gw;
  SessionFiles a;
  SessionFiles b;
  std::string a_name;
  std::string b_name;
};

Configs write_configs(const ServerFixture& fx,
                      const std::string& stem,
                      const std::string& gateway_extra = {}) {
  Configs c;
  c.gw = write_config(fx, stem + "-gw", "exit", "1000");
  rewrite(c.gw.config, [&](std::string& t) { t += kUsdcInstrument + gateway_extra; });
  c.a_name = stem + "-a";
  c.b_name = stem + "-b";
  c.a = write_config(fx, c.a_name, "exit", "1000");
  c.b = write_config(fx, c.b_name, "exit", "1000");
  rewrite(c.b.config, [&](std::string& t) {
    replace_first(t, R"(symbol = "BTCUSDT")", R"(symbol = "BTCUSDC")");
    replace_first(t, R"(quote = "USDT")", R"(quote = "USDC")");
  });
  for (const SessionFiles* f : {&c.gw, &c.a, &c.b})
    remove_all_of({f->epoch, f->kill, f->journal_dir, f->config + ".log", f->status});
  return c;
}

std::uint64_t fills(const ServerFixture& fx, std::size_t symbol) {
  const sim::server::SimServerStats s = fx.server.stats();
  return symbol < s.symbol_fills.size() ? s.symbol_fills[symbol] : 0;
}
Qty position(const sim::server::SimServerStats& s, std::size_t symbol) {
  return symbol < s.symbol_positions.size() ? s.symbol_positions[symbol] : Qty{};
}

// The epochs of the orders resting at the simulator, one entry per order.
std::vector<std::uint16_t> open_epochs(const ServerFixture& fx) {
  std::vector<std::uint16_t> out;
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    out.push_back(cl ? cl_ord_id_epoch(*cl) : std::uint16_t{0});
  }
  return out;
}
std::size_t open_of(const ServerFixture& fx, std::uint16_t epoch) {
  const std::vector<std::uint16_t> e = open_epochs(fx);
  return static_cast<std::size_t>(std::count(e.begin(), e.end(), epoch));
}

// Waits until `f`'s strategy has orders resting and returns their epoch (none of `others`).
std::uint16_t wait_resting(const ServerFixture& fx,
                           const SessionFiles& f,
                           std::vector<std::uint16_t> others) {
  std::uint16_t epoch = 0;
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        for (const std::uint16_t e : open_epochs(fx)) {
                          if (std::find(others.begin(), others.end(), e) == others.end()) {
                            epoch = e;
                            return true;
                          }
                        }
                        return false;
                      },
                      30000),
                  "no orders resting: " << fastmm::test::read_file(f.config + ".log"));
  return epoch;
}

void stop_strategy(pid_t pid) {
  REQUIRE(::kill(pid, SIGTERM) == 0);
  CHECK(reap(pid) == live::kExitOk);
}

// What each session's engine was given names its own orders only.
void check_journals(const SessionFiles& f, std::uint16_t forbidden_epoch) {
  const std::vector<JournalEpochs> js = read_journals(f);
  REQUIRE(!js.empty());
  for (const JournalEpochs& j : js) {
    INFO(j.path << " epoch " << j.epoch);
    CHECK(j.own > 0);
    CHECK(j.crossed.empty());
    for (const std::string& c : j.crossed) MESSAGE("crossed: " << c);
    CHECK(!j.fill_epochs.contains(forbidden_epoch));
  }
}

}  // namespace

TEST_CASE(
    "gateway: two strategies attached at once trade their own instruments and never see each "
    "other's orders") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-two");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const Sessions opened = sessions_opened(fx);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  CHECK(ea != eb);
  REQUIRE_MESSAGE(wait_until([&] { return fills(fx, 0) >= 3 && fills(fx, 1) >= 3; }, 60000),
                  "not both traded: a: " << fastmm::test::read_file(c.a.config + ".log") << "\nb: "
                                         << fastmm::test::read_file(c.b.config + ".log"));

  // A third strategy claiming an instrument a live one trades is refused; so is one the gateway
  // does not have. The two keep trading.
  {
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = "intruder";
    req.instruments = {{"sim", "BTCUSDC"}};
    CHECK(live::GatewayClient::attach(g.socket, req, &err) == nullptr);
    CHECK(err.find("BTCUSDC on venue 'sim' is traded by gw-two-b") != std::string::npos);
    req.instruments = {{"sim", "ETHUSDT"}};
    CHECK(live::GatewayClient::attach(g.socket, req, &err) == nullptr);
    CHECK(err.find("not in the gateway's configuration") != std::string::npos);
  }
  const std::uint64_t fa = fills(fx, 0);
  const std::uint64_t fb = fills(fx, 1);
  CHECK(wait_until([&] { return fills(fx, 0) >= fa + 2 && fills(fx, 1) >= fb + 2; }, 60000));

  stop_strategy(a);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue BTCUSDT " << position(ss, 0).raw << " BTCUSDC " << position(ss, 1).raw);
  CHECK(store_position(c.a, c.a_name, "BTCUSDT") == position(ss, 0));
  CHECK(store_position(c.b, c.b_name, "BTCUSDC") == position(ss, 1));
  CHECK(ss.duplicate_client_order_ids == 0);
  check_journals(c.a, eb);
  check_journals(c.b, ea);
  for (const SessionFiles* f : {&c.a, &c.b})
    for (const JournalEpochs& j : read_journals(*f)) CHECK(j.unsolicited_cancel_acks == 0);
  const Sessions after = sessions_opened(fx);
  CHECK(after.md == opened.md);
  CHECK(after.api == opened.api);
  stop_gateway(g);
}

TEST_CASE(
    "gateway: kill -9 of one strategy cancels its orders only, the other keeps trading and a "
    "replacement reconciles its own") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-kill");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const Sessions opened = sessions_opened(fx);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  REQUIRE(wait_until([&] { return fills(fx, 0) >= 1 && fills(fx, 1) >= 1; }, 60000));
  REQUIRE(wait_until([&] { return open_of(fx, ea) > 0 && open_of(fx, eb) > 0; }, 20000));

  REQUIRE(::kill(a, SIGKILL) == 0);
  const auto killed_at = std::chrono::steady_clock::now();
  const bool cleared = wait_until([&] { return open_of(fx, ea) == 0; }, 2000);
  const auto cleared_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - killed_at)
                              .count();
  CHECK(reap(a) == -1);
  CHECK_MESSAGE(cleared, "a's orders still resting 2 s after it died");
  MESSAGE("kill -9 -> none of a's orders at the venue in " << cleared_ms << " ms");
  // b's quotes stay: the gateway cancelled a's epoch, not the account.
  CHECK(open_of(fx, eb) > 0);
  const std::uint64_t fb = fills(fx, 1);
  CHECK_MESSAGE(wait_until([&] { return fills(fx, 1) >= fb + 2; }, 60000),
                "b stopped trading: " << fastmm::test::read_file(c.b.config + ".log"));
  Sessions now = sessions_opened(fx);
  CHECK(now.md == opened.md);
  CHECK(now.api == opened.api);

  // a's replacement: restores its position, gets the executions since from the gateway, trades.
  const std::uint64_t fa = fills(fx, 0);
  const pid_t a2 = spawn_strategy(c.a, g);
  const std::uint16_t ea2 = wait_resting(fx, c.a, {ea, eb});
  CHECK(ea2 != ea);
  CHECK_MESSAGE(wait_until([&] { return fills(fx, 0) > fa; }, 60000),
                "the replacement never traded: " << fastmm::test::read_file(c.a.config + ".log"));

  stop_strategy(a2);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue BTCUSDT " << position(ss, 0).raw << " BTCUSDC " << position(ss, 1).raw);
  CHECK(store_position(c.a, c.a_name, "BTCUSDT") == position(ss, 0));
  CHECK(store_position(c.b, c.b_name, "BTCUSDC") == position(ss, 1));
  CHECK(ss.duplicate_client_order_ids == 0);
  check_journals(c.b, ea);
  check_journals(c.b, ea2);
  // Nobody but b cancelled b's orders.
  for (const JournalEpochs& j : read_journals(c.b)) CHECK(j.unsolicited_cancel_acks == 0);
  // The replacement reconciled only its own orders (a reconcile row of another epoch would be in
  // `crossed`) and never saw b's fills.
  for (const JournalEpochs& j : read_journals(c.a)) {
    INFO(j.path);
    CHECK(j.crossed.empty());
    CHECK(!j.fill_epochs.contains(eb));
  }
  now = sessions_opened(fx);
  CHECK(now.md == opened.md);
  CHECK(now.api == opened.api);
  stop_gateway(g);
  const std::string gw_log = fastmm::test::read_file(g.log);
  CHECK(gw_log.find("epoch " + std::to_string(ea) + ") detached after") != std::string::npos);
}

TEST_CASE(
    "gateway: a fill the private stream missed reaches its strategy through another one's attach "
    "replay") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-missed");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);

  // b trades once and stops, so that its next attach restores from its store and replays.
  {
    const pid_t b = spawn_strategy(c.b, g);
    REQUIRE_MESSAGE(wait_until(
                        [&] {
                          if (fills(fx, 1) < 1) return false;
                          auto st = KillStateStore::load(c.b.kill);
                          return st && st->fees.is_positive();
                        },
                        60000),
                    "b never traded: " << fastmm::test::read_file(c.b.config + ".log"));
    stop_strategy(b);
  }

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  // One of a's resting orders fills while a's private stream is out: a hears nothing of it.
  std::string wire;
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == ea) wire = id;
  }
  REQUIRE(!wire.empty());
  fx.server.set_user_stream_muted(true);
  const Qty filled = fx.server.fill_open_order(wire);
  REQUIRE(filled.is_positive());
  fx.server.set_user_stream_muted(false);
  // b's attach replays the account's executions since its last stored fill, which includes it.
  const pid_t b = spawn_strategy(c.b, g);
  wait_resting(fx, c.b, {ea});
  std::this_thread::sleep_for(std::chrono::seconds(2));  // a's engine takes the fill

  stop_strategy(b);
  stop_strategy(a);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue BTCUSDT " << position(ss, 0).raw);
  CHECK(store_position(c.a, c.a_name, "BTCUSDT") == position(ss, 0));
  // a booked it from the execution itself: at its order's price, with the venue's fee.
  const std::optional<ClientOrderId> want = decode_cl_ord_id(wire);
  bool booked = false;
  for (const auto& e : std::filesystem::directory_iterator(c.a.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    Price order_px{};
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) != 0) {
        if (h->type == EventType::OutNewOrder && msg_cast<OutNewOrderMsg>(h).cl_ord_id == *want)
          order_px = msg_cast<OutNewOrderMsg>(h).price;
        if (h->type == EventType::OutReplace && msg_cast<OutReplaceMsg>(h).cl_ord_id == *want)
          order_px = msg_cast<OutReplaceMsg>(h).price;
        return;
      }
      if (h->type != EventType::OrderFill) return;
      const auto& f = msg_cast<OrderFillMsg>(h);
      if (f.cl_ord_id != *want || (f.flags & OrderFillMsg::kReplayed) == 0) return;
      CHECK(f.qty == filled);
      CHECK(f.price == order_px);
      CHECK(f.fee.is_positive());
      booked = true;
    });
  }
  CHECK_MESSAGE(booked, "a never booked the replayed fill of " << wire);
  stop_gateway(g);
}

TEST_CASE(
    "gateway: another attach's replay reaching back before a strategy's store does not book its "
    "old fills again") {
  ServerFixture fx(two_markets());
  const Configs c = write_configs(fx, "gw-old");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const auto traded = [&](const SessionFiles& f, std::size_t symbol) {
    if (fills(fx, symbol) < 1) return false;
    auto st = KillStateStore::load(f.kill);
    return st && st->fees.is_positive();
  };

  // b trades and stops: its store ends before anything a does.
  {
    const pid_t b = spawn_strategy(c.b, g);
    REQUIRE_MESSAGE(wait_until([&] { return traded(c.b, 1); }, 60000),
                    "b never traded: " << fastmm::test::read_file(c.b.config + ".log"));
    stop_strategy(b);
  }
  // a trades for longer than the 10 s its next session's replay reaches back, and stops.
  {
    const pid_t a = spawn_strategy(c.a, g);
    REQUIRE_MESSAGE(wait_until([&] { return traded(c.a, 0); }, 60000),
                    "a never traded: " << fastmm::test::read_file(c.a.config + ".log"));
    const std::uint64_t first = fills(fx, 0);
    std::this_thread::sleep_for(std::chrono::seconds(15));
    REQUIRE(fills(fx, 0) > first);
    stop_strategy(a);
  }
  const Qty before = position(fx.server.stats(), 0);
  REQUIRE(store_position(c.a, c.a_name, "BTCUSDT") == before);

  // a's replacement restores from its store; its replay starts 10 s before a's last fill.
  const pid_t a2 = spawn_strategy(c.a, g);
  const std::uint16_t ea2 = wait_resting(fx, c.a, {});
  // b comes back: its replay starts before every fill of a's first session, which name an epoch no
  // attachment holds and so go to BTCUSDT's owner, a2, whose store has them.
  const pid_t b2 = spawn_strategy(c.b, g);
  wait_resting(fx, c.b, {ea2});
  std::this_thread::sleep_for(std::chrono::seconds(2));

  stop_strategy(b2);
  stop_strategy(a2);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue BTCUSDT " << position(ss, 0).raw << ", before a2 " << before.raw);
  CHECK(store_position(c.a, c.a_name, "BTCUSDT") == position(ss, 0));
  CHECK(store_position(c.b, c.b_name, "BTCUSDC") == position(ss, 1));
  stop_gateway(g);
  CHECK(fastmm::test::read_file(g.log).find("older than its owner's history") != std::string::npos);
}

TEST_CASE("gateway: the open-notional limit refuses an order back to the strategy that sent it") {
  ServerFixture fx(two_markets());
  // One quote is 0.0008-0.001 BTC at about 60000 (48-60): two quotes fit, a third does not, so
  // once both strategies quote, one of them has an order refused.
  const Configs c = write_configs(fx, "gw-limit", "\n[gateway]\nmax_open_notional = \"130\"\n");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  REQUIRE(wait_until([&] { return open_of(fx, ea) >= 2; }, 20000));
  const pid_t b = spawn_strategy(c.b, g);
  // The gateway's once-a-second counters (no assertion inside the predicate: doctest does not
  // nest them).
  // "refused: rate=0 open_notional=<n>" with n > 0.
  const auto counted = [](const std::string& text) {
    const std::string key = "refused: rate=0 open_notional=";
    for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
      const std::size_t d = at + key.size();
      if (d < text.size() && text[d] >= '1' && text[d] <= '9') return true;
    }
    return false;
  };
  const bool refused = wait_until(
      [&] {
        std::ifstream in(g.log);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        return counted(text);
      },
      30000);
  CHECK_MESSAGE(refused, "no order refused: " << fastmm::test::read_file(g.log));
  // At no point did the account work more than the limit.
  CHECK(fx.server.stats().max_open_orders <= 2);

  stop_strategy(b);
  stop_strategy(a);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  // Whichever of the two had its order refused (the one whose order came second), the reject
  // went to it: every reject a strategy was given names one of its own orders.
  std::size_t refusals = 0;
  for (const SessionFiles* f : {&c.a, &c.b}) {
    for (const JournalEpochs& j : read_journals(*f)) {
      INFO(j.path);
      CHECK(j.crossed.empty());
      refusals += j.gateway_rejects;
    }
  }
  CHECK(refusals > 0);
  stop_gateway(g);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
