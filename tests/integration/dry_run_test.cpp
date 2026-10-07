// fastmm-live --dry-run: the engine quotes on paper (live/paper_venue.hpp).
//   * PaperVenue acknowledges new orders, cancels and replaces at once, expires IOC orders, answers
//     a reconciliation with its resting orders and reports the quotes and the would-be counts;
//   * a whole dry-run session against the in-process simulator sends the exchange nothing, its
//     engine counts the orders it would have sent, and its journal replays exactly;
//   * a dry run between two keyed sessions leaves their store, kill ledger and the position and
//     execution-replay start the next one restores exactly as they were.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/paper_venue.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/store/reader.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

// A connector that does nothing: what PaperVenue wraps in these tests.
class NullVenue final : public venues::Venue {
 public:
  [[nodiscard]] VenueId id() const noexcept override { return VenueId{0}; }
  [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
  [[nodiscard]] venues::VenueCaps caps() const noexcept override { return {}; }
  Result<void, std::string> load_reference_data(InstrumentTable&) override { return {}; }
  void attach(const venues::SymbolTable&,
              const InstrumentTable&,
              venues::EventSink&,
              venues::EventSink&,
              MsgRing* outbound) override {
    outbound_ = outbound;
  }
  void connect(net::Reactor&) override {}
  void disconnect() override {}
  void subscribe(std::span<const InstrumentId>) override {}
  void on_timer(std::int64_t) override {}
  void on_wake() override { ++wakes; }
  void send_now(std::span<const EventHeader* const>) override { ++wakes; }
  void request_open_orders() override {}
  bool cancel_all() override { return false; }
  [[nodiscard]] venues::VenueStatus status() const noexcept override { return {}; }
  MsgRing* outbound_ = nullptr;
  int wakes = 0;
};

struct PaperRig {
  InstrumentTable instruments;
  venues::SymbolTable symbols;
  MsgRing md{1U << 16};
  MsgRing orders{1U << 16};
  MsgRing outbound{1U << 16};
  venues::EventSink md_sink{&md, venues::SinkPolicy::Drop};
  venues::EventSink order_sink{&orders, venues::SinkPolicy::Spin};
  NullVenue* inner = nullptr;
  std::unique_ptr<live::PaperVenue> paper;

  PaperRig() {
    Instrument inst{};
    inst.venue = VenueId{0};
    inst.symbol = Symbol("BTCUSDT");
    inst.tick = Price::from_decimal("0.01").value();
    inst.lot = Qty::from_decimal("0.00001").value();
    REQUIRE(instruments.add(inst));
    auto v = std::make_unique<NullVenue>();
    inner = v.get();
    paper = std::make_unique<live::PaperVenue>(std::move(v), true);
    paper->attach(symbols, instruments, md_sink, order_sink, &outbound);
  }

  void send_new(std::uint64_t id, Side side, const char* px, TimeInForce tif = TimeInForce::Gtc) {
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    m.side = side;
    m.type = OrderType::Limit;
    m.tif = tif;
    m.price = Price::from_decimal(px).value();
    m.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&m, m.hdr.len));
  }
  void send_cancel(std::uint64_t id) {
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    m.cl_ord_id = ClientOrderId{id};
    REQUIRE(outbound.try_push(&m, m.hdr.len));
  }
  void send_replace(std::uint64_t orig, std::uint64_t id, const char* px) {
    OutReplaceMsg m{};
    init_header(m, EventType::OutReplace, InstrumentId{0}, VenueId{0});
    m.orig_cl_ord_id = ClientOrderId{orig};
    m.cl_ord_id = ClientOrderId{id};
    m.price = Price::from_decimal(px).value();
    m.qty = Qty::from_decimal("0.002").value();
    REQUIRE(outbound.try_push(&m, m.hdr.len));
  }
  // The events the order sink holds, oldest first: (type, client order id).
  std::vector<std::pair<EventType, std::uint64_t>> drain() {
    std::vector<std::pair<EventType, std::uint64_t>> out;
    while (const std::byte* p = orders.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      std::uint64_t id = 0;
      switch (h->type) {
        case EventType::OrderAck:
          id = msg_cast<OrderAckMsg>(h).cl_ord_id.value;
          break;
        case EventType::OrderExpired:
          id = msg_cast<OrderExpiredMsg>(h).cl_ord_id.value;
          break;
        case EventType::OrderCancelAck:
          id = msg_cast<OrderCancelAckMsg>(h).cl_ord_id.value;
          break;
        case EventType::OrderCancelReject:
          id = msg_cast<OrderCancelRejectMsg>(h).cl_ord_id.value;
          break;
        case EventType::OrderReject:
          id = msg_cast<OrderRejectMsg>(h).cl_ord_id.value;
          break;
        case EventType::Reconcile:
          id = msg_cast<ReconcileMsg>(h).cl_ord_id.value;
          break;
        default:
          break;
      }
      out.emplace_back(h->type, id);
      orders.release();
    }
    return out;
  }
};

