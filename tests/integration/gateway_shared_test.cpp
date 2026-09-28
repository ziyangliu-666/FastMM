// Several strategies on one instrument behind fastmm-gateway ([gateway.shared]): strategies "a"
// and "b" both quote BTCUSDT on the simulator. The gateway and the strategies are real children;
// a raw GatewayClient in this process plays a third strategy where a test needs exact orders.
#include "gateway_util.hpp"

#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/gateway.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

// No market trades: every fill is one a test makes, so positions can be compared exactly.
sim::server::SimServerConfig quiet() {
  sim::server::SimServerConfig c = test_server_config();
  c.generator.market_rate_per_s = 0.0;
  return c;
}

struct Shared {
  SessionFiles gw;
  SessionFiles a;
  SessionFiles b;
  std::string gw_name;
  std::string a_name;
  std::string b_name;
};

// The gateway lists BTCUSDT under [gateway.shared] (with `primary` when given); a and b trade it.
Shared write_shared(const ServerFixture& fx,
                    const std::string& stem,
                    const std::string& primary = {}) {
  Shared c;
  c.gw_name = stem + "-gw";
  c.a_name = stem + "-a";
  c.b_name = stem + "-b";
  c.gw = write_config(fx, c.gw_name, "exit", "1000");
  rewrite(c.gw.config, [&](std::string& t) {
    t += "\n[gateway.shared.\"sim:BTCUSDT\"]\n";
    if (!primary.empty()) t += "primary = \"" + primary + "\"\n";
  });
  c.a = write_config(fx, c.a_name, "exit", "1000");
  c.b = write_config(fx, c.b_name, "exit", "1000");
  for (const SessionFiles* f : {&c.gw, &c.a, &c.b})
    remove_all_of({f->epoch, f->kill, f->journal_dir, f->config + ".log", f->status});
  remove_all_of({default_gateway_status_path(c.gw_name)});
  return c;
}

std::string log_of(const SessionFiles& f) {
  return fastmm::test::read_file(f.config + ".log");
}

std::optional<StatusSnapshot> gateway_status(const Shared& c) {
  StatusReader r;
  std::string err;
  if (!r.open(default_gateway_status_path(c.gw_name), &err)) return std::nullopt;
  StatusSnapshot s;
  if (!r.read(s)) return std::nullopt;
  return s;
}

// The gateway's BTCUSDT position, or nullopt before its status names it.
std::optional<StatusPosition> btc_position(const Shared& c) {
  const auto s = gateway_status(c);
  if (!s) return std::nullopt;
  for (std::uint32_t i = 0; i < s->gateway.position_count; ++i) {
    if (std::string_view(s->gateway.positions[i].symbol) == "BTCUSDT")
      return s->gateway.positions[i];
  }
  return std::nullopt;
}

