// The strategy API as docs/reference/strategy-api.md documents it. The page's signature blocks are
// snippets of this file (tools/doc_snippets.py), so a change to a documented signature fails here
// first and then shows up in the page:
//   * AllHooks implements every hook with exactly the documented signature and runs in the
//     StrategyHarness; the test asserts that each hook fired (a wrong signature is a build error,
//     a hook the engine never calls would fail the count);
//   * the static_asserts pin the type of every documented context method, message field, Fill
//     field, parameter kind and helper.
#include "test_support.hpp"

#include "fastmm/strategies/factories.hpp"
#include "fastmm/strategies/module.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/strategy.hpp"
#include "fastmm/testing/strategy_harness.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace fastmm;

namespace docs {

// [start:params]
struct AllHooksParams {
  FASTMM_PARAMS(AllHooksParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
  FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
  FASTMM_PARAM(bool, hedge, false, false, true, "hedge fills")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000_qty, "quantity per side")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 1000_bps, "half spread, bps")
  FASTMM_PARAM_MS(timer_ms, milliseconds(100), milliseconds(1), milliseconds(60000), "timer, ms")

  // Optional: runs after all keys of a configure() call are applied.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (hedge && levels > 4) return "hedge supports at most 4 levels";
    return std::nullopt;
  }
};
// [end:params]

enum HookIndex : std::uint8_t {
  kStart,
  kStop,
  kBook,
  kBookTicker,
  kTrade,
  kOptionTicker,
  kFill,
  kOrderUpdate,
  kTimer,
  kConnection,
  kQuoting,
  kHookCount
};

class AllHooks : public StrategyBase<AllHooksParams> {
 public:
  static constexpr std::string_view name() noexcept { return "doc_all_hooks"; }

  // [start:hooks]
  void on_start(auto& ctx) noexcept {
    hit(kStart);
    static_cast<void>(ctx.every(params().timer_ms, /*tag=*/7));
  }
  void on_stop(auto& /*ctx*/) noexcept { hit(kStop); }
  void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
    hit(kBook);
    if (!book.is_valid()) return ctx.pull_quotes(id);
    DesiredQuotes q;
    q.bid(book.best_bid().price, params().quote_qty);
    q.ask(book.best_ask().price, params().quote_qty);
    static_cast<void>(ctx.set_quotes(id, q));
  }
  void on_book_ticker(auto& /*ctx*/, InstrumentId /*id*/, const BookTickerMsg& /*m*/) noexcept {
    hit(kBookTicker);
  }
  void on_trade(auto& /*ctx*/, InstrumentId /*id*/, const TradeMsg& /*m*/) noexcept { hit(kTrade); }
  void on_option_ticker(auto& /*ctx*/, InstrumentId /*id*/, const OptionTickerMsg& /*m*/) noexcept {
    hit(kOptionTicker);
  }
  void on_fill(auto& /*ctx*/, const Fill& /*fill*/) noexcept { hit(kFill); }
  void on_order_update(auto& /*ctx*/, const OmsUpdate& /*u*/) noexcept { hit(kOrderUpdate); }
  void on_timer(auto& /*ctx*/, TimerId /*id*/, std::uint64_t tag) noexcept {
    if (tag == 7) hit(kTimer);
  }
  void on_connection(auto& /*ctx*/, const ConnectionStateMsg& /*m*/) noexcept { hit(kConnection); }
  void on_quoting(auto& /*ctx*/, bool /*enabled*/) noexcept { hit(kQuoting); }
  // [end:hooks]

  [[nodiscard]] int count(HookIndex h) const noexcept { return counts_[h]; }

 private:
  void hit(HookIndex h) noexcept { ++counts_[h]; }
  int counts_[kHookCount] = {};
};

// [start:verify]
static_assert(StrategyLike<AllHooks>);
static_assert(verify_strategy<AllHooks>());
// [end:verify]

