#pragma once
// fastmm-gateway as a child process for the integration tests, and what a strategy's journal says
// about the order events it was given.
#include "process_util.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/session.hpp"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace fastmm::integration {

struct GatewayProcess {
  std::string socket;
  std::string log;
  pid_t pid = -1;
};

inline GatewayProcess spawn_gateway(const SessionFiles& f) {
  GatewayProcess g;
  g.socket = f.config + ".gw";
  g.log = f.config + ".gw.log";
  remove_all_of({g.socket, g.log});
  g.pid = spawn_process(FASTMM_GATEWAY_EXE,
                        {"--config", f.config, "--socket", g.socket, "--log", g.log});
  return g;
}

// The gateway listens and its venue is connected: market data, the order channel and the user
// stream are up at the simulator (two WebSocket API sessions: orders, and the user stream's).
inline void wait_gateway_up(const ServerFixture& fx, const GatewayProcess& g) {
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const sim::server::SimServerStats s = fx.server.stats();
                        return std::filesystem::exists(g.socket) && s.md_sessions >= 1 &&
                               s.api_sessions >= 2 && s.user_subscriptions >= 1;
                      },
                      20000),
                  "the gateway did not come up: " << fastmm::test::read_file(g.log));
}

inline pid_t spawn_strategy(const SessionFiles& f, const GatewayProcess& g, int duration_s = 120) {
  return spawn_live(f, duration_s, {"--gateway", g.socket, "--no-control"});
}

inline void stop_gateway(const GatewayProcess& g) {
  REQUIRE(::kill(g.pid, SIGTERM) == 0);
  CHECK(reap(g.pid) == live::kExitOk);
}

struct Sessions {
  std::uint64_t md = 0;
  std::uint64_t api = 0;
};
inline Sessions sessions_opened(const ServerFixture& fx) {
  const sim::server::SimServerStats s = fx.server.stats();
  return {s.md_sessions_opened, s.api_sessions_opened};
}

// The order events one session's engine consumed, read back from its journal.
struct JournalEpochs {
  std::string path;
  std::uint16_t epoch = 0;  // the session's own
  std::size_t own = 0;      // events about an order of its own epoch
  // Acks, rejects, cancel acks and rejects, expiries and reconciliation rows about an order of
  // another epoch: none may reach a strategy behind a gateway.
  std::vector<std::string> crossed;
  std::set<std::uint16_t> fill_epochs;  // of every fill it booked (0: an execution naming no order)
  std::size_t reconcile_rows = 0;
  // Cancel acks for an order it never asked to cancel or replace: someone else cancelled it.
  std::size_t unsolicited_cancel_acks = 0;
  std::size_t gateway_rejects = 0;  // RejectReason::Gateway*
};

inline JournalEpochs read_journal_epochs(const std::string& path) {
  JournalEpochs j;
  j.path = path;
  JournalReader r;
  REQUIRE_MESSAGE(r.open(path).has_value(), "cannot open " << path);
  REQUIRE(r.has_session());
  j.epoch = r.header().session_epoch;
  std::set<std::uint64_t> asked;
  const auto about = [&](ClientOrderId id, const char* what) {
    if (cl_ord_id_epoch(id) == j.epoch) {
      ++j.own;
    } else {
      j.crossed.push_back(std::string(what) + " " + std::string(encode_cl_ord_id(id).view()));
    }
  };
  r.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0) {
      if (h->type == EventType::OutCancel) asked.insert(msg_cast<OutCancelMsg>(h).cl_ord_id.value);
      if (h->type == EventType::OutReplace)
        asked.insert(msg_cast<OutReplaceMsg>(h).orig_cl_ord_id.value);
      return;
    }
    switch (h->type) {
      case EventType::OrderAck:
        about(msg_cast<OrderAckMsg>(h).cl_ord_id, "ack");
        break;
      case EventType::OrderReject: {
        const auto& m = msg_cast<OrderRejectMsg>(h);
        about(m.cl_ord_id, "reject");
        if (m.reason == RejectReason::GatewayRateLimit ||
            m.reason == RejectReason::GatewayOpenNotional ||
            m.reason == RejectReason::GatewayNotOwner)
          ++j.gateway_rejects;
        break;
      }
      case EventType::OrderCancelAck: {
        const ClientOrderId id = msg_cast<OrderCancelAckMsg>(h).cl_ord_id;
        about(id, "cancel ack");
        if (!asked.contains(id.value)) ++j.unsolicited_cancel_acks;
        break;
      }
      case EventType::OrderCancelReject:
        about(msg_cast<OrderCancelRejectMsg>(h).cl_ord_id, "cancel reject");
        break;
      case EventType::OrderExpired:
        about(msg_cast<OrderExpiredMsg>(h).cl_ord_id, "expiry");
        break;
      case EventType::OrderFill:
        j.fill_epochs.insert(cl_ord_id_epoch(msg_cast<OrderFillMsg>(h).cl_ord_id));
        break;
      case EventType::Reconcile: {
        const auto& m = msg_cast<ReconcileMsg>(h);
        if (m.kind == ReconcileMsg::Kind::OpenOrder) {
          ++j.reconcile_rows;
          about(m.cl_ord_id, "reconcile row");
        }
        break;
      }
      default:
        break;
    }
  });
  return j;
}