using Ev = std::pair<EventType, std::uint64_t>;

std::string fresh(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

}  // namespace

TEST_CASE("paper venue: acks, cancels, replaces and expiries never reach the connector") {
  PaperRig r;
  r.send_new(1, Side::Buy, "100.00");
  r.send_new(2, Side::Sell, "101.00");
  r.send_new(3, Side::Buy, "99.00", TimeInForce::Ioc);
  r.paper->on_wake();
  CHECK(r.inner->wakes == 0);
  CHECK(r.drain() == std::vector<Ev>{{EventType::OrderAck, 1},
                                     {EventType::OrderAck, 2},
                                     {EventType::OrderAck, 3},
                                     {EventType::OrderExpired, 3}});
  r.send_replace(1, 4, "100.50");
  r.send_cancel(2);
  r.send_cancel(9);  // unknown
  r.paper->on_wake();
  CHECK(r.drain() == std::vector<Ev>{{EventType::OrderAck, 4},
                                     {EventType::OrderCancelAck, 2},
                                     {EventType::OrderCancelReject, 9}});
  const std::vector<live::PaperQuotes> q = r.paper->quotes();
  REQUIRE(q.size() == 1);
  CHECK(q[0].bid_orders == 1);
  CHECK(q[0].ask_orders == 0);
  CHECK(q[0].bid.price == Price::from_decimal("100.50").value());
  CHECK(q[0].bid.qty == Qty::from_decimal("0.002").value());
  CHECK(q[0].news == 3);
  CHECK(q[0].cancels == 2);
  CHECK(q[0].replaces == 1);
  const live::PaperTotals t = r.paper->totals();
  CHECK(t.news == 3);
  CHECK(t.cancels == 2);
  CHECK(t.replaces == 1);
  CHECK(r.paper->caps().supports_replace);
  CHECK(r.paper->cancel_all());

  // A reconciliation sees what rests on paper.
  r.paper->request_open_orders();
  CHECK(r.drain() == std::vector<Ev>{{EventType::Reconcile, 0},
                                     {EventType::Reconcile, 4},
                                     {EventType::Reconcile, 0}});
}

TEST_CASE("dry run session: quotes on paper, sends the exchange nothing, replays exactly") {
  static const bool registered = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(registered);
  ServerFixture fx;
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = "dry-run-test";
  cfg.engine.epoch_file = fresh("dry-run-test.epoch");
  cfg.engine.kill_file = fresh("dry-run-test.kill");
  live::LiveOptions o;
  o.dry_run = true;
  o.duration_ns = seconds(4).ns;
  o.journal_path = fresh("dry-run-test.fmj");
  o.status_path = fresh("dry-run-test.status");
  o.program = "dry-run-test";
  REQUIRE(live::run_live(cfg, o) == live::kExitOk);

  const sim::server::SimServerStats s = fx.server.stats();
  CHECK(s.orders_accepted == 0);
  CHECK(s.orders_rejected == 0);
  CHECK(s.cancel_all_requests == 0);

  StatusReader reader;
  std::string err;
  REQUIRE(reader.open(o.status_path, &err));
  StatusSnapshot st;
  REQUIRE(reader.read(st));
  CHECK(st.dry_run == 1);
  CHECK(st.orders_sent > 0);  // on paper
  CHECK(st.fills == 0);
  CHECK(format_status(st, st.updated_ns, false).find("paper orders=") != std::string::npos);

  const bt::ReplayResult rr = bt::replay_journal(o.journal_path);
  INFO("expected: " << rr.expected_message);
  INFO("actual:   " << rr.actual_message);
  CHECK(rr.first_mismatch == -1);
  CHECK(rr.outbound_messages == rr.recorded_messages);
  CHECK(rr.recorded_messages > 0);
  CHECK(rr.ok());
}