using Harness = sim::StrategyHarness<AllHooks>;
using Ctx = StrategyContext<Harness::EngineType>;
using Book = Harness::EngineType::Book;

template <class T>
T& lvalue();

// ---- context: one static_assert per documented method -----------------------------------------

// [start:context]
// time and reference data
static_assert(std::same_as<decltype(lvalue<Ctx>().now()), Timestamp>);
static_assert(std::same_as<decltype(lvalue<Ctx>().instrument(InstrumentId{})), const Instrument&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().instruments()), const InstrumentTable&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().contains(InstrumentId{})), bool>);
// market data
static_assert(std::same_as<decltype(lvalue<Ctx>().book(InstrumentId{})), const Book&>);
// portfolio
static_assert(std::same_as<decltype(lvalue<Ctx>().position(InstrumentId{})), const Position&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().portfolio()), Portfolio>);
// quoting
static_assert(
    std::same_as<decltype(lvalue<Ctx>().set_quotes(InstrumentId{}, DesiredQuotes{})), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().pull_quotes(InstrumentId{})), void>);
static_assert(std::same_as<decltype(lvalue<Ctx>().pull_all_quotes()), void>);
static_assert(std::same_as<decltype(lvalue<Ctx>().working_quote(InstrumentId{}, Side::Buy, 0)),
                           const Order*>);
// direct orders
static_assert(std::same_as<decltype(lvalue<Ctx>().send(NewOrderRequest{})),
                           Result<ClientOrderId, RejectReason>>);
static_assert(
    std::same_as<decltype(lvalue<Ctx>().cancel(ClientOrderId{})), Result<void, RejectReason>>);
static_assert(std::same_as<decltype(lvalue<Ctx>().replace(ClientOrderId{}, Price{}, Qty{})),
                           Result<void, RejectReason>>);
