// ITCH simulation property (plan 7): seeded random order flow into the simulator's
// MatchingEngine; its book events are published as TotalView-ITCH 5.0 messages (ItchFlow in
// itch_sim_util.hpp, which lists the publishing rules), decoded by ItchDecoder and applied to an
// L3Book. After every step the L3 book must equal the matching engine's book: the same levels
// with the same quantities, and at every level the same orders (ITCH reference numbers) with the
// same leaves in the same FIFO order.
#include "itch_sim_util.hpp"
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/core/book/l3_book.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;
using namespace fastmm::codecs;
using fastmm::codecs::test::FlowDriver;
using fastmm::codecs::test::ItchFlow;
using fastmm::codecs::test::kSimInst;
using fastmm::codecs::test::kSimLocate;
using fastmm::codecs::test::kSimOtherLocate;
using fastmm::codecs::test::kSimTick;
using fastmm::codecs::test::RecordingSink;

namespace {

constexpr L3BookConfig kBookCfg{.price_window_ticks = 1U << 12, .max_orders = 1U << 14};

// ItchFlow -> ItchDecoder -> RecordingSink -> L3Book.
class ItchConsumer {
 public:
  explicit ItchConsumer(const L3BookConfig& cfg = kBookCfg) : book_(kSimTick, cfg) {
    REQUIRE(dec_.add_symbol("FMSIM", kSimInst));
    flow_.start();
    REQUIRE(dec_.instrument(kSimLocate) == kSimInst);
    REQUIRE_FALSE(dec_.instrument(kSimOtherLocate).valid());
  }

  [[nodiscard]] ItchFlow& flow() noexcept { return flow_; }
  [[nodiscard]] const ItchFlow& flow() const noexcept { return flow_; }
  [[nodiscard]] const L3Book& book() const noexcept { return book_; }
  [[nodiscard]] const itch::ItchDecoder& decoder() const noexcept { return dec_; }
  [[nodiscard]] std::uint64_t trades_seen() const noexcept { return trades_; }
  [[nodiscard]] std::uint64_t replaces_seen() const noexcept { return replaces_; }
  [[nodiscard]] std::uint64_t partial_cancels_seen() const noexcept { return partials_; }
  [[nodiscard]] std::uint64_t priced_execs_seen() const noexcept { return priced_execs_; }

 private:
  void on_itch(std::span<const std::byte> msg) {
    const FrameView f{msg, msg.size(), 0};
    const venues::ParseStatus st = dec_.decode(f, ++rx_, rec_.sink);
    REQUIRE((st == venues::ParseStatus::Ok || st == venues::ParseStatus::Ignored));
    while (const std::byte* p = rec_.ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      REQUIRE(h->instrument == kSimInst);
      switch (h->type) {
        case EventType::OrderAddL3: {
          const auto& m = msg_cast<OrderAddL3Msg>(h);
          REQUIRE(book_.add(m.order_ref, m.side, m.price, m.qty) == L3Error::None);
          break;
        }
        case EventType::OrderExecL3: {
          const auto& m = msg_cast<OrderExecL3Msg>(h);
          Price at;
          REQUIRE(book_.execute(m.order_ref, m.exec_qty, &at) == L3Error::None);
          if (!m.exec_price.is_zero()) {
            CHECK(m.exec_price == at);  // the engine trades at the maker's price
            ++priced_execs_;
          }
          break;
        }
        case EventType::OrderCancelL3: {
          const auto& m = msg_cast<OrderCancelL3Msg>(h);
          if (!m.canceled_qty.is_zero()) ++partials_;
          REQUIRE(book_.cancel(m.order_ref, m.canceled_qty) == L3Error::None);
          break;
        }
        case EventType::OrderReplaceL3: {
          const auto& m = msg_cast<OrderReplaceL3Msg>(h);
          REQUIRE(book_.replace(m.old_order_ref, m.new_order_ref, m.price, m.qty) == L3Error::None);
          ++replaces_;
          break;
        }
        case EventType::Trade:
          ++trades_;
          break;
        default:
          FAIL("unexpected event type");
      }
      rec_.ring.release();
    }
  }

