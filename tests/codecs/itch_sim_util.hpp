#pragma once
// Seeded random order flow into the simulator's MatchingEngine, published as TotalView-ITCH 5.0
// messages (ItchEncoder) to a callback. Shared by itch_sim_property_test.cpp and
// itch_l2_bridge_test.cpp.
//
// Publishing rules (what an exchange feed does with each engine effect):
//   order rests                          -> A (or F) with the resting quantity
//   resting order executed               -> E, or C with the execution price
//   resting order cancelled              -> D
//   replace kept in place (same price,   -> X for the reduction; the ITCH reference is kept
//   qty <= leaves: priority kept)           because the queue position is kept
//   replace re-entered, rests untouched  -> U (old reference -> new reference, back of queue)
//   replace re-entered and traded        -> D for the old order, E for the makers, A for the rest
#include "test_support.hpp"

#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

namespace fastmm::codecs::test {

inline const Price kSimTick = Price::from_decimal("0.01").value();
inline constexpr std::uint16_t kSimLocate = 12;
inline constexpr std::uint16_t kSimOtherLocate = 13;
inline constexpr InstrumentId kSimInst{0};

inline Price sim_ticks(std::int64_t t) {
  return Price::from_raw(t * kSimTick.raw);
}

struct SimResting {
  std::uint64_t ref;
  Side side;
  Price price;
  Qty leaves;
};

struct ItchFlowOptions {
  // Every k-th C (Order Executed With Price) has Printable = N; 0: all printable.
  std::uint64_t non_printable_every = 0;
};

class ItchFlow final : public sim::MatchingSink {
 public:
  using Out = std::function<void(std::span<const std::byte>)>;

  explicit ItchFlow(Out out, ItchFlowOptions opt = {}) : out_(std::move(out)), opt_(opt) {}

  // System Event 'O' and the directory: FMSIM on kSimLocate, OTHER on kSimOtherLocate.
  void start() {
    publish(enc_.system_event(buf_, next_ts(), 'O'));
    publish(enc_.stock_directory(buf_, kSimLocate, next_ts(), "FMSIM"));
    publish(enc_.stock_directory(buf_, kSimOtherLocate, next_ts(), "OTHER"));
  }

  // ---- MatchingSink ------------------------------------------------------------------------
  void on_fill(
      const sim::SimOrder& maker, const sim::SimOrder&, Price px, Qty qty, Timestamp) override {
    const auto it = resting_.find(maker.order_id);
    REQUIRE(it != resting_.end());
    const std::uint64_t match = ++match_;
    if (match % 3 == 0) {
      const bool printable =
          opt_.non_printable_every == 0 || ++priced_ % opt_.non_printable_every != 0;
      publish(enc_.order_executed_with_price(
          buf_, kSimLocate, next_ts(), it->second.ref, qty, match, px, printable));
    } else {
      publish(enc_.order_executed(buf_, kSimLocate, next_ts(), it->second.ref, qty, match));
    }
    it->second.leaves -= qty;
    if (it->second.leaves.is_zero()) resting_.erase(it);
    if (match % 7 == 0)  // a non-displayable trade print alongside (does not touch the book)
      publish(
          enc_.trade(buf_, kSimLocate, next_ts(), Side::Buy, qty, "FMSIM", px, 1'000'000 + match));
  }
  void on_cancel(const sim::SimOrder& o, sim::CancelReason why, Timestamp) override {
    if (why == sim::CancelReason::Replaced) return;  // resolved by after_replace()
    const auto it = resting_.find(o.order_id);
    if (it == resting_.end()) return;  // IOC / FOK / market remainder that never rested
    publish(enc_.order_delete(buf_, kSimLocate, next_ts(), it->second.ref));
    resting_.erase(it);
  }

