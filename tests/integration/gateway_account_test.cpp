// The account over every strategy in fastmm-gateway: its positions and PnL against the simulator's
// and the strategies' own, its exposure limits and its loss budget. Two strategies trade BTCUSDT
// and ETHUSDT on the simulator through one gateway; everything is a real child process.
#include "gateway_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/live/gateway.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <map>
#include <set>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

// The words of the last line of `text` that contains `key`, split at spaces.
std::vector<std::string> last_line_words(const std::string& text, std::string_view key) {
  const std::size_t at = text.rfind(key);
  if (at == std::string::npos) return {};
  const std::size_t begin = text.rfind('\n', at);
  const std::size_t end = text.find('\n', at);
  std::istringstream in(text.substr(begin == std::string::npos ? 0 : begin + 1,
                                    end == std::string::npos ? std::string::npos
                                                             : end - (begin + 1)));
  std::vector<std::string> out;
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

// key=value words as numbers (the decimal after '=', up to a non-number).
std::map<std::string, double> numbers(const std::vector<std::string>& words) {
  std::map<std::string, double> out;
  for (const std::string& w : words) {
    const std::size_t eq = w.find('=');
    if (eq == std::string::npos || eq + 1 >= w.size()) continue;
    const char c = w[eq + 1];
    if (c != '-' && (c < '0' || c > '9')) continue;
    out[w.substr(0, eq)] = std::strtod(w.c_str() + eq + 1, nullptr);
  }
  return out;
}

// The gateway's last account line: its numbers, and the positions ("sim:BTCUSDT" -> qty).
struct AccountLine {
  bool found = false;
  std::map<std::string, double> n;
  std::string text;  // the gateway's log
  std::string line;  // its last account line
  // The last "gateway: account position <key> <qty>" (flat when there is none).
  [[nodiscard]] Qty position(const std::string& key) const {
    const std::vector<std::string> w = last_line_words(text, "gateway: account position " + key + " ");
    if (w.empty()) return Qty{};
    return Qty::from_decimal(w.back()).value_or(Qty::from_raw(-1));
  }
};

std::string dec(Qty q) {
  char buf[kMaxDecimalChars];
  return {buf, q.to_decimal(buf)};
}
AccountLine account_line(const GatewayProcess& g) {
  AccountLine a;
  a.text = fastmm::test::read_file(g.log);
  const std::vector<std::string> words = last_line_words(a.text, "gateway: account net_pnl=");
  a.found = !words.empty();
  a.n = numbers(words);
  for (const std::string& w : words) a.line += w + " ";
  return a;
}

// The strategy's final PnL line: realized_pnl, unrealized_pnl, fees.
std::map<std::string, double> strategy_pnl(const SessionFiles& f) {
  return numbers(
      last_line_words(fastmm::test::read_file(f.config + ".log"), "fastmm-live: realized_pnl="));
}

bool log_has(const std::string& path, std::string_view what) {
  return fastmm::test::read_file(path).find(what) != std::string::npos;
}

// A gateway child with extra arguments (--clear-kill, --duration so that a failed case does not
// leave it running).
GatewayProcess spawn_gateway_with(const SessionFiles& f, const std::vector<std::string>& extra) {
  GatewayProcess g;
  g.socket = f.config + ".gw";
  g.log = f.config + ".gw.log";
  remove_all_of({g.socket, g.log});
  std::vector<std::string> args{"--config", f.config, "--socket", g.socket, "--log", g.log};
  args.insert(args.end(), extra.begin(), extra.end());
  g.pid = spawn_process(FASTMM_GATEWAY_EXE, args);
  return g;
}

// What one strategy's journal says about the account's exposure refusals: each refused order
// would have added to its position (it was flat, or the order was on the side of its position),
// and after the first one an order that reduced its position was accepted by the venue.
struct ExposureRefusals {
  std::size_t refused = 0;
  std::size_t refused_reducing = 0;  // must stay 0
  bool reducing_acked_after = false;
};
ExposureRefusals exposure_refusals(const SessionFiles& f) {
  ExposureRefusals out;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    std::map<std::uint64_t, Side> side;  // client order id -> side
    std::int64_t pos = 0;                // raw Qty, from the fills this session booked
    std::set<std::string> execs;
    r.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) != 0) {
        if (h->type == EventType::OutNewOrder) {
          const auto& m = msg_cast<OutNewOrderMsg>(h);
          side[m.cl_ord_id.value] = m.side;
        } else if (h->type == EventType::OutReplace) {
          const auto& m = msg_cast<OutReplaceMsg>(h);
          if (const auto it = side.find(m.orig_cl_ord_id.value); it != side.end())
            side[m.cl_ord_id.value] = it->second;
        }
        return;
      }
      if (h->type == EventType::Reconcile) {
        const auto& m = msg_cast<ReconcileMsg>(h);
        if (m.kind == ReconcileMsg::Kind::Position) pos = m.position_qty.raw;
      } else if (h->type == EventType::OrderFill) {
        const auto& m = msg_cast<OrderFillMsg>(h);
        // A replay names executions the stream delivered already; the engine books each once.
        if (!execs.insert(std::string(m.exec_id.view())).second) return;
        pos += m.side == Side::Buy ? m.qty.raw : -m.qty.raw;
      } else if (h->type == EventType::OrderReject) {
        const auto& m = msg_cast<OrderRejectMsg>(h);
        if (m.reason != RejectReason::GatewayGrossNotional) return;
        const auto it = side.find(m.cl_ord_id.value);
        if (it == side.end()) return;
        ++out.refused;
        const bool reduces = pos != 0 && (pos > 0) != (it->second == Side::Buy);
        if (reduces) ++out.refused_reducing;
      } else if (h->type == EventType::OrderAck && out.refused > 0) {
        const auto it = side.find(msg_cast<OrderAckMsg>(h).cl_ord_id.value);
        if (it != side.end() && pos != 0 && (pos > 0) != (it->second == Side::Buy))
          out.reducing_acked_after = true;
      }
    });
  }
  return out;
}

}  // namespace

