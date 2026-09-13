// Property test: MatchingEngine vs a naive std::map<Price, std::deque<Order>> oracle over
// seeded random operation sequences (limit GTC / IOC / market / cancel), comparing the fill
// stream and the aggregated book after every operation. CI: 200 seeds x 5000 ops.
#include "test_support.hpp"

#include "fastmm/core/rng.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {

struct Fill {
  std::uint64_t maker;
  std::uint64_t taker;
  std::int64_t px;
  std::int64_t qty;
  bool operator==(const Fill&) const = default;
};

// ---- oracle ---------------------------------------------------------------------------------
struct OracleOrder {
  std::uint64_t id;
  std::int64_t leaves;
};

class Oracle {
 public:
  std::vector<Fill> fills;
  std::map<std::int64_t, std::deque<OracleOrder>> bids;  // price -> FIFO
  std::map<std::int64_t, std::deque<OracleOrder>> asks;

  void submit(
      std::uint64_t id, Side side, std::int64_t px, std::int64_t qty, bool ioc, bool market) {
    std::int64_t leaves = qty;
    auto& opp = side == Side::Buy ? asks : bids;
    while (leaves > 0 && !opp.empty()) {
      auto it = side == Side::Buy ? opp.begin() : std::prev(opp.end());
      const std::int64_t best = it->first;
      if (!market && (side == Side::Buy ? px < best : px > best)) break;
      auto& q = it->second;
      while (leaves > 0 && !q.empty()) {
        OracleOrder& m = q.front();
        const std::int64_t f = std::min(leaves, m.leaves);
        fills.push_back({m.id, id, best, f});
        m.leaves -= f;
        leaves -= f;
        if (m.leaves == 0) q.pop_front();
      }
      if (q.empty()) opp.erase(it);
    }
    if (leaves > 0 && !ioc && !market) {
      (side == Side::Buy ? bids : asks)[px].push_back({id, leaves});
    }
  }
  bool cancel(std::uint64_t id) {
    for (auto* book : {&bids, &asks}) {
      for (auto it = book->begin(); it != book->end(); ++it) {
        auto& q = it->second;
        for (auto o = q.begin(); o != q.end(); ++o) {
          if (o->id == id) {
            q.erase(o);
            if (q.empty()) book->erase(it);
            return true;
          }
        }
      }
    }
    return false;
  }
  std::vector<Level> levels(Side s) const {
    std::vector<Level> out;
    const auto& book = s == Side::Buy ? bids : asks;
    auto emit = [&](const auto& kv) {
      std::int64_t sum = 0;
      for (const OracleOrder& o : kv.second) sum += o.leaves;
      out.push_back(Level{Price::from_raw(kv.first), Qty::from_raw(sum)});
    };
    if (s == Side::Buy) {
      for (auto it = book.rbegin(); it != book.rend(); ++it) emit(*it);
    } else {
      for (const auto& kv : book) emit(kv);
    }
    return out;
  }
};

struct FillRecorder final : MatchingSink {
  std::vector<Fill> fills;
  void on_fill(const SimOrder& m, const SimOrder& t, Price p, Qty q, Timestamp) override {
    fills.push_back({m.cl_ord_id.value, t.cl_ord_id.value, p.raw, q.raw});
  }
};

int env_int(const char* name, int def) {
  const char* v = std::getenv(name);
  return v == nullptr ? def : std::atoi(v);
}

