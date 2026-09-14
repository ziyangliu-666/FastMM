// ITCH simulation property (plan 7): seeded random order flow into the simulator's
// MatchingEngine; its book events are published as TotalView-ITCH 5.0 messages (ItchEncoder),
// decoded by ItchDecoder and applied to an L3Book. After every step the L3 book must equal the
// matching engine's book: the same levels with the same quantities, and at every level the same
// orders (ITCH reference numbers) with the same leaves in the same FIFO order.
//
// Publishing rules (what an exchange feed does with each engine effect):
//   order rests                          -> A (or F) with the resting quantity
//   resting order executed               -> E, or C with the execution price
//   resting order cancelled              -> D
//   replace kept in place (same price,   -> X for the reduction; the ITCH reference is kept
//   qty <= leaves: priority kept)           because the queue position is kept
//   replace re-entered, rests untouched  -> U (old reference -> new reference, back of queue)
//   replace re-entered and traded        -> D for the old order, E for the makers, A for the rest
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/core/book/l3_book.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <array>
#include <map>
#include <memory>
#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;
using namespace fastmm::codecs;
using fastmm::codecs::test::RecordingSink;

namespace {

const Price kTick = Price::from_decimal("0.01").value();
constexpr std::uint16_t kLocate = 12;
constexpr std::uint16_t kOtherLocate = 13;
constexpr InstrumentId kInst{0};
using Book = L3Book<1U << 12, 1U << 14>;

Price ticks(std::int64_t t) {
  return Price::from_raw(t * kTick.raw);
}

struct Resting {
  std::uint64_t ref;
  Side side;
  Price price;
  Qty leaves;
};

class ItchPublisher final : public MatchingSink {
 public:
  ItchPublisher() : book_(kTick) {
    REQUIRE(dec_.add_symbol("FMSIM", kInst));
    publish_raw(enc_.system_event(buf_, next_ts(), 'O'));
    publish_raw(enc_.stock_directory(buf_, kLocate, next_ts(), "FMSIM"));
    publish_raw(enc_.stock_directory(buf_, kOtherLocate, next_ts(), "OTHER"));
    REQUIRE(dec_.instrument(kLocate) == kInst);
    REQUIRE_FALSE(dec_.instrument(kOtherLocate).valid());
  }

  // ---- MatchingSink ------------------------------------------------------------------------
  void on_fill(const SimOrder& maker, const SimOrder&, Price px, Qty qty, Timestamp) override {
    const auto it = resting_.find(maker.order_id);
    REQUIRE(it != resting_.end());
    const std::uint64_t match = ++match_;
    if (match % 3 == 0) {
      publish_raw(enc_.order_executed_with_price(
          buf_, kLocate, next_ts(), it->second.ref, qty, match, px, true));
    } else {
      publish_raw(enc_.order_executed(buf_, kLocate, next_ts(), it->second.ref, qty, match));
    }
    it->second.leaves -= qty;
    if (it->second.leaves.is_zero()) resting_.erase(it);
    if (match % 7 == 0)  // a non-displayable trade print alongside (does not touch the book)
      publish_raw(
          enc_.trade(buf_, kLocate, next_ts(), Side::Buy, qty, "FMSIM", px, 1'000'000 + match));
  }
  void on_cancel(const SimOrder& o, CancelReason why, Timestamp) override {
    if (why == CancelReason::Replaced) return;  // resolved by after_replace()
    const auto it = resting_.find(o.order_id);
    if (it == resting_.end()) return;  // IOC / FOK / market remainder that never rested
    publish_raw(enc_.order_delete(buf_, kLocate, next_ts(), it->second.ref));
    resting_.erase(it);
  }

  // ---- driver hooks ------------------------------------------------------------------------
  void after_submit(const SubmitResult& r, const MatchingEngine& eng, const NewOrder& o) {
    if (!r.accepted() || r.resting.is_zero()) return;
    const SimOrder* s = eng.find(o.account, o.cl_ord_id);
    REQUIRE(s != nullptr);
    add_resting(*s);
  }

