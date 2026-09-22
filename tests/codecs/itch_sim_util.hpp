#pragma once
// Seeded random order flow into the simulator's MatchingEngine, published as TotalView-ITCH 5.0
// messages by the simulator's ItchPublisher (fastmm/sim/itch/itch_publisher.hpp, which lists the
// publishing rules) to a callback. Shared by itch_sim_property_test.cpp and
// itch_l2_bridge_test.cpp. The publisher is set up to use every message type it can: F, C (with
// and without Printable), P prints and U for re-entered replaces.
#include "test_support.hpp"

#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/sim/itch/itch_publisher.hpp"
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

struct ItchFlowOptions {
  // Every k-th C (Order Executed With Price) has Printable = N; 0: all printable.
  std::uint64_t non_printable_every = 0;
};

// The engine's sink is publisher(); messages go to `out`.
class ItchFlow final : public sim::itch::ItchOutput {
 public:
  using Out = std::function<void(std::span<const std::byte>)>;

  explicit ItchFlow(Out out, ItchFlowOptions opt = {})
      : out_(std::move(out)),
        pub_(*this,
             numbering_,
             kSimLocate,
             "FMSIM",
             sim::itch::ItchPublisherOptions{.mpid_every = 2,
                                             .priced_exec_every = 3,
                                             .non_printable_every = opt.non_printable_every,
                                             .trade_print_every = 7,
                                             .replace_messages = true}) {}

  // System Event 'O' and the directory: FMSIM on kSimLocate, OTHER on kSimOtherLocate.
  void start() {
    emit(enc_.system_event(buf_, itch_timestamp(), 'O'));
    emit(enc_.stock_directory(buf_, kSimLocate, itch_timestamp(), "FMSIM"));
    emit(enc_.stock_directory(buf_, kSimOtherLocate, itch_timestamp(), "OTHER"));
  }

  // Noise for an instrument the decoder is not subscribed to.
  void publish_foreign_add() {
    emit(enc_.add_order(buf_,
                        kSimOtherLocate,
                        itch_timestamp(),
                        9'000'000'000ULL + ++foreign_,
                        Side::Buy,
                        Qty::from_int(1),
                        "OTHER",
                        sim_ticks(100)));
  }

  [[nodiscard]] sim::itch::ItchPublisher& publisher() noexcept { return pub_; }
  [[nodiscard]] const sim::itch::ItchPublisher& publisher() const noexcept { return pub_; }

  // ---- ItchOutput ----
  std::uint64_t itch_timestamp() noexcept override { return ts_ += 1'000; }
  void publish(std::span<const std::byte> msg) noexcept override { out_(msg); }

 private:
  void emit(std::size_t n) {
    REQUIRE(n != 0);
    out_(std::span<const std::byte>(buf_.data(), n));
  }

  Out out_;
  sim::itch::ItchNumbering numbering_;
  sim::itch::ItchPublisher pub_;
  itch::ItchEncoder enc_;
  std::array<std::byte, 64> buf_{};
  std::uint64_t ts_ = 34'200'000'000'000ULL;
  std::uint64_t foreign_ = 0;
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
      const sim::SubmitResult r = flow_.publisher().submit(eng_, o, now_);
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
        REQUIRE(flow_.publisher().cancel(eng_, target.account, target.id, now_));
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
        flow_.publisher().replace(eng_, target.account, target.id, new_id, price, qty, now_);
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