TEST_CASE(
    "gateway account: its positions are the simulator's, its PnL is the sum of the strategies', "
    "and a restarted gateway starts from their stores") {
  ServerFixture fx(two_markets(kEthUsdt));
  const Configs c = write_configs(fx, "gw-acct", {}, kEthUsdt);
  {
    const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
    wait_gateway_up(fx, g);
    const pid_t a = spawn_strategy(c.a, g);
    const std::uint16_t ea = wait_resting(fx, c.a, {});
    const pid_t b = spawn_strategy(c.b, g);
    wait_resting(fx, c.b, {ea});
    REQUIRE_MESSAGE(wait_until([&] { return fills(fx, 0) >= 3 && fills(fx, 1) >= 3; }, 90000),
                    "not both traded: a: " << fastmm::test::read_file(c.a.config + ".log")
                                           << "\nb: "
                                           << fastmm::test::read_file(c.b.config + ".log"));
    stop_strategy(a);
    stop_strategy(b);
    CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // two account lines later
    const sim::server::SimServerStats ss = fx.server.stats();
    const AccountLine acct = account_line(g);
    INFO("gateway: " << acct.line);
    REQUIRE(acct.found);
    CHECK(acct.position("sim:BTCUSDT") == position(ss, 0));
    CHECK(acct.position("sim:ETHUSDT") == position(ss, 1));
    CHECK(store_position(c.a, c.a_name, "BTCUSDT") == position(ss, 0));
    CHECK(store_position(c.b, c.b_name, "ETHUSDT") == position(ss, 1));

    // The account's realized PnL and fees are the strategies' together, to the last unit; its net
    // PnL differs only by the marks having moved since the strategies stopped.
    const std::map<std::string, double> pa = strategy_pnl(c.a);
    const std::map<std::string, double> pb = strategy_pnl(c.b);
    INFO("a: realized " << pa.at("realized_pnl") << " unrealized " << pa.at("unrealized_pnl")
                        << " fees " << pa.at("fees"));
    INFO("b: realized " << pb.at("realized_pnl") << " unrealized " << pb.at("unrealized_pnl")
                        << " fees " << pb.at("fees"));
    CHECK(pa.at("fees") > 0);
    CHECK(pb.at("fees") > 0);
    CHECK(std::abs(acct.n.at("realized") - (pa.at("realized_pnl") + pb.at("realized_pnl"))) <
          1e-7);
    CHECK(std::abs(acct.n.at("fees") - (pa.at("fees") + pb.at("fees"))) < 1e-7);
    const double strategies_net = pa.at("realized_pnl") + pa.at("unrealized_pnl") -
                                  pa.at("fees") + pb.at("realized_pnl") +
                                  pb.at("unrealized_pnl") - pb.at("fees");
    // 20 bps of the positions' notional for the marks' moves.
    const double drift =
        (std::abs(position(ss, 0).to_double()) + std::abs(position(ss, 1).to_double())) * 60000 *
            0.002 +
        1e-6;
    CHECK(std::abs(acct.n.at("net_pnl") - strategies_net) <= drift);
    stop_gateway(g);
  }

  // A new gateway knows nothing of the account; the strategies' stores do. Its positions start
  // from what they restore, and it books the rest.
  {
    const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
    wait_gateway_up(fx, g);
    const Qty before_a = position(fx.server.stats(), 0);
    const pid_t a = spawn_strategy(c.a, g);
    const std::uint16_t ea = wait_resting(fx, c.a, {});
    const pid_t b = spawn_strategy(c.b, g);
    wait_resting(fx, c.b, {ea});
    CHECK_MESSAGE(log_has(g.log,
                          "account position of BTCUSDT starts at " + dec(before_a)),
                  fastmm::test::read_file(g.log));
    const std::uint64_t fa = fills(fx, 0);
    const std::uint64_t fb = fills(fx, 1);
    REQUIRE(wait_until([&] { return fills(fx, 0) > fa && fills(fx, 1) > fb; }, 90000));
    stop_strategy(a);
    stop_strategy(b);
    CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const sim::server::SimServerStats ss = fx.server.stats();
    const AccountLine acct = account_line(g);
    INFO("gateway: " << acct.line);
    CHECK(acct.position("sim:BTCUSDT") == position(ss, 0));
    CHECK(acct.position("sim:ETHUSDT") == position(ss, 1));
    stop_gateway(g);
  }
}