  // ---- driver hooks ------------------------------------------------------------------------
  void after_submit(const sim::SubmitResult& r,
                    const sim::MatchingEngine& eng,
                    const sim::NewOrder& o) {
    if (!r.accepted() || r.resting.is_zero()) return;
    const sim::SimOrder* s = eng.find(o.account, o.cl_ord_id);
    REQUIRE(s != nullptr);
    add_resting(*s);
  }

  void after_replace(const sim::SimOrder& old,
                     const sim::SubmitResult& r,
                     const sim::MatchingEngine& eng,
                     sim::AccountId account,
                     ClientOrderId new_id,
                     Price price,
                     Qty qty) {
    const auto it = resting_.find(old.order_id);
    REQUIRE(it != resting_.end());
    const SimResting prev = it->second;
    const sim::SimOrder* now = eng.find(account, new_id);
    const bool in_place = qty.is_positive() && price == old.price && qty <= old.leaves();
    if (in_place) {
      REQUIRE(now != nullptr);
      resting_.erase(it);
      if (qty < prev.leaves)
        publish(enc_.order_cancel(buf_, kSimLocate, next_ts(), prev.ref, prev.leaves - qty));
      resting_[now->order_id] = SimResting{prev.ref, prev.side, prev.price, qty};
      return;
    }
    resting_.erase(it);
    if (now != nullptr && r.filled.is_zero()) {
      const std::uint64_t ref = now->order_id;
      publish(enc_.order_replace(
          buf_, kSimLocate, next_ts(), prev.ref, ref, now->leaves(), now->price));
      resting_[now->order_id] = SimResting{ref, now->side, now->price, now->leaves()};
      return;
    }
    publish(enc_.order_delete(buf_, kSimLocate, next_ts(), prev.ref));
    if (now != nullptr) add_resting(*now);
  }

  // Noise for an instrument the decoder is not subscribed to.
  void publish_foreign_add() {
    publish(enc_.add_order(buf_,
                           kSimOtherLocate,
                           next_ts(),
                           9'000'000'000ULL + match_,
                           Side::Buy,
                           Qty::from_int(1),
                           "OTHER",
                           sim_ticks(100)));
  }

  [[nodiscard]] const std::unordered_map<std::uint64_t, SimResting>& resting() const noexcept {
    return resting_;
  }

 private:
  void add_resting(const sim::SimOrder& s) {
    const std::uint64_t ref = s.order_id;
    const std::size_t n =
        s.order_id % 2 == 0
            ? enc_.add_order(buf_, kSimLocate, next_ts(), ref, s.side, s.leaves(), "FMSIM", s.price)
            : enc_.add_order_mpid(
                  buf_, kSimLocate, next_ts(), ref, s.side, s.leaves(), "FMSIM", s.price, "FMMM");
    publish(n);
    resting_[s.order_id] = SimResting{ref, s.side, s.price, s.leaves()};
  }

  std::uint64_t next_ts() noexcept { return ts_ += 1'000; }

  void publish(std::size_t n) {
    REQUIRE(n != 0);
    out_(std::span<const std::byte>(buf_.data(), n));
  }

  Out out_;
  ItchFlowOptions opt_;
  itch::ItchEncoder enc_;
  std::array<std::byte, 64> buf_{};
  std::unordered_map<std::uint64_t, SimResting> resting_;  // engine order_id -> ITCH view
  std::uint64_t ts_ = 34'200'000'000'000ULL;
  std::uint64_t match_ = 0;
  std::uint64_t priced_ = 0;
};

struct FlowDriverOptions {
  // Percent of new orders placed far from the market (stub quotes).
  std::int64_t stub_pct = 0;
  // Every k steps the reference price moves by -3..3 ticks; 0: fixed.
  int drift_every = 0;
};

// Random limit / IOC / FOK / post-only / market orders, cancels and replaces around a
// reference price (kMid ticks of 0.01).
class FlowDriver {
 public:
  FlowDriver(std::uint64_t seed,
             sim::MatchingEngine& eng,
             ItchFlow& flow,
             FlowDriverOptions opt = {})
      : rng_(seed), eng_(eng), flow_(flow), opt_(opt) {}