// Fills one of `epoch`'s resting orders at the simulator; the quantity filled (zero when none
// rests). A requote cancels before it places, so it tries the next one until one fills.
Qty fill_one_of(ServerFixture& fx, std::uint16_t epoch, std::string* wire = nullptr) {
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

// Every fill the store of `engine` holds: their signed sum, and the client order ids they name.
struct StoredFills {
  Qty sum{};
  Qty no_order{};  // of the executions naming no order of FastMM's
  std::vector<std::string> cl_ord_ids;
  std::vector<std::string> exec_ids;
};
// Without assertions, so that a wait_until predicate can call it: nullopt when unreadable.
std::optional<StoredFills> try_stored(const SessionFiles& f, const std::string& engine) {
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
StoredFills stored(const SessionFiles& f, const std::string& engine) {
  std::optional<StoredFills> s = try_stored(f, engine);
  REQUIRE_MESSAGE(s.has_value(), "cannot read the fills of " << engine);
  return *s;
}
bool stored_holds(const SessionFiles& f, const std::string& engine, const std::string& cl_ord_id) {
  const std::optional<StoredFills> s = try_stored(f, engine);
  return s &&
         std::find(s->cl_ord_ids.begin(), s->cl_ord_ids.end(), cl_ord_id) != s->cl_ord_ids.end();
}

bool holds(const std::vector<std::string>& ids, const std::string& id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// The numbers the gateway's last account line gives BTCUSDT: `<name>=<qty>` of each strategy
// and `unattributed=<qty>` (logged once a second when they change, and at the end).
std::optional<Qty> logged_share(const GatewayProcess& g, const std::string& name) {
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

// Each strategy's store equals its own fills, the gateway counted each fill towards the strategy
// that booked it, and the venue's position is the stores' plus what nobody holds.
void check_books(ServerFixture& fx, const Shared& c, const GatewayProcess& g, Qty unattributed) {
  const Qty venue = fx.server.stats().position;
  const Qty pa = store_position(c.a, c.a_name);
  const Qty pb = store_position(c.b, c.b_name);
  const StoredFills fa = stored(c.a, c.a_name);
  const StoredFills fb = stored(c.b, c.b_name);
  INFO("venue " << venue.raw << " a " << pa.raw << " (fills " << fa.sum.raw << ") b " << pb.raw
                << " (fills " << fb.sum.raw << ") unattributed " << unattributed.raw);
  CHECK(pa == fa.sum);
  CHECK(pb == fb.sum);
  CHECK(pa + pb + unattributed == venue);
  CHECK(booked_twice(c.a, c.a_name).empty());
  CHECK(booked_twice(c.b, c.b_name).empty());
  // No execution in both stores.
  for (const std::string& id : fa.exec_ids) CHECK_MESSAGE(!holds(fb.exec_ids, id), id);
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
  // The gateway's attribution is the stores'.
  CHECK(logged_share(g, c.a_name) == pa);
  CHECK(logged_share(g, c.b_name) == pb);
  CHECK(logged_share(g, "unattributed") == unattributed);
  CHECK(fastmm::test::read_file(g.log).find("does not match its strategies'") == std::string::npos);
}

// The status says what nobody holds and that the books agree.
void check_status(const Shared& c, const ServerFixture& fx, Qty unattributed) {
  std::optional<StatusPosition> p;
  const bool agreed = wait_until(
      [&] {
        p = btc_position(c);
        return p && p->qty_raw == fx.server.stats().position.raw &&
               p->unattributed_raw == unattributed.raw;
      },
      5000);
  REQUIRE(p.has_value());
  INFO("status qty " << p->qty_raw << " unattributed " << p->unattributed_raw << " unexplained "
                     << p->unexplained_raw << "; venue " << fx.server.stats().position.raw);
  CHECK(agreed);
  CHECK(p->shared == 1);
  CHECK(p->unexplained_raw == 0);
  CHECK(p->owner_epoch == 0);
}

}  // namespace

TEST_CASE(
    "gateway shared: two strategies trade one instrument, each books its own fills and the venue "
    "holds their sum") {
  ServerFixture fx(quiet());
  const Shared c = write_shared(fx, "gw-sh-two");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  CHECK(ea != eb);
  REQUIRE(wait_until([&] { return btc_position(c) && btc_position(c)->traders == 2; }, 5000));
  // Both trade: one of each one's orders is filled, and the market fills more.
  for (int round = 0; round < 2; ++round) {
    REQUIRE_MESSAGE(fill_one_of(fx, ea).is_positive(), log_of(c.a));
    REQUIRE_MESSAGE(fill_one_of(fx, eb).is_positive(), log_of(c.b));
  }
  // A trade FastMM did not make, with no primary: the account's alone.
  outside_trade(fx, "BUY", "0.002");
  const Qty outside = Qty::from_decimal("0.002").value();
  check_status(c, fx, outside);

  stop_strategy(a);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  check_status(c, fx, outside);
  stop_gateway(g);
  check_books(fx, c, g, outside);
  // Neither engine saw the other's order events or fills; neither booked the outside trade.
  for (const auto& [f, other] : {std::pair{&c.a, eb}, std::pair{&c.b, ea}}) {
    for (const JournalEpochs& j : read_journals(*f)) {
      INFO(j.path);
      CHECK(j.crossed.empty());
      CHECK(!j.fill_epochs.contains(other));
      CHECK(!j.fill_epochs.contains(0));
      CHECK(j.unsolicited_cancel_acks == 0);
    }
  }
}

namespace {

// a and b trade; a dies by kill -9 while b trades on, and comes back. With `fill_while_dead` one
// of a's orders is filled at the venue after a died and before the gateway has cancelled it (the
// gateway is stopped meanwhile); a's restart books it from its replay, and b never does.
void kill_and_restart(const std::string& stem, bool fill_while_dead) {
  ServerFixture fx(quiet());
  const Shared c = write_shared(fx, stem);
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);

  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  REQUIRE_MESSAGE(fill_one_of(fx, ea).is_positive(), log_of(c.a));
  REQUIRE_MESSAGE(fill_one_of(fx, eb).is_positive(), log_of(c.b));
  // a's first fill is in its store before it dies: its restart resumes after it.
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const auto s = try_stored(c.a, c.a_name);
                        return s && !s->exec_ids.empty();
                      },
                      10000),
                  log_of(c.a));
  REQUIRE(wait_until([&] { return open_of(fx, ea) > 0 && open_of(fx, eb) > 0; }, 20000));

  std::string dead_order;
  Qty dead_fill{};
  if (fill_while_dead) {
    REQUIRE(::kill(g.pid, SIGSTOP) == 0);
    REQUIRE(::kill(a, SIGKILL) == 0);
    CHECK(reap(a) == -1);
    dead_fill = fill_one_of(fx, ea, &dead_order);
    REQUIRE(::kill(g.pid, SIGCONT) == 0);
    REQUIRE(dead_fill.is_positive());
  } else {
    REQUIRE(::kill(a, SIGKILL) == 0);
    CHECK(reap(a) == -1);
  }
  CHECK_MESSAGE(wait_until([&] { return open_of(fx, ea) == 0; }, 2000),
                "a's orders still resting 2 s after it died");
  // b's quotes stay, and b trades on.
  CHECK(open_of(fx, eb) > 0);
  CHECK_MESSAGE(fill_one_of(fx, eb).is_positive(), log_of(c.b));
  CHECK(wait_until([&] { return open_of(fx, eb) > 0; }, 10000));

  // a's restart: its position from its store, its replay books what happened while it was dead.
  const pid_t a2 = spawn_strategy(c.a, g);
  const std::uint16_t ea2 = wait_resting(fx, c.a, {ea, eb});
  CHECK(ea2 != ea);
  if (fill_while_dead) {
    CHECK_MESSAGE(
        wait_until([&] { return stored_holds(c.a, c.a_name, dead_order); }, 10000),
        "a never booked the fill of " << dead_order << " made while it was dead: " << log_of(c.a));
  }
  CHECK_MESSAGE(fill_one_of(fx, ea2).is_positive(), log_of(c.a));
  CHECK_MESSAGE(fill_one_of(fx, eb).is_positive(), log_of(c.b));
  check_status(c, fx, Qty{});

  stop_strategy(a2);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  stop_gateway(g);
  check_books(fx, c, g, Qty{});
  if (fill_while_dead) {
    CHECK(holds(stored(c.a, c.a_name).cl_ord_ids, dead_order));
    CHECK(!holds(stored(c.b, c.b_name).cl_ord_ids, dead_order));
  }
  for (const JournalEpochs& j : read_journals(c.b)) {
    INFO(j.path);
    CHECK(j.crossed.empty());
    CHECK(!j.fill_epochs.contains(ea));
    CHECK(!j.fill_epochs.contains(ea2));
    CHECK(j.unsolicited_cancel_acks == 0);
  }
  for (const JournalEpochs& j : read_journals(c.a)) {
    INFO(j.path);
    CHECK(!j.fill_epochs.contains(eb));
  }
}

}  // namespace