void run_seed(std::uint64_t seed, int ops) {
  Xoshiro256ss rng(seed);
  FillRecorder rec;
  MatchingEngine me(1, &rec);
  Oracle oracle;
  std::vector<std::uint64_t> live;  // ids that may still be resting
  std::uint64_t next_id = 1;
  const std::int64_t tick = 100;  // raw units
  std::int64_t centre = 10'000 * tick;
  Timestamp now{1};
  for (int i = 0; i < ops; ++i) {
    now.ns += 1;
    const std::uint64_t op = rng.uniform(100);
    if (op < 60 || live.empty()) {
      // limit order near the centre; 15 % of them are IOC
      const Side side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t off = rng.between(-8, 8) * tick;
      const std::int64_t px = centre + off;
      const std::int64_t qty = rng.between(1, 5) * kFixedScale;
      const bool ioc = rng.uniform(100) < 15;
      const std::uint64_t id = next_id++;
      NewOrder o;
      o.account = 0;
      o.cl_ord_id = ClientOrderId{id};
      o.instrument = InstrumentId{0};
      o.side = side;
      o.type = OrderType::Limit;
      o.tif = ioc ? TimeInForce::Ioc : TimeInForce::Gtc;
      o.price = Price::from_raw(px);
      o.qty = Qty::from_raw(qty);
      const SubmitResult r = me.submit(o, now);
      REQUIRE(r.accepted());
      oracle.submit(id, side, px, qty, ioc, false);
      if (!ioc) live.push_back(id);
    } else if (op < 70) {
      const Side side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t qty = rng.between(1, 10) * kFixedScale;
      const std::uint64_t id = next_id++;
      NewOrder o;
      o.account = 0;
      o.cl_ord_id = ClientOrderId{id};
      o.instrument = InstrumentId{0};
      o.side = side;
      o.type = OrderType::Market;
      o.qty = Qty::from_raw(qty);
      REQUIRE(me.submit(o, now).accepted());
      oracle.submit(id, side, 0, qty, true, true);
    } else if (op < 95) {
      const std::size_t k = rng.uniform(live.size());
      const std::uint64_t id = live[k];
      live[k] = live.back();
      live.pop_back();
      const bool a = me.cancel(0, ClientOrderId{id}, now);
      const bool b = oracle.cancel(id);
      REQUIRE(a == b);
    } else {
      centre += rng.between(-2, 2) * tick;  // drift the arrival centre
    }
    // book equality after every op
    for (Side s : {Side::Buy, Side::Sell}) {
      const std::vector<Level> want = oracle.levels(s);
      std::vector<Level> got(want.size() + 1);
      const std::size_t n = me.l2_snapshot(InstrumentId{0}, s, got.data(), got.size());
      REQUIRE(n == want.size());
      for (std::size_t j = 0; j < n; ++j) REQUIRE(got[j] == want[j]);
    }
  }
  REQUIRE(rec.fills.size() == oracle.fills.size());
  for (std::size_t j = 0; j < rec.fills.size(); ++j) REQUIRE(rec.fills[j] == oracle.fills[j]);
}

}  // namespace

TEST_CASE("sim.matching.property: engine == std::map oracle over random ops (seeded)") {
  const int seeds = env_int("FASTMM_PROPERTY_SEEDS", 200);
  const int ops = env_int("FASTMM_PROPERTY_OPS", 5000);
  for (int s = 0; s < seeds; ++s) {
    CAPTURE(s);
    run_seed(static_cast<std::uint64_t>(s) * 7919U + 1U, ops);
  }
}

TEST_CASE("sim.matching.property: deterministic - same seed, same fill stream and ids") {
  auto run = [](std::uint64_t seed) {
    Xoshiro256ss rng(seed);
    FillRecorder rec;
    MatchingEngine me(1, &rec);
    for (std::uint64_t i = 1; i <= 2000; ++i) {
      NewOrder o;
      o.account = static_cast<AccountId>(i % 3);
      o.cl_ord_id = ClientOrderId{i};
      o.instrument = InstrumentId{0};
      o.side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
      o.type = rng.uniform(10) == 0 ? OrderType::Market : OrderType::Limit;
      o.price = Price::from_raw((10'000 + rng.between(-5, 5)) * 100);
      o.qty = Qty::from_raw(rng.between(1, 4) * kFixedScale);
      me.submit(o, Timestamp{static_cast<std::int64_t>(i)});
    }
    return std::make_pair(rec.fills, me.update_id(InstrumentId{0}));
  };
  const auto a = run(42);
  const auto b = run(42);
  const auto c = run(43);
  CHECK(a.first == b.first);
  CHECK(a.second == b.second);
  CHECK(a.first != c.first);
}
