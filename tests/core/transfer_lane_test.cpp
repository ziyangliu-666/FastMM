// The transfer lane (core/transfer_lane.hpp) and its worker (core/transfer_worker.hpp) on their
// own: the windows the budget counts, the breaker, assets the venue refuses, the sweep's plan, the
// client ids, and the worker's answers, queries, ledger and restart.
#include "fastmm/core/transfer_lane.hpp"

#include "test_support.hpp"

#include "fastmm/core/transfer_worker.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

constexpr std::int64_t kS = 1'000'000'000;

Timestamp at(std::int64_t s) {
  return Timestamp{1'000'000 * kS + (s * kS)};
}

SweepCandidate cand(std::uint32_t inst, std::uint8_t from, const char* asset, double value) {
  SweepCandidate c;
  c.instrument = InstrumentId{inst};
  c.from = VenueId{from};
  c.to = VenueId{0};
  c.asset.assign(asset);
  c.amount = 100;
  c.value = value;
  c.dust = value < 5.0;
  return c;
}

}  // namespace

TEST_CASE("core.transfer_lane: the windows count the last minute, hour and day") {
  EventWindows w(10);
  for (int k = 0; k < 3; ++k) w.add(at(k).ns);
  CHECK(w.minute(at(3).ns) == 3);
  CHECK(w.minute(at(60).ns) == 2);  // the first is 60 s old
  CHECK(w.hour(at(60).ns) == 3);
  w.add(at(3600).ns);
  CHECK(w.hour(at(3600).ns) == 3);
  CHECK(w.hour(at(3602).ns) == 1);
  CHECK(w.day(at(3600).ns) == 4);
  CHECK(w.day(at(86400 + 2).ns) == 1);
  // Full: the oldest go first, and the counts stay consistent.
  EventWindows small(3);
  for (int k = 0; k < 6; ++k) small.add(at(k).ns);
  CHECK(small.day(at(6).ns) == 3);
  CHECK(small.minute(at(6).ns) == 3);
}

TEST_CASE(
    "core.transfer_lane: the budget leaves the minute's, hour's and day's rest and the slots") {
  LaneConfig c;
  c.per_minute = 3;
  c.per_hour = 5;
  c.per_day = 6;
  c.max_in_flight = 2;
  TransferLane lane(VenueId{0}, c);
  CHECK(lane.room(at(0)) == 2);  // the slots
  REQUIRE(lane.open(1) != nullptr);
  CHECK(lane.room(at(0)) == 1);
  lane.count(at(0));
  lane.count(at(0));
  lane.count(at(0));
  CHECK(lane.room(at(1)) == 0);  // the minute's 3
  CHECK(lane.room(at(61)) == 1);
  lane.count(at(61));
  lane.count(at(62));
  CHECK(lane.room(at(63)) == 0);  // the hour's 5
  CHECK_FALSE(lane.day_spent(at(63)));
  lane.count(at(3700));
  CHECK(lane.day_spent(at(3700)));
  CHECK_FALSE(lane.day_spent(at(86400 + 61)));
  // A slot freed is a slot again; open() takes none past kSlots.
  lane.close(*lane.find(1));
  CHECK(lane.in_flight() == 0);
  for (std::uint64_t k = 0; k < TransferLane::kSlots; ++k) REQUIRE(lane.open(10 + k) != nullptr);
  CHECK(lane.open(99) == nullptr);
}