TEST_CASE(
    "gateway shared: kill -9 of one strategy leaves the other trading, and its restart books "
    "nothing twice") {
  kill_and_restart("gw-sh-kill", false);
}

TEST_CASE(
    "gateway shared: a fill of a dead strategy's order is booked by its restart, not by the "
    "strategy still attached") {
  kill_and_restart("gw-sh-dead", true);
}

namespace {

// A third strategy in this process: exact orders into the gateway, its answers read back.
struct RawStrategy {
  std::unique_ptr<live::GatewayClient> client;
  InstrumentId inst{};
  std::uint32_t seq = 0;

  ClientOrderId send(Side side, Price px, Qty qty, OrderType type) {
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, inst, VenueId{0});
    m.cl_ord_id = make_cl_ord_id(client->session_epoch(), ++seq);
    m.side = side;
    m.price = px;
    m.qty = qty;
    m.type = type;
    m.tif = type == OrderType::Market ? TimeInForce::Ioc : TimeInForce::Gtc;
    REQUIRE(client->venues()[0].outbound->try_push(&m, m.hdr.len));
    live::GatewayClient::wake_venue(client.get(), VenueId{0});
    return m.cl_ord_id;
  }
  // The first answer about `id` on the order ring: an ack or a reject.
  std::optional<EventType> answer(ClientOrderId id, RejectReason* why) {
    std::optional<EventType> got;
    static_cast<void>(wait_until(
        [&] {
          ShmRing& ring = *client->venues()[0].order;
          while (const std::byte* p = ring.try_peek()) {
            const auto& h = *reinterpret_cast<const EventHeader*>(p);
            if (h.type == EventType::OrderAck && msg_cast<OrderAckMsg>(&h).cl_ord_id == id) {
              got = h.type;
            } else if (h.type == EventType::OrderReject &&
                       msg_cast<OrderRejectMsg>(&h).cl_ord_id == id) {
              got = h.type;
              *why = msg_cast<OrderRejectMsg>(&h).reason;
            }
            ring.release();
            if (got) return true;
          }
          // Market data is not read: the gateway drops it for this attachment alone.
          return false;
        },
        10000));
    return got;
  }
};

}  // namespace