  void after_replace(const SimOrder& old,
                     const SubmitResult& r,
                     const MatchingEngine& eng,
                     AccountId account,
                     ClientOrderId new_id,
                     Price price,
                     Qty qty) {
    const auto it = resting_.find(old.order_id);
    REQUIRE(it != resting_.end());
    const Resting prev = it->second;
    const SimOrder* now = eng.find(account, new_id);
    const bool in_place = qty.is_positive() && price == old.price && qty <= old.leaves();
    if (in_place) {
      REQUIRE(now != nullptr);
      resting_.erase(it);
      if (qty < prev.leaves)
        publish_raw(enc_.order_cancel(buf_, kLocate, next_ts(), prev.ref, prev.leaves - qty));
      resting_[now->order_id] = Resting{prev.ref, prev.side, prev.price, qty};
      return;
    }
    resting_.erase(it);
    if (now != nullptr && r.filled.is_zero()) {
      const std::uint64_t ref = now->order_id;
      publish_raw(
          enc_.order_replace(buf_, kLocate, next_ts(), prev.ref, ref, now->leaves(), now->price));
      resting_[now->order_id] = Resting{ref, now->side, now->price, now->leaves()};
      return;
    }
    publish_raw(enc_.order_delete(buf_, kLocate, next_ts(), prev.ref));
    if (now != nullptr) add_resting(*now);
  }

  // Noise for an instrument the decoder is not subscribed to.
  void publish_foreign_add() {
    publish_raw(enc_.add_order(buf_,
                               kOtherLocate,
                               next_ts(),
                               9'000'000'000ULL + match_,
                               Side::Buy,
                               Qty::from_int(1),
                               "OTHER",
                               ticks(100)));
  }

  [[nodiscard]] const Book& book() const noexcept { return book_; }
  [[nodiscard]] const std::unordered_map<std::uint64_t, Resting>& resting() const noexcept {
    return resting_;
  }
  [[nodiscard]] const itch::ItchDecoder& decoder() const noexcept { return dec_; }
  [[nodiscard]] std::uint64_t trades_seen() const noexcept { return trades_; }
  [[nodiscard]] std::uint64_t replaces_seen() const noexcept { return replaces_; }
  [[nodiscard]] std::uint64_t partial_cancels_seen() const noexcept { return partials_; }
  [[nodiscard]] std::uint64_t priced_execs_seen() const noexcept { return priced_execs_; }

 private:
  void add_resting(const SimOrder& s) {
    const std::uint64_t ref = s.order_id;
    const std::size_t n =
        s.order_id % 2 == 0
            ? enc_.add_order(buf_, kLocate, next_ts(), ref, s.side, s.leaves(), "FMSIM", s.price)
            : enc_.add_order_mpid(
                  buf_, kLocate, next_ts(), ref, s.side, s.leaves(), "FMSIM", s.price, "FMMM");
    publish_raw(n);
    resting_[s.order_id] = Resting{ref, s.side, s.price, s.leaves()};
  }