// Every journal a session of this configuration wrote.
inline std::vector<JournalEpochs> read_journals(const SessionFiles& f) {
  std::vector<JournalEpochs> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir, ec)) {
    if (e.path().extension() == ".fmj") out.push_back(read_journal_epochs(e.path().string()));
  }
  return out;
}

// ---- two strategies on one simulator: BTCUSDT and BTCUSDC, same filters ----------------------

// The simulator's second market, which strategy "b" trades: BTCUSDC by default; ETHUSDT (same
// filters and prices) where the account's limits need one settlement currency.
struct Market {
  std::string_view symbol;
  std::string_view base;
  std::string_view quote;
};
inline constexpr Market kBtcUsdc{"BTCUSDC", "BTC", "USDC"};
inline constexpr Market kEthUsdt{"ETHUSDT", "ETH", "USDT"};

inline sim::server::SimServerConfig two_markets(const Market& second = kBtcUsdc) {
  sim::server::SimServerConfig c = test_server_config();
  sim::server::SimSymbolConfig s = c.symbols.front();
  s.symbol = second.symbol;
  s.base_asset = second.base;
  s.quote_asset = second.quote;
  c.symbols.push_back(s);
  return c;
}

inline std::string second_instrument(const Market& m) {
  return "\n[[instruments]]\nvenue = \"sim\"\nsymbol = \"" + std::string(m.symbol) +
         "\"\nbase = \"" + std::string(m.base) + "\"\nquote = \"" + std::string(m.quote) +
         "\"\nasset_class = \"spot\"\ntick = \"0.01\"\nlot = \"0.00001\"\nmin_qty = "
         "\"0.00001\"\nmax_qty = \"100\"\nmin_notional = \"5\"\nenabled = true\n";
}

template <class Edit>
inline void rewrite(const std::string& path, Edit&& edit) {
  std::string text = fastmm::test::read_file(path);
  edit(text);
  std::ofstream out(path, std::ios::trunc);
  REQUIRE(out.good());
  out << text;
}

inline void replace_first(std::string& text, std::string_view from, std::string_view to) {
  const std::size_t p = text.find(from);
  REQUIRE(p != std::string::npos);
  text.replace(p, from.size(), to);
}

// The gateway's configuration lists both markets (and `gateway_extra`); a's lists BTCUSDT, b's
// the second market.
struct Configs {
  SessionFiles gw;
  SessionFiles a;
  SessionFiles b;
  std::string a_name;
  std::string b_name;
};

inline Configs write_configs(const ServerFixture& fx,
                             const std::string& stem,
                             const std::string& gateway_extra = {},
                             const Market& second = kBtcUsdc) {
  Configs c;
  c.gw = write_config(fx, stem + "-gw", "exit", "1000");
  rewrite(c.gw.config, [&](std::string& t) { t += second_instrument(second) + gateway_extra; });
  c.a_name = stem + "-a";
  c.b_name = stem + "-b";
  c.a = write_config(fx, c.a_name, "exit", "1000");
  c.b = write_config(fx, c.b_name, "exit", "1000");
  rewrite(c.b.config, [&](std::string& t) {
    replace_first(t, R"(symbol = "BTCUSDT")", "symbol = \"" + std::string(second.symbol) + "\"");
    replace_first(t, R"(base = "BTC")", "base = \"" + std::string(second.base) + "\"");
    replace_first(t, R"(quote = "USDT")", "quote = \"" + std::string(second.quote) + "\"");
  });
  for (const SessionFiles* f : {&c.gw, &c.a, &c.b})
    remove_all_of({f->epoch, f->kill, f->journal_dir, f->config + ".log", f->status});
  return c;
}

inline std::uint64_t fills(const ServerFixture& fx, std::size_t symbol) {
  const sim::server::SimServerStats s = fx.server.stats();
  return symbol < s.symbol_fills.size() ? s.symbol_fills[symbol] : 0;
}
inline Qty position(const sim::server::SimServerStats& s, std::size_t symbol) {
  return symbol < s.symbol_positions.size() ? s.symbol_positions[symbol] : Qty{};
}

// The epochs of the orders resting at the simulator, one entry per order.
inline std::vector<std::uint16_t> open_epochs(const ServerFixture& fx) {
  std::vector<std::uint16_t> out;
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    out.push_back(cl ? cl_ord_id_epoch(*cl) : std::uint16_t{0});
  }
  return out;
}
inline std::size_t open_of(const ServerFixture& fx, std::uint16_t epoch) {
  const std::vector<std::uint16_t> e = open_epochs(fx);
  return static_cast<std::size_t>(std::count(e.begin(), e.end(), epoch));
}