TEST_CASE(
    "gateway shared: an order that would trade with another strategy's resting order is refused "
    "at the gateway") {
  ServerFixture fx(quiet());
  const Shared c = write_shared(fx, "gw-sh-cross");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  // a quotes both sides.
  Price bid{};
  Price ask{};
  REQUIRE(wait_until(
      [&] {
        bid = ask = Price{};
        for (const auto& o : fx.server.open_orders()) {
          const auto cl = decode_cl_ord_id(o.client_order_id);
          if (!cl || cl_ord_id_epoch(*cl) != ea) continue;
          (o.side == Side::Buy ? bid : ask) = o.price;
        }
        return bid.is_positive() && ask.is_positive();
      },
      20000));

  // One [engine] name is one strategy.
  {
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = c.a_name;
    req.instruments = {{"sim", "BTCUSDT"}};
    CHECK(live::GatewayClient::attach(g.socket, req, &err) == nullptr);
    CHECK(err.find("'" + c.a_name + "' is attached already") != std::string::npos);
  }

  RawStrategy x;
  {
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = "gw-sh-cross-x";
    req.instruments = {{"sim", "BTCUSDT"}};
    x.client = live::GatewayClient::attach(g.socket, req, &err);
    REQUIRE_MESSAGE(x.client != nullptr, err);
  }
  const Instrument* btc = x.client->instruments().find(VenueId{0}, "BTCUSDT");
  REQUIRE(btc != nullptr);
  x.inst = btc->id;
  const Qty qty = Qty::from_decimal("0.001").value();
  const Price margin = Price::from_decimal("20").value();  // a requotes on every tick
  const auto refused = [&](ClientOrderId id) {
    RejectReason why = RejectReason::None;
    const std::optional<EventType> got = x.answer(id, &why);
    INFO("answer " << (got ? static_cast<int>(*got) : -1) << " reason " << to_string(why));
    return got == EventType::OrderReject && why == RejectReason::GatewaySelfTrade;
  };
  // A buy through a's ask, a sell through its bid, a market order: each would trade with a.
  CHECK(refused(x.send(Side::Buy, ask + margin, qty, OrderType::Limit)));
  CHECK(refused(x.send(Side::Sell, bid - margin, qty, OrderType::Limit)));
  CHECK(refused(x.send(Side::Buy, Price{}, qty, OrderType::Market)));
  // One that does not reach a's prices goes to the venue and rests there.
  const ClientOrderId away =
      x.send(Side::Buy, bid - Price::from_decimal("100").value(), qty, OrderType::Limit);
  RejectReason why = RejectReason::None;
  CHECK(x.answer(away, &why) == EventType::OrderAck);
  const std::string away_wire(encode_cl_ord_id(away).view());
  CHECK(wait_until([&] { return holds(fx.server.open_client_order_ids(), away_wire); }, 5000));
  // a's quotes that could have been hit are still there, and a never saw its orders go.
  CHECK(open_of(fx, ea) > 0);
  // The status counts them against the sender (published every 250 ms).
  std::uint64_t refusals = 0;
  CHECK(wait_until(
      [&] {
        const auto st = gateway_status(c);
        if (!st) return false;
        for (std::uint32_t k = 0; k < st->gateway.attachment_count; ++k) {
          const StatusAttachment& at = st->gateway.attachments[k];
          if (std::string_view(at.engine) == "gw-sh-cross-x") refusals = at.refused[9];
        }
        return refusals == 3;
      },
      5000));
  CHECK(refusals == 3);
  // Its detach cancels its order; a trades on.
  x.client.reset();
  CHECK(wait_until([&] { return !holds(fx.server.open_client_order_ids(), away_wire); }, 5000));
  CHECK(open_of(fx, ea) > 0);

  stop_strategy(a);
  stop_gateway(g);
  CHECK(fastmm::test::read_file(g.log).find("self_trade=3") != std::string::npos);
  for (const JournalEpochs& j : read_journals(c.a)) CHECK(j.unsolicited_cancel_acks == 0);
}