  itch::ItchDecoder dec_;
  RecordingSink rec_{1U << 16};
  L3Book book_;
  ItchFlow flow_{[this](std::span<const std::byte> m) { on_itch(m); }};
  std::int64_t rx_ = 0;
  std::uint64_t trades_ = 0;
  std::uint64_t replaces_ = 0;
  std::uint64_t partials_ = 0;
  std::uint64_t priced_execs_ = 0;
};

// Engine book (levels best-first, per-level FIFO via the prev/next handles) == L3 book.
void check_books(const MatchingEngine& eng, const ItchConsumer& pub) {
  const L3Book& book = pub.book();
  const SimBook& sb = eng.book(kSimInst);
  const auto& resting = pub.flow().resting();
  std::map<std::tuple<int, std::int64_t, std::uint32_t>, const SimOrder*> by_prev;
  eng.for_each_open_order(
      [&](const SimOrder& o) { by_prev[{static_cast<int>(o.side), o.price.raw, o.prev}] = &o; });
  REQUIRE(eng.open_orders() == resting.size());
  REQUIRE(book.order_count() == resting.size());
  for (Side s : {Side::Buy, Side::Sell}) {
    const SimBook::Levels& levels = sb.levels(s);
    REQUIRE(book.depth(s) == levels.size());
    const Level best = s == Side::Buy ? book.best_bid() : book.best_ask();
    if (levels.empty()) {
      CHECK(best.qty.is_zero());
      continue;
    }
    CHECK(best.price == levels.back().price);
    CHECK(best.qty == levels.back().qty);
    for (const PriceLevel& pl : levels) {
      std::vector<std::pair<std::uint64_t, std::int64_t>> expect;
      std::uint32_t handle = pl.head;
      std::uint32_t prev = kNullHandle;
      while (handle != kNullHandle) {
        const auto it = by_prev.find({static_cast<int>(s), pl.price.raw, prev});
        REQUIRE(it != by_prev.end());
        const SimOrder& o = *it->second;
        expect.emplace_back(resting.at(o.order_id).ref, o.leaves().raw);
        prev = handle;
        handle = o.next;
      }
      REQUIRE(expect.size() == pl.count);
      std::vector<std::pair<std::uint64_t, std::int64_t>> got;
      std::int64_t level_qty = 0;
      book.for_each_order_at(s, pl.price, [&](const L3Order& o) {
        got.emplace_back(o.ref, o.qty.raw);
        level_qty += o.qty.raw;
      });
      REQUIRE(level_qty == pl.qty.raw);
      REQUIRE(got == expect);
    }
  }
}

}  // namespace

TEST_CASE("codecs.itch: simulated order flow through ITCH rebuilds the matching engine book") {
  std::uint64_t total_trades = 0;
  std::uint64_t total_replaces = 0;
  std::uint64_t total_partials = 0;
  std::uint64_t total_priced = 0;
  std::uint64_t total_unknown_locate = 0;
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    CAPTURE(seed);
    auto pub = std::make_unique<ItchConsumer>();
    auto eng = std::make_unique<MatchingEngine>(1, &pub->flow());
    FlowDriver driver(seed, *eng, pub->flow());
    for (int step = 0; step < 1500; ++step) {
      CAPTURE(step);
      driver.step(step);
      check_books(*eng, *pub);
    }
    total_trades += pub->trades_seen();
    total_replaces += pub->replaces_seen();
    total_partials += pub->partial_cancels_seen();
    total_priced += pub->priced_execs_seen();
    total_unknown_locate += pub->decoder().stats().unknown_locate;
    CHECK(pub->decoder().stats().malformed == 0);
    CHECK(pub->decoder().stats().overflow == 0);
    CHECK(eng->stats().fills > 0);
  }
  // Every publishing path was exercised.
  CHECK(total_trades > 0);
  CHECK(total_replaces > 0);
  CHECK(total_partials > 0);
  CHECK(total_priced > 0);
  CHECK(total_unknown_locate == 16 * 30);
}

// Stub quotes far from the market and a drifting reference price through a 64-tick window: most
// levels live in the overflow store and the touches keep leaving the window.
TEST_CASE("codecs.itch: stub quotes and a drifting market through a small L3 window") {
  std::uint32_t recentres = 0;
  std::size_t max_overflow = 0;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    CAPTURE(seed);
    auto pub = std::make_unique<ItchConsumer>(
        L3BookConfig{.price_window_ticks = 64, .max_orders = 1U << 14, .max_overflow_levels = 512});
    auto eng = std::make_unique<MatchingEngine>(1, &pub->flow());
    FlowDriver driver(seed, *eng, pub->flow(), {.stub_pct = 5, .drift_every = 10});
    for (int step = 0; step < 1500; ++step) {
      CAPTURE(step);
      driver.step(step);
      check_books(*eng, *pub);
      max_overflow = std::max(
          max_overflow,
          pub->book().overflow_levels(Side::Buy) + pub->book().overflow_levels(Side::Sell));
    }
    recentres += pub->book().recentre_count();
    CHECK(pub->book().recentre_failures() == 0);
  }
  CHECK(recentres > 0);
  CHECK(max_overflow > 0);
}