TEST_CASE("core.transfer_lane: the breaker trips, resumes once by itself, then stays off") {
  LaneConfig c;
  c.breaker = seconds(900);
  TransferLane lane(VenueId{0}, c);
  for (int k = 0; k < 4; ++k) CHECK_FALSE(lane.note_failure(at(k), false));
  CHECK(lane.breaker(at(4)) == BreakerState::Closed);
  CHECK(lane.note_failure(at(5), false));
  CHECK(lane.breaker(at(6)) == BreakerState::Paused);
  CHECK(lane.breaker(at(5 + 900)) == BreakerState::Closed);
  // Failures spread over more than ten minutes do not trip it.
  for (int k = 0; k < 6; ++k) CHECK_FALSE(lane.note_failure(at(1000 + 200 * k), false));
  // Three unknown answers trip it again: off until the operator.
  CHECK_FALSE(lane.note_unknown(at(3000)));
  CHECK_FALSE(lane.note_unknown(at(3001)));
  CHECK(lane.note_unknown(at(3002)));
  CHECK(lane.breaker(at(9000)) == BreakerState::Off);
  CHECK(lane.stats().trips == 2);
  lane.set_on();
  CHECK(lane.breaker(at(9001)) == BreakerState::Closed);
  // A rate-limit answer trips it at once; the operator can pause it too.
  CHECK(lane.note_failure(at(9002), true));
  CHECK(lane.breaker(at(9003)) == BreakerState::Paused);
  lane.set_on();
  lane.pause(at(9004));
  CHECK(lane.breaker(at(9005)) == BreakerState::Paused);
  CHECK(lane.breaker(at(9004 + 900)) == BreakerState::Closed);
}

TEST_CASE("core.transfer_lane: refusals for want of balance count apart from failures") {
  LaneConfig c;
  c.breaker = seconds(900);
  TransferLane lane(VenueId{0}, c);
  // Nine within ten minutes, with four failures besides: no trip.
  for (int k = 0; k < 9; ++k) CHECK_FALSE(lane.note_short(at(k)));
  for (int k = 0; k < 4; ++k) CHECK_FALSE(lane.note_failure(at(10 + k), false));
  CHECK(lane.breaker(at(20)) == BreakerState::Closed);
  // The tenth trips it: the engine's estimates are off across assets.
  CHECK(lane.note_short(at(21)));
  CHECK(lane.breaker(at(22)) == BreakerState::Paused);
  CHECK(lane.breaker(at(21 + 900)) == BreakerState::Closed);
  // The trip cleared the count; ten spread over more than ten minutes do not trip it.
  for (int k = 0; k < 10; ++k) CHECK_FALSE(lane.note_short(at(1000 + 70 * k)));
  CHECK(lane.breaker(at(2000)) == BreakerState::Closed);
  CHECK(lane.stats().trips == 1);
}

TEST_CASE("core.transfer_lane: three refusals for want of balance in a row block an asset") {
  TransferLane lane(VenueId{0}, LaneConfig{});
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  CHECK_FALSE(lane.note_asset_refusal("SOL", false));  // another refusal starts again
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  lane.note_asset_done("SOL");  // so does a transfer done
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  CHECK_FALSE(lane.note_asset_refusal("SOL", true));
  CHECK(lane.note_asset_refusal("SOL", true));
  CHECK(lane.blocked("SOL"));
  CHECK_FALSE(lane.blocked("ADA"));
  CHECK(lane.stats().blocked_assets == 1);
}

TEST_CASE("core.transfer_lane: the plan takes the largest pieces, dust last, one per asset") {
  std::vector<SweepCandidate> c{cand(1, 2, "ETH", 50.0),
                                cand(0, 1, "BTC", 900.0),
                                cand(2, 1, "SOL", 1.0),  // dust
                                cand(0, 2, "BTC", 300.0),
                                cand(3, 3, "XRP", 80.0)};
  const auto none = [](std::string_view) { return false; };
  std::size_t n = plan_sweeps(std::span<SweepCandidate>(c), 10, none);
  REQUIRE(n == 4);
  CHECK(c[0].asset.view() == "BTC");
  CHECK(c[0].value == 900.0);
  CHECK(c[1].asset.view() == "XRP");
  CHECK(c[2].asset.view() == "ETH");
  CHECK(c[3].asset.view() == "SOL");
  // The room, and an asset already in flight.
  std::vector<SweepCandidate> d{cand(1, 2, "ETH", 50.0), cand(0, 1, "BTC", 900.0)};
  n = plan_sweeps(std::span<SweepCandidate>(d), 1, [](std::string_view a) { return a == "BTC"; });
  REQUIRE(n == 1);
  CHECK(d[0].asset.view() == "ETH");
}