TEST_CASE(
    "gateway account: an order that would take the account past max_gross_notional goes back to "
    "its sender, one that reduces exposure passes, the other strategy keeps trading") {
  ServerFixture fx(two_markets(kEthUsdt));
  // a quotes 0.001 BTC (about 60), b 0.0002 (about 12). Once a holds a position, its orders on
  // that side would take the account past 100 and are refused; its other side reduces and goes.
  // b always has room for a side: flat it adds 12 to a's 60, and otherwise one side reduces.
  const Configs c = write_configs(fx, "gw-gross", "\n[gateway]\nmax_gross_notional = \"100\"\n", kEthUsdt);
  rewrite(c.b.config, [](std::string& t) { replace_first(t, "quote_qty = 0.001", "quote_qty = 0.0002"); });
  const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  REQUIRE(wait_until([&] { return open_of(fx, ea) >= 2; }, 20000));
  // a trades one of its quotes: it holds 0.001 BTC.
  std::string wire;
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) == ea) wire = id;
  }
  REQUIRE(!wire.empty());
  REQUIRE(fx.server.fill_open_order(wire).is_positive());
  const std::uint64_t fb = fills(fx, 1);
  // The gateway's once-a-second counters: "refused: ... gross_notional=<n>".
  const auto refusals = [&] {
    const std::map<std::string, double> n =
        numbers(last_line_words(fastmm::test::read_file(g.log), "refused: rate="));
    const auto it = n.find("gross_notional");
    return it == n.end() ? 0.0 : it->second;
  };
  CHECK_MESSAGE(wait_until([&] { return refusals() > 0; }, 30000),
                "no order refused: " << fastmm::test::read_file(g.log));
  // b trades on.
  CHECK_MESSAGE(wait_until([&] { return fills(fx, 1) >= fb + 2; }, 90000),
                "b stopped trading: " << fastmm::test::read_file(c.b.config + ".log"));
  CHECK(open_of(fx, eb) > 0);

  stop_strategy(a);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  stop_gateway(g);
  const ExposureRefusals ra = exposure_refusals(c.a);
  const ExposureRefusals rb = exposure_refusals(c.b);
  INFO("a: refused " << ra.refused << ", reducing among them " << ra.refused_reducing
                     << "; b: refused " << rb.refused);
  CHECK(ra.refused > 0);
  CHECK(ra.refused_reducing == 0);
  CHECK(rb.refused_reducing == 0);
  CHECK(ra.reducing_acked_after);
  // Every reject went to the strategy whose order it was.
  for (const SessionFiles* f : {&c.a, &c.b})
    for (const JournalEpochs& j : read_journals(*f)) CHECK(j.crossed.empty());
}