static_assert(std::same_as<decltype(lvalue<Ctx>().order(ClientOrderId{})), const Order*>);
static_assert(std::same_as<decltype(lvalue<Ctx>().open_qty(InstrumentId{}, Side::Buy)), Qty>);
static_assert(std::same_as<decltype(lvalue<Ctx>().oms()), const Oms&>);
// timers
static_assert(std::same_as<decltype(lvalue<Ctx>().every(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().once(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().cancel_timer(TimerId{})), bool>);
// control
static_assert(std::same_as<decltype(lvalue<Ctx>().quoting_enabled()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().killed()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().request_stop()), void>);
// randomness (seeded from the configuration)
static_assert(std::same_as<decltype(lvalue<Ctx>().rng()), Xoshiro256ss&>);
// [end:context]

// ---- the book a hook receives -------------------------------------------------------------------

// [start:book]
static_assert(BookView<Book>);  // best_bid(), best_ask(), mid(), spread(), level(side, i), ...
static_assert(std::same_as<decltype(lvalue<const Book>().best_bid()), Level>);
static_assert(std::same_as<decltype(lvalue<const Book>().mid()), Price>);
static_assert(std::same_as<decltype(lvalue<const Book>().is_valid()), bool>);
// [end:book]

// ---- Fill and the messages hooks receive --------------------------------------------------------

// [start:fill]
static_assert(std::same_as<decltype(Fill::instrument), InstrumentId>);
static_assert(std::same_as<decltype(Fill::side), Side>);
static_assert(std::same_as<decltype(Fill::price), Price>);
static_assert(std::same_as<decltype(Fill::qty), Qty>);
static_assert(std::same_as<decltype(Fill::position_delta), Qty>);
static_assert(std::same_as<decltype(Fill::fee), Notional>);
static_assert(std::same_as<decltype(Fill::fee_converted), bool>);
static_assert(std::same_as<decltype(Fill::liquidity), Liquidity>);
static_assert(std::same_as<decltype(Fill::known), bool>);
static_assert(std::same_as<decltype(Fill::late), bool>);
static_assert(std::same_as<decltype(Fill::order_done), bool>);
static_assert(std::same_as<decltype(Fill::update), const OmsUpdate*>);
static_assert(std::same_as<decltype(Fill::msg), const OrderFillMsg*>);
// [end:fill]

// [start:messages]
static_assert(std::same_as<decltype(TradeMsg::price), Price>);
static_assert(std::same_as<decltype(TradeMsg::qty), Qty>);
static_assert(std::same_as<decltype(TradeMsg::aggressor), Side>);
static_assert(std::same_as<decltype(TradeMsg::trade_id), std::uint64_t>);
static_assert(std::same_as<decltype(BookTickerMsg::bid_px), Price>);
static_assert(std::same_as<decltype(BookTickerMsg::bid_qty), Qty>);
static_assert(std::same_as<decltype(BookTickerMsg::ask_px), Price>);
static_assert(std::same_as<decltype(BookTickerMsg::ask_qty), Qty>);
static_assert(std::same_as<decltype(OptionTickerMsg::mark_price), Price>);
static_assert(std::same_as<decltype(OptionTickerMsg::underlying_price), Price>);
static_assert(std::same_as<decltype(OptionTickerMsg::mark_iv), double>);
static_assert(std::same_as<decltype(OptionTickerMsg::delta), double>);
static_assert(std::same_as<decltype(ConnectionStateMsg::state), ConnState>);
static_assert(std::same_as<decltype(ConnectionStateMsg::channel), std::uint8_t>);
static_assert(std::same_as<decltype(EventHeader::instrument), InstrumentId>);
static_assert(std::same_as<decltype(EventHeader::venue), VenueId>);
static_assert(std::same_as<decltype(EventHeader::recv_ts), Timestamp>);
static_assert(std::same_as<decltype(EventHeader::exch_ts), Timestamp>);
static_assert(std::same_as<decltype(OmsUpdate::order), Order>);
static_assert(std::same_as<decltype(OmsUpdate::prev), OrderState>);
static_assert(std::same_as<decltype(OmsUpdate::terminal), bool>);
// [end:messages]

// ---- fixed-point helpers ------------------------------------------------------------------------

// [start:helpers]
static_assert(100.25_px == Price::from_raw(10'025'000'000));
static_assert(0.01_qty == Qty::from_raw(1'000'000));
static_assert(5_bps == Ratio::from_raw(50'000));  // 1 bp = 10'000 raw, 1.0 = 1e8 raw
static_assert(100_px * 5_bps == 0.05_px);         // Price * Ratio, one truncation toward zero
static_assert(ratio(1_px, 4_px) == Ratio::from_raw(25'000'000));
static_assert(mid(100.00_px, 100.02_px) == 100.01_px);
static_assert(microprice(Level{100.00_px, 3_qty}, Level{100.04_px, 1_qty}) == 100.03_px);
static_assert(spread_ratio(99_px, 101_px) == 200_bps);
static_assert(away_from(100_px, Side::Buy, 0.5_px) == 99.5_px);
static_assert(inventory_allows(Side::Buy, 0.003_qty, 0.001_qty, 0.004_qty));
static_assert(!inventory_allows(Side::Buy, 0.004_qty, 0.001_qty, 0.004_qty));
static_assert(inventory_allows(Side::Sell, 5_qty, 1_qty, Qty{}));  // a zero limit is no limit
static_assert(std::same_as<decltype(lvalue<const Instrument>().ticks(3)), Price>);
static_assert(std::same_as<decltype(lvalue<DesiredQuotes>().bid(Price{}, Qty{})), bool>);
static_assert(std::same_as<decltype(lvalue<DesiredQuotes>().uncross(Price{})), void>);
static_assert(
    std::same_as<decltype(keep_passive(lvalue<DesiredQuotes>(), Price{}, Price{}, Price{})), void>);
static_assert(
    std::same_as<decltype(NewOrderRequest::limit(InstrumentId{}, Side::Buy, Price{}, Qty{})
                              .post_only()
                              .reduce_only()
                              .ioc()
                              .tag(7)),
                 LimitOrder>);
static_assert(std::convertible_to<LimitOrder, NewOrderRequest>);
// [end:helpers]

// ---- registration -------------------------------------------------------------------------------

// [start:registration]
void register_strategies(StrategyRegistry& r) {
  register_strategy<AllHooks>(r);  // Sim + Replay + Live
}
static_assert(std::same_as<decltype(&register_strategies), StrategyModule>);
// [end:registration]

}  // namespace docs

TEST_CASE("docs.strategy_api: every documented hook fires in the harness") {
  using docs::AllHooks;
  // [start:harness]
  sim::StrategyHarness<AllHooks> h({{"quote_qty", "0.001"}, {"timer_ms", "5"}});  // on_start
  h.book("100.00", "100.02");   // on_book; quotes go out
  h.advance(milliseconds(10));  // acks (on_order_update), the timer (on_timer)
  REQUIRE(h.working_orders().size() == 2);
  REQUIRE(h.fill(Side::Buy));              // on_fill, then on_order_update
  h.trade(100.02_px, 0.5_qty, Side::Buy);  // on_trade
  BookTickerMsg ticker{};
  init_header(ticker, EventType::BookTicker, h.instrument(), VenueId{0});
  ticker.bid_px = 100.00_px;
  ticker.ask_px = 100.02_px;
  h.push(ticker.hdr);  // on_book_ticker
  OptionTickerMsg option{};
  init_header(option, EventType::OptionTicker, h.instrument(), VenueId{0});
  h.push(option.hdr);   // on_option_ticker
  h.disconnect();       // on_connection (market data lost)
  h.reconnect();        // on_connection (live again)
  h.pull_quotes();      // on_quoting(false)
  h.resume_quotes();    // on_quoting(true)
  h.engine().finish();  // on_stop
  // [end:harness]

  const AllHooks& s = h.strategy();
  CHECK(s.count(docs::kStart) == 1);
  CHECK(s.count(docs::kStop) == 1);
  CHECK(s.count(docs::kBook) >= 1);
  CHECK(s.count(docs::kBookTicker) == 1);
  CHECK(s.count(docs::kTrade) == 1);
  CHECK(s.count(docs::kOptionTicker) == 1);
  CHECK(s.count(docs::kFill) == 1);
  CHECK(s.count(docs::kOrderUpdate) >= 3);  // two acks and the fill
  CHECK(s.count(docs::kTimer) >= 1);
  CHECK(s.count(docs::kConnection) == 2);
  CHECK(s.count(docs::kQuoting) == 2);
  for (int i = 0; i < docs::kHookCount; ++i) {
    INFO("hook index " << i);
    CHECK(s.count(static_cast<docs::HookIndex>(i)) > 0);
  }
}

TEST_CASE("docs.strategy_api: parameters parse, validate and register") {
  docs::AllHooks s;
  CHECK_FALSE(s.configure({{"levels", "3"},
                           {"gamma", "0.5"},
                           {"hedge", "yes"},
                           {"quote_qty", "2e-05"},
                           {"half_spread_bps", "0.25"},
                           {"timer_ms", "250"}})
                  .has_value());
  CHECK(s.params().quote_qty == 0.00002_qty);
  CHECK(s.params().half_spread_bps == 0.25_bps);
  CHECK(s.params().timer_ms == milliseconds(250));
  CHECK(s.configure({{"levels", "8"}}).value_or("") == "hedge supports at most 4 levels");
  CHECK(s.params().levels == 3);  // an error leaves the previous values

  StrategyRegistry r;
  docs::register_strategies(r);
  const StrategyEntry* e = r.find("doc_all_hooks");
  REQUIRE(e != nullptr);
  CHECK(e->supports(TransportKind::Sim));
  CHECK(e->supports(TransportKind::Replay));
  CHECK(e->supports(TransportKind::Live));
}