TEST_CASE(
    "gateway shared: an execution naming no order goes to the primary strategy, booked once") {
  ServerFixture fx(quiet());
  const Shared c = write_shared(fx, "gw-sh-pri", "gw-sh-pri-a");
  const GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  REQUIRE_MESSAGE(fill_one_of(fx, eb).is_positive(), log_of(c.b));
  outside_trade(fx, "SELL", "0.003");
  const Qty outside = Qty::from_decimal("0.003").value();
  CHECK_MESSAGE(wait_until(
                    [&] {
                      const auto s = try_stored(c.a, c.a_name);
                      return s && s->no_order == Qty{} - outside;
                    },
                    10000),
                "the primary never booked the outside trade: " << log_of(c.a));
  check_status(c, fx, Qty{});

  stop_strategy(a);
  stop_strategy(b);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  stop_gateway(g);
  check_books(fx, c, g, Qty{});
  bool a_booked = false;
  for (const JournalEpochs& j : read_journals(c.a))
    a_booked = a_booked || j.fill_epochs.contains(0);
  CHECK(a_booked);
  for (const JournalEpochs& j : read_journals(c.b)) CHECK(!j.fill_epochs.contains(0));
  CHECK(stored(c.a, c.a_name).no_order == Qty{} - outside);
  CHECK(stored(c.b, c.b_name).no_order.is_zero());
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