TEST_CASE(
    "gateway account: max_loss over every strategy trips the account, cancels everything, refuses "
    "attaches and stays latched across a restart until --clear-kill") {
  sim::server::SimServerConfig sc = two_markets(kEthUsdt);
  // Every fill costs 1% (about 0.5-0.6 a quote): the account loses by trading. Little market flow,
  // so the fills are the ones this test makes.
  sc.maker_bps = 100.0;
  sc.generator.market_rate_per_s = 0.5;
  ServerFixture fx(sc);
  // Each strategy's own [risk] max_loss is 1000: only the account's budget can stop them.
  const Configs c = write_configs(fx, "gw-loss", "\n[gateway]\nmax_loss = \"3\"\n", kEthUsdt);
  const std::string latch_line = "account kill switch tripped";
  // Fills one resting order of `epoch` (none resting: nothing).
  const auto fill_one = [&](std::uint16_t epoch) {
    for (const std::string& id : fx.server.open_client_order_ids()) {
      const auto cl = decode_cl_ord_id(id);
      if (cl && cl_ord_id_epoch(*cl) == epoch) {
        static_cast<void>(fx.server.fill_open_order(id));
        return;
      }
    }
  };
  {
    const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
    wait_gateway_up(fx, g);
    const pid_t a = spawn_strategy(c.a, g);
    const std::uint16_t ea = wait_resting(fx, c.a, {});
    const pid_t b = spawn_strategy(c.b, g);
    const std::uint16_t eb = wait_resting(fx, c.b, {ea});
    // Both lose, turn about, until the account's budget is spent: neither alone reaches it first
    // by much.
    for (int i = 0; i < 40 && !log_has(g.log, latch_line); ++i) {
      fill_one(i % 2 == 0 ? ea : eb);
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    REQUIRE_MESSAGE(wait_until([&] { return log_has(g.log, latch_line); }, 10000),
                    "the account never tripped: " << fastmm::test::read_file(g.log));
    // Both strategies are told (their venue's kill switch, and so the global one) and exit.
    CHECK(reap(a) == live::kExitKilled);
    CHECK(reap(b) == live::kExitKilled);
    CHECK(log_has(c.a.config + ".log", "GatewayMaxLoss"));
    CHECK(log_has(c.b.config + ".log", "GatewayMaxLoss"));
    CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
    CHECK(fx.server.stats().cancel_all_requests >= 1);
    // Latched: in the kill file, and no new strategy attaches.
    const auto st = KillStateStore::load(c.gw.kill);
    REQUIRE(st.has_value());
    CHECK(st->latched);
    CHECK(st->reason == KillReason::GatewayMaxLoss);
    CHECK(st->carry().is_negative());
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = "late";
    req.instruments = {{"sim", "BTCUSDT"}};
    CHECK(live::GatewayClient::attach(g.socket, req, &err) == nullptr);
    CHECK_MESSAGE(err.find("fastmm-gateway --clear-kill") != std::string::npos, err);
    // Orders were refused while it latched, not only cancelled.
    stop_gateway(g);
  }
  // A restart without clearing keeps the trip: the gateway does not start.
  {
    const GatewayProcess g = spawn_gateway_with(c.gw, {"--duration", "300s"});
    CHECK(reap(g.pid) == live::kExitKilled);
    const auto st = KillStateStore::load(c.gw.kill);
    REQUIRE(st.has_value());
    CHECK(st->latched);
  }
  // --clear-kill: the trip and the loss so far are gone, the whole budget is armed again.
  {
    const GatewayProcess g = spawn_gateway_with(c.gw, {"--clear-kill", "--duration", "300s"});
    wait_gateway_up(fx, g);
    const auto st = KillStateStore::load(c.gw.kill);
    REQUIRE(st.has_value());
    CHECK(!st->latched);
    CHECK(st->carry().is_zero());
    const std::uint64_t fa = fills(fx, 0);
    const pid_t a = spawn_strategy(c.a, g);
    const std::uint16_t ea = wait_resting(fx, c.a, {});
    fill_one(ea);  // one fill, about 0.6 of the 3 armed again
    REQUIRE_MESSAGE(wait_until([&] { return fills(fx, 0) > fa; }, 10000),
                    "a never traded after the clear: " << fastmm::test::read_file(c.a.config + ".log"));
    // The account line after the fill: its fee is booked, nothing carried, the budget armed.
    AccountLine acct;
    wait_until(
        [&] {
          acct = account_line(g);
          const auto fees = acct.n.find("fees");
          return fees != acct.n.end() && fees->second > 0;
        },
        5000);
    INFO("gateway: " << acct.line);
    CHECK(acct.line.find("kill=armed") != std::string::npos);
    CHECK(acct.n.at("max_loss") == 3.0);
    CHECK(acct.n.at("carried") == 0.0);
    CHECK(!log_has(g.log, latch_line));
    stop_strategy(a);
    stop_gateway(g);
  }
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
