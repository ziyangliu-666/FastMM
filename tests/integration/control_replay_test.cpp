// The control plane is inside the deterministic record, not beside it: a session that an operator
// touched while it ran -- a parameter change, a per-instrument pull, a flatten -- replays from its
// journal alone to the identical outbound stream. This is the reason the commands go through the
// control ring instead of reaching into the engine (docs/explanation/determinism.md).
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

constexpr std::uint16_t kEpoch = 11;
constexpr InstrumentId kFirst{0};

std::string fresh_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

bool fills_at_least(ServerFixture& fx, std::uint64_t fills, int timeout_ms) {
  return wait_until([&] { return fx.server.stats().fills >= fills; }, timeout_ms);
}

}  // namespace

TEST_CASE("sim_exchange replay: an operator sequence replays to the identical outbound stream") {
  const std::string path = fresh_journal("control_ops.fmj");
  ServerFixture fx;
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = kEpoch;
  Config cfg = sim_local_config(fx, false);
  LiveEngine live(cfg, opts);
  live.start();
  REQUIRE(fills_at_least(fx, 2, 30000));

  // 1. A parameter change, validated on the control thread and published onto the control ring,
  //    exactly as fastmm-live's `fastmm-ctl param half_spread_bps=...` does.
  BasicMM configured;
  REQUIRE_FALSE(configured.configure(cfg.strategy.params).has_value());
  ParamPublisher publisher(ParamSink::to_ring(live.control_ring()), configured.params());
  REQUIRE(publisher.publish({{"half_spread_bps", "2"}, {"quote_qty", "0.002"}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 2. A pull of the one instrument: its quotes go, the session keeps running.
  REQUIRE(live.control(ControlCommand::PullQuotes, kFirst));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 3. A position to work off. BasicMM's inventory cap keeps it near flat against the simulated
  //    market, so the test puts one on the way a venue does after a reconnect: a position update.
  {
    PositionUpdateMsg m{};
    init_header(m, EventType::PositionUpdate, kFirst, VenueId{0});
    m.qty = Qty::from_decimal("0.005").value();
    m.avg_px = Price::from_decimal("60000").value();  // [sim] start_mid
    m.hdr.recv_ts = wall_now();
    REQUIRE(live.control_ring().try_push(&m, m.hdr.len));
  }
  // 4. A flatten with a 50 bps allowance: reduce-only slices through the touch until it is flat.
  REQUIRE(live.control(ControlCommand::Flatten, kFirst, VenueId::invalid(), 50));
  // It publishes its state as soon as it ends; a slice the rate limiter refuses is retried at the
  // next sweep, so this waits for the end rather than for the first order.
  REQUIRE(wait_until([&] { return live.live_stats().flatten_state == FlattenState::Flat; }, 20000));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  live.stop();

  const EngineStats& es = live.engine().stats();
  CHECK(es.flattens == 1);
  CHECK(es.param_updates == 1);
  CHECK(es.flatten_orders >= 1);
  CHECK(live.live_stats().flatten_state == FlattenState::Flat);
  MESSAGE("flatten: orders=" << es.flatten_orders
                             << " state=" << to_string(live.live_stats().flatten_state)
                             << " fills=" << es.fills);

  // The journal holds the operator's commands, and the replay reproduces every order they caused.
  std::size_t control_records = 0;
  std::size_t param_records = 0;
  std::size_t flatten_timers = 0;
  {
    JournalReader reader;
    REQUIRE(reader.open(path));
    reader.for_each([&](const EventHeader* h) {
      if (h->type == EventType::Control) ++control_records;
      if (h->type == EventType::ParamUpdate) ++param_records;
      if (h->type == EventType::Timer &&
          msg_cast<TimerMsg>(h).engine == LiveEngine::EngineT::kFlattenTimer) {
        ++flatten_timers;
      }
    });
  }
  CHECK(control_records == 3);  // pull, flatten, and the kill switch of the shutdown
  CHECK(param_records == 1);
  MESSAGE("flatten sweeps on the engine timer: " << flatten_timers);

  const bt::ReplayResult r = bt::replay_journal(path);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}