  std::uint64_t next_ts() noexcept { return ts_ += 1'000; }

  // Encoded message -> ItchDecoder -> RecordingSink -> L3Book.
  void publish_raw(std::size_t n) {
    REQUIRE(n != 0);
    const FrameView f{std::span<const std::byte>(buf_.data(), n), n, 0};
    const venues::ParseStatus st = dec_.decode(f, static_cast<std::int64_t>(ts_), rec_.sink);
    REQUIRE((st == venues::ParseStatus::Ok || st == venues::ParseStatus::Ignored));
    while (const std::byte* p = rec_.ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      REQUIRE(h->instrument == kInst);
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

  itch::ItchEncoder enc_;
  itch::ItchDecoder dec_;
  RecordingSink rec_{1U << 16};
  Book book_;
  std::array<std::byte, 64> buf_{};
  std::unordered_map<std::uint64_t, Resting> resting_;  // engine order_id -> ITCH view
  std::uint64_t ts_ = 34'200'000'000'000ULL;
  std::uint64_t match_ = 0;
  std::uint64_t trades_ = 0;
  std::uint64_t replaces_ = 0;
  std::uint64_t partials_ = 0;
  std::uint64_t priced_execs_ = 0;
};

// Engine book (levels best-first, per-level FIFO via the prev/next handles) == L3 book.
void check_books(const MatchingEngine& eng, const ItchPublisher& pub) {
  const Book& book = pub.book();
  const SimBook& sb = eng.book(kInst);
  std::map<std::tuple<int, std::int64_t, std::uint32_t>, const SimOrder*> by_prev;
  eng.for_each_open_order(
      [&](const SimOrder& o) { by_prev[{static_cast<int>(o.side), o.price.raw, o.prev}] = &o; });
  REQUIRE(eng.open_orders() == pub.resting().size());
  REQUIRE(book.order_count() == pub.resting().size());
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
        expect.emplace_back(pub.resting().at(o.order_id).ref, o.leaves().raw);
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

struct LiveOrder {
  AccountId account;
  ClientOrderId id;
};

}  // namespace

TEST_CASE("codecs.itch: simulated order flow through ITCH rebuilds the matching engine book") {
  std::uint64_t total_trades = 0;
  std::uint64_t total_replaces = 0;
  std::uint64_t total_partials = 0;
  std::uint64_t total_priced = 0;
  std::uint64_t total_unknown_locate = 0;
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    CAPTURE(seed);
    std::mt19937_64 rng(seed);
    auto uni = [&](std::int64_t lo, std::int64_t hi) {
      return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    auto pub = std::make_unique<ItchPublisher>();
    auto eng = std::make_unique<MatchingEngine>(1, pub.get());
    std::vector<LiveOrder> live;
    std::uint32_t next_id = 1;
    Timestamp now{1'700'000'000'000'000'000};
    constexpr std::int64_t kMid = 10'000;

    for (int step = 0; step < 1500; ++step) {
      CAPTURE(step);
      now += nanoseconds(1'000);
      const std::int64_t action = uni(0, 99);
      if (action < 50 || live.empty()) {
        NewOrder o;
        o.account = static_cast<AccountId>(uni(1, 3));
        o.cl_ord_id = make_cl_ord_id(1, next_id++);
        o.instrument = kInst;
        o.side = uni(0, 1) == 0 ? Side::Buy : Side::Sell;
        const std::int64_t offset = uni(-4, 25);
        o.price = ticks(o.side == Side::Buy ? kMid - offset : kMid + offset);
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
        const SubmitResult r = eng->submit(o, now);
        pub->after_submit(r, *eng, o);
        if (r.accepted() && r.resting.is_positive()) live.push_back({o.account, o.cl_ord_id});
      } else {
        const std::size_t pick =
            static_cast<std::size_t>(uni(0, static_cast<std::int64_t>(live.size()) - 1));
        const LiveOrder target = live[pick];
        const SimOrder* o = eng->find(target.account, target.id);
        if (o == nullptr) {  // filled meanwhile
          live[pick] = live.back();
          live.pop_back();
        } else if (action < 75) {
          REQUIRE(eng->cancel(target.account, target.id, now));
          live[pick] = live.back();
          live.pop_back();
        } else {
          const SimOrder old = *o;
          Price price = old.price;
          Qty qty = old.leaves();
          if (uni(0, 99) < 40) {
            qty = Qty::from_int(uni(1, old.leaves().raw / kFixedScale));  // amend down in place
          } else {
            const std::int64_t offset = uni(-4, 25);
            price = ticks(old.side == Side::Buy ? kMid - offset : kMid + offset);
            qty = Qty::from_int(uni(1, 25));
          }
          const ClientOrderId new_id = make_cl_ord_id(1, next_id++);
          const SubmitResult r = eng->replace(target.account, target.id, new_id, price, qty, now);
          pub->after_replace(old, r, *eng, target.account, new_id, price, qty);
          live[pick] = live.back();
          live.pop_back();
          if (eng->find(target.account, new_id) != nullptr)
            live.push_back({target.account, new_id});
        }
      }
      if (step % 50 == 0) pub->publish_foreign_add();
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