  std::int64_t uni(std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
  }

  void step(int step) {
    now_ += nanoseconds(1'000);
    if (opt_.drift_every > 0 && step % opt_.drift_every == 0) mid_ += uni(-3, 3);
    const std::int64_t action = uni(0, 99);
    if (action < 50 || live_.empty()) {
      sim::NewOrder o;
      o.account = static_cast<sim::AccountId>(uni(1, 3));
      o.cl_ord_id = make_cl_ord_id(1, next_id_++);
      o.instrument = kSimInst;
      o.side = uni(0, 1) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t offset = uni(-4, 25);
      o.price = sim_ticks(o.side == Side::Buy ? mid_ - offset : mid_ + offset);
      o.qty = Qty::from_int(uni(1, 20));
      const std::int64_t kind = uni(0, 99);
      if (kind < 4) {
        o.type = OrderType::Market;
        o.tif = TimeInForce::Ioc;
      } else if (kind < 10) {
        o.tif = TimeInForce::Ioc;
      } else if (kind < 15) {
        o.tif = TimeInForce::Fok;
      } else if (kind < 22) {
        o.type = OrderType::PostOnly;
      }
      if (opt_.stub_pct > 0 && uni(0, 99) < opt_.stub_pct) {
        o.type = OrderType::Limit;
        o.tif = TimeInForce::Gtc;
        o.price = sim_ticks(o.side == Side::Buy ? uni(1, 40) : 3 * mid_ + uni(0, 40));
      }
      const sim::SubmitResult r = eng_.submit(o, now_);
      flow_.after_submit(r, eng_, o);
      if (r.accepted() && r.resting.is_positive()) live_.push_back({o.account, o.cl_ord_id});
    } else {
      const std::size_t pick =
          static_cast<std::size_t>(uni(0, static_cast<std::int64_t>(live_.size()) - 1));
      const LiveOrder target = live_[pick];
      const sim::SimOrder* o = eng_.find(target.account, target.id);
      if (o == nullptr) {  // filled meanwhile
        live_[pick] = live_.back();
        live_.pop_back();
      } else if (action < 75) {
        REQUIRE(eng_.cancel(target.account, target.id, now_));
        live_[pick] = live_.back();
        live_.pop_back();
      } else {
        const sim::SimOrder old = *o;
        Price price = old.price;
        Qty qty = old.leaves();
        if (uni(0, 99) < 40) {
          qty = Qty::from_int(uni(1, old.leaves().raw / kFixedScale));  // amend down in place
        } else {
          const std::int64_t offset = uni(-4, 25);
          price = sim_ticks(old.side == Side::Buy ? mid_ - offset : mid_ + offset);
          qty = Qty::from_int(uni(1, 25));
        }
        const ClientOrderId new_id = make_cl_ord_id(1, next_id_++);
        const sim::SubmitResult r =
            eng_.replace(target.account, target.id, new_id, price, qty, now_);
        flow_.after_replace(old, r, eng_, target.account, new_id, price, qty);
        live_[pick] = live_.back();
        live_.pop_back();
        if (eng_.find(target.account, new_id) != nullptr) live_.push_back({target.account, new_id});
      }
    }
    if (step % 50 == 0) flow_.publish_foreign_add();
  }

 private:
  struct LiveOrder {
    sim::AccountId account;
    ClientOrderId id;
  };

  std::mt19937_64 rng_;
  sim::MatchingEngine& eng_;
  ItchFlow& flow_;
  FlowDriverOptions opt_;
  std::vector<LiveOrder> live_;
  std::uint32_t next_id_ = 1;
  Timestamp now_{1'700'000'000'000'000'000};
  std::int64_t mid_ = 10'000;
};

}  // namespace fastmm::codecs::test