// Waits until `f`'s strategy has orders resting and returns their epoch (none of `others`).
inline std::uint16_t wait_resting(const ServerFixture& fx,
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

// Fills one of `epoch`'s resting orders at the simulator; the quantity filled (zero when none
// rests). A requote cancels before it places, so it tries the next one until one fills.
inline Qty fill_one_of(ServerFixture& fx, std::uint16_t epoch, std::string* wire = nullptr) {
  Qty filled{};
  static_cast<void>(wait_until(
      [&] {
        for (const std::string& id : fx.server.open_client_order_ids()) {
          const auto cl = decode_cl_ord_id(id);
          if (!cl || cl_ord_id_epoch(*cl) != epoch) continue;
          filled = fx.server.fill_open_order(id);
          if (filled.is_positive()) {
            if (wire != nullptr) *wire = id;
            return true;
          }
        }
        return false;
      },
      10000));
  return filled;
}

inline void stop_strategy(pid_t pid) {
  REQUIRE(::kill(pid, SIGTERM) == 0);
  CHECK(reap(pid) == live::kExitOk);
}

// ---- what the gateway and the strategies' stores say -----------------------------------------

inline std::optional<StatusSnapshot> gateway_status(const std::string& gw_name) {
  StatusReader r;
  std::string err;
  if (!r.open(default_gateway_status_path(gw_name), &err)) return std::nullopt;
  StatusSnapshot s;
  if (!r.read(s)) return std::nullopt;
  return s;
}

// The gateway's position of `symbol`, or nullopt before its status names it.
inline std::optional<StatusPosition> gateway_position(const std::string& gw_name,
                                                      std::string_view symbol = "BTCUSDT") {
  const auto s = gateway_status(gw_name);
  if (!s) return std::nullopt;
  for (std::uint32_t i = 0; i < s->gateway.position_count; ++i) {
    if (std::string_view(s->gateway.positions[i].symbol) == symbol) return s->gateway.positions[i];
  }
  return std::nullopt;
}

// Every fill the store of `engine` holds: their signed sum, and the client order ids they name.
struct StoredFills {
  Qty sum{};
  Qty no_order{};  // of the executions naming no order of FastMM's
  std::vector<std::string> cl_ord_ids;
  std::vector<std::string> exec_ids;
};
// Without assertions, so that a wait_until predicate can call it: nullopt when unreadable.
inline std::optional<StoredFills> try_stored(const SessionFiles& f, const std::string& engine) {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  if (reader == nullptr) return std::nullopt;
  store::BackendOptions opts;
  opts.engine_name = engine;
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  if (!reader->open(opts).has_value()) return std::nullopt;
  store::QueryFilter qf;
  qf.engine = engine;
  auto rows = reader->fills(qf);
  if (!rows.has_value()) return std::nullopt;
  std::size_t side = 0;
  std::size_t qty = 0;
  std::size_t cl = 0;
  std::size_t exec = 0;
  for (std::size_t i = 0; i < rows->columns.size(); ++i) {
    const std::string& n = rows->columns[i];
    if (n == "side") side = i;
    if (n == "qty") qty = i;
    if (n == "cl_ord_id") cl = i;
    if (n == "exec_id") exec = i;
  }
  StoredFills out;
  for (const std::vector<std::string>& row : rows->rows) {
    const auto q = Qty::from_decimal(row[qty]);
    if (!q.has_value()) return std::nullopt;
    const bool buy = !row[side].empty() && (row[side][0] == 'B' || row[side][0] == 'b');
    out.sum = buy ? out.sum + *q : out.sum - *q;
    const auto id = decode_cl_ord_id(row[cl]);
    if (!id || cl_ord_id_epoch(*id) == 0)
      out.no_order = buy ? out.no_order + *q : out.no_order - *q;
    out.cl_ord_ids.push_back(row[cl]);
    out.exec_ids.push_back(row[exec]);
  }
  return out;
}
inline StoredFills stored(const SessionFiles& f, const std::string& engine) {
  std::optional<StoredFills> s = try_stored(f, engine);
  REQUIRE_MESSAGE(s.has_value(), "cannot read the fills of " << engine);
  return *s;
}
inline bool stored_holds(const SessionFiles& f,
                         const std::string& engine,
                         const std::string& cl_ord_id) {
  const std::optional<StoredFills> s = try_stored(f, engine);
  return s &&
         std::find(s->cl_ord_ids.begin(), s->cl_ord_ids.end(), cl_ord_id) != s->cl_ord_ids.end();
}

inline bool holds(const std::vector<std::string>& ids, const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// The numbers the gateway's last account line gives BTCUSDT: `<name>=<qty>` of each strategy
// and `unattributed=<qty>` (logged once a second when they change, and at the end).
inline std::optional<Qty> logged_share(const GatewayProcess& g, const std::string& name) {
  const std::string text = fastmm::test::read_file(g.log);
  const std::size_t line = text.rfind("gateway: account position sim:BTCUSDT");
  if (line == std::string::npos) return std::nullopt;
  const std::size_t end = text.find('\n', line);
  const std::string l = text.substr(line, end - line);
  const std::string key = " " + name + "=";
  const std::size_t at = l.find(key);
  if (at == std::string::npos) return std::nullopt;
  const std::size_t from = at + key.size();
  const std::size_t to = l.find_first_of(" )", from);
  return Qty::from_decimal(l.substr(from, to - from));
}

}  // namespace fastmm::integration

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