namespace {

// Every file under `dir` with its bytes.
std::map<std::string, std::string> files_under(const std::filesystem::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    out[std::filesystem::relative(e.path(), dir).string()] = fastmm::test::read_file(e.path());
  }
  return out;
}

store::Recovery recovery_of(const Config& cfg) {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions bo;
  bo.config = &cfg.storage;
  bo.engine_name = cfg.engine.name;
  bo.default_dir = cfg.engine.journal_dir;
  bo.read_only = true;
  REQUIRE(reader->open(bo).has_value());
  store::QueryFilter qf;
  qf.engine = cfg.engine.name;
  auto rec = reader->recovery(qf);
  REQUIRE(rec.has_value());
  return *rec;
}

}  // namespace

TEST_CASE("dry run session: a keyed session's store, kill ledger and replay start are untouched") {
  static const bool registered = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(registered);
  ServerFixture fx;
  const std::filesystem::path dir = fastmm::test::tmp_dir() / "dry-run-state";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = "dry-run-state";
  cfg.engine.journal_dir = dir.string();
  cfg.engine.epoch_file = (dir / "epoch").string();
  const auto options = [&](const std::string& tag, bool dry) {
    live::LiveOptions o;
    o.dry_run = dry;
    o.duration_ns = seconds(5).ns;
    o.journal_path = fresh("dry-run-state-" + tag + ".fmj");  // outside `dir`
    o.status_path = fresh("dry-run-state-" + tag + ".status");
    o.program = "dry-run-test";
    return o;
  };

  REQUIRE(live::run_live(cfg, options("keyed1", false)) == live::kExitOk);
  const store::Recovery before = recovery_of(cfg);
  REQUIRE(before.found);
  for (const auto& p : before.position_state) MESSAGE(p.symbol << " qty_raw=" << p.qty_raw);
  auto files = files_under(dir);
  files.erase("epoch");  // the next session epoch; any value is unique
  REQUIRE(files.contains("dry-run-state.kill"));

  REQUIRE(live::run_live(cfg, options("dry", true)) == live::kExitOk);
  auto after_files = files_under(dir);
  after_files.erase("epoch");
  CHECK(after_files == files);  // no store row, no kill ledger write, no new file

  const store::Recovery after = recovery_of(cfg);
  CHECK(after.session_id == before.session_id);
  REQUIRE(after.position_state.size() == before.position_state.size());
  for (std::size_t i = 0; i < before.position_state.size(); ++i) {
    CHECK(after.position_state[i].symbol == before.position_state[i].symbol);
    CHECK(after.position_state[i].qty_raw == before.position_state[i].qty_raw);
    CHECK(after.position_state[i].avg_px_raw == before.position_state[i].avg_px_raw);
  }
  REQUIRE(after.venue_resume.size() == before.venue_resume.size());
  for (std::size_t i = 0; i < before.venue_resume.size(); ++i)
    CHECK(after.venue_resume[i].since_ms == before.venue_resume[i].since_ms);
  CHECK(after.fallback_since_ms == before.fallback_since_ms);

  // The next keyed session carries the first one's position over and records itself after it.
  REQUIRE(live::run_live(cfg, options("keyed2", false)) == live::kExitOk);
  const store::Recovery last = recovery_of(cfg);
  CHECK(last.session_id != before.session_id);
}