TEST_CASE("core.transfer_lane: a sequence number and client id stay unique across restarts") {
  TransferLane a(VenueId{2}, LaneConfig{});
  TransferLane b(VenueId{2}, LaneConfig{});
  const std::uint64_t s1 = a.next_seq(at(0));
  const std::uint64_t s2 = a.next_seq(at(0));
  const std::uint64_t s3 = b.next_seq(at(1));  // a later process
  CHECK(s1 != s2);
  CHECK(s1 != s3);
  FixedString<31> id;
  lane_client_id(id, VenueId{2}, s1);
  CHECK(id.view().substr(0, 3) == "fs2");
  CHECK(id.size() <= TransferRequest::kMaxClientId);
  CHECK(lane_client_id(VenueId{2}, s1) == std::string(id.view()));
  for (const char ch : id.view()) CHECK(((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z')));
}

// ---- worker -------------------------------------------------------------------------------------

namespace {

// A venue that answers each request from a script, and the records the worker sends the engine.
struct FakePort final : TreasuryPort {
  std::deque<TransferResult> submits;
  std::deque<TransferResult> statuses;
  std::vector<TransferRequest> sent;
  std::vector<std::string> asked;
  struct Rec {
    TreasuryEventKind kind;
    std::uint64_t seq;
    TransferState state;
    bool lane;
  };
  std::vector<Rec> records;
  TransferResult submit(const TransferRequest& r) override {
    sent.push_back(r);
    REQUIRE_FALSE(submits.empty());
    TransferResult x = submits.front();
    submits.pop_front();
    return x;
  }
  TransferResult status(const TransferRequest& r) override {
    asked.push_back(r.client_id);
    REQUIRE_FALSE(statuses.empty());
    TransferResult x = statuses.front();
    statuses.pop_front();
    return x;
  }
  void record(const TreasuryEvent& e) override {
    records.push_back(
        {e.kind, e.seq, e.result != nullptr ? e.result->state : TransferState::Unknown, e.lane});
  }
};

LanePool lane_pool(FakePort& port, std::string state_file = {}) {
  LanePool p;
  p.primary = VenueId{0};
  p.cfg.status_interval = seconds(10);
  p.cfg.timeout = seconds(60);
  p.cfg.state_file = std::move(state_file);
  PoolPlan plan;
  REQUIRE(plan.add(VenueId{1}, VenueId{0}));
  p.members = plan.members(VenueId{0});
  p.names[0] = "a";
  p.names[1] = "b";
  p.port = &port;
  return p;
}

LaneIntent send_of(std::uint64_t seq) {
  LaneIntent in;
  in.primary = VenueId{0};
  in.from = VenueId{1};
  in.to = VenueId{0};
  in.asset.assign("BTC");
  in.amount = 5'000'000;
  in.seq = seq;
  return in;
}

struct Queue {
  std::deque<LaneIntent> q;
  TransferWorker::Pull pull() {
    return [this](LaneIntent& out) {
      if (q.empty()) return false;
      out = q.front();
      q.pop_front();
      return true;
    };
  }
};

}  // namespace

TEST_CASE("core.transfer_worker: sends, reports, and drops what the engine confirmed") {
  FakePort port;
  TransferWorker w({lane_pool(port)});
  REQUIRE(w.open());
  Queue q;
  q.q.push_back(send_of(7));
  port.submits.push_back({TransferState::Pending, "123", {}});
  w.step(at(0).ns, q.pull());
  REQUIRE(port.sent.size() == 1);
  CHECK(port.sent[0].client_id == lane_client_id(VenueId{0}, 7));
  CHECK(port.sent[0].amount.raw == 5'000'000);
  REQUIRE(port.records.size() == 1);
  CHECK(port.records[0].kind == TreasuryEventKind::Sent);
  CHECK(port.records[0].state == TransferState::Pending);
  CHECK(port.records[0].lane);
  CHECK(w.in_flight(0).size() == 1);
  // The engine saw both flows: nothing is asked for.
  LaneIntent done;
  done.kind = LaneIntent::Kind::Resolved;
  done.primary = VenueId{0};
  done.seq = 7;
  q.q.push_back(done);
  w.step(at(1).ns, q.pull());
  CHECK(w.in_flight(0).empty());
  w.step(at(30).ns, q.pull());
  CHECK(port.asked.empty());
}

TEST_CASE("core.transfer_worker: an unconfirmed or lost transfer is asked for until it resolves") {
  FakePort port;
  TransferWorker w({lane_pool(port)});
  REQUIRE(w.open());
  Queue q;
  q.q.push_back(send_of(1));
  q.q.push_back(send_of(2));
  q.q.push_back(send_of(3));
  port.submits.push_back({TransferState::Pending, "1", {}});
  port.submits.push_back({TransferState::Unknown, {}, "timeout"});
  TransferResult refused{TransferState::Failed, {}, "no", -9000, 400};
  port.submits.push_back(refused);
  w.step(at(0).ns, q.pull());
  // 1 answered Pending, 2 without an answer (reported as Sent, Unknown), 3 refused.
  REQUIRE(port.records.size() == 3);
  CHECK(port.records[1].kind == TreasuryEventKind::Sent);
  CHECK(port.records[1].state == TransferState::Unknown);
  CHECK(port.records[2].kind == TreasuryEventKind::Failed);
  CHECK(w.in_flight(0).size() == 2);
  CHECK(w.next_due_ns() == at(10).ns);
  // Ten seconds on: both asked for; 1 is done, 2 not found yet.
  port.statuses.push_back({TransferState::Done, "1", {}});
  port.statuses.push_back({TransferState::NotFound, {}, {}});
  w.step(at(10).ns, q.pull());
  CHECK(port.records.back().kind == TreasuryEventKind::Done);
  CHECK(w.in_flight(0).size() == 1);
  // Not found past timeout_s: failed, nothing moved.
  for (int k = 2; k <= 6; ++k) {
    port.statuses.push_back({TransferState::NotFound, {}, {}});
    w.step(at(10 * k).ns, q.pull());
  }
  CHECK(w.in_flight(0).empty());
  CHECK(port.records.back().kind == TreasuryEventKind::Failed);
  CHECK(port.records.back().seq == 2);
  CHECK(w.stats().failed == 2);
}

TEST_CASE("core.transfer_worker: the ledger outlives the process, and the next one holds again") {
  const auto file = (fastmm::test::tmp_dir() / "lane.transfers").string();
  std::filesystem::remove(file);
  {
    FakePort port;
    TransferWorker w({lane_pool(port, file)});
    REQUIRE(w.open());
    Queue q;
    q.q.push_back(send_of(42));
    port.submits.push_back({TransferState::Unknown, {}, "connection reset"});
    w.step(at(0).ns, q.pull());
    const auto parsed = TransferWorker::parse(lane_pool(port), w.serialize(0));
    REQUIRE(parsed);
    REQUIRE(parsed->size() == 1);
    CHECK((*parsed)[0].seq == 42);
    CHECK((*parsed)[0].req.from == VenueId{1});
  }
  FakePort port;
  TransferWorker w({lane_pool(port, file)});
  REQUIRE(w.open());
  REQUIRE(w.in_flight(0).size() == 1);
  // The first step holds it again (Reserve) and asks for it at once.
  port.statuses.push_back({TransferState::Done, "9", {}});
  Queue q;
  w.step(at(5).ns, q.pull());
  REQUIRE(port.records.size() == 2);
  CHECK(port.records[0].kind == TreasuryEventKind::Reserve);
  CHECK(port.records[0].seq == 42);
  CHECK(port.records[1].kind == TreasuryEventKind::Done);
  CHECK(w.in_flight(0).empty());
  // A ledger naming an account outside the pool is refused.
  CHECK_FALSE(
      TransferWorker::parse(lane_pool(port), "fastmm-transfers 1\n1 fs01 a zz BTC 5 0 0\n"));
  std::filesystem::remove(file);
}
