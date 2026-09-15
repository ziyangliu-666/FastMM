// HotStrategy (ADR-0013, section 1) with hooks written in C against hot_abi.h: level conversion,
// the parameter copy, State, warm-up and the failure path.
#include "fastmm/strategies/hot_strategy.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/testing/strategy_harness.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace fastmm;

namespace {

using Harness = sim::StrategyHarness<HotStrategy>;

int g_calls = 0;
double g_bid = 0.0;  // the float bid the hook quotes

// Record: a double parameter (8 bytes, the parameter block) and an int64 State counter.
HotProgram program(fastmm_hot_fn book_hook) {
  HotProgram p;
  p.hooks[static_cast<std::size_t>(HotHook::Book)] = book_hook;
  p.record.assign(16, 0);
  const double param = 1.5;
  std::memcpy(p.record.data(), &param, sizeof param);
  p.param_bytes = 8;
  return p;
}

std::int32_t float_levels(std::uint8_t*, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  ++g_calls;
  c->bid_px[0] = g_bid;
  c->bid_qty[0] = 0.0019999;
  c->ask_px[0] = b->best_ask + 0.004;
  c->ask_qty[0] = 0.002;
  c->n_bids = 1;
  c->n_asks = 1;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  return FASTMM_HOT_OK;
}

// A crossing raw ladder: level 0 bid above the best ask, then keep_passive shifts it back.
std::int32_t raw_crossing(std::uint8_t*, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  c->bid_px_raw[0] = b->best_ask_raw + 2 * c->tick_raw;
  c->bid_px_raw[1] = b->best_ask_raw;
  c->bid_qty_raw[0] = c->bid_qty_raw[1] = 100'000;
  c->bid_is_raw[0] = c->bid_is_raw[1] = 1;
  c->n_bids = 2;
  c->flags = FASTMM_HOT_FLAG_KEEP_PASSIVE;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  return FASTMM_HOT_OK;
}

std::int32_t self_record(std::uint8_t* self, fastmm_hot_ctx*, fastmm_hot_book*) {
  ++g_calls;
  double param = 0.0;
  std::int64_t count = 0;
  std::memcpy(&param, self, sizeof param);
  std::memcpy(&count, self + 8, sizeof count);
  if (param != 1.5) return FASTMM_HOT_FAILED;  // an earlier write to the parameter persisted
  param = 42.0;
  ++count;
  std::memcpy(self, &param, sizeof param);
  std::memcpy(self + 8, &count, sizeof count);
  return FASTMM_HOT_OK;
}

// Quotes on the first call and raises on the second.
std::int32_t fails_second(std::uint8_t*, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  if (++g_calls >= 2 && c->now_ns != 0) return FASTMM_HOT_EXCEPTION;
  c->bid_px[0] = b->best_bid;
  c->bid_qty[0] = 0.002;
  c->ask_px[0] = b->best_ask;
  c->ask_qty[0] = 0.002;
  c->n_bids = c->n_asks = 1;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  return FASTMM_HOT_OK;
}

std::int32_t not_finite(std::uint8_t*, fastmm_hot_ctx* c, fastmm_hot_book*) {
  c->bid_px[0] = 1.0 / 0.0;
  c->bid_qty[0] = 0.002;
  c->n_bids = 1;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  return FASTMM_HOT_OK;
}

fastmm_hot_book g_book{};

std::int32_t capture_book(std::uint8_t*, fastmm_hot_ctx*, fastmm_hot_book* b) {
  g_book = *b;
  return FASTMM_HOT_OK;
}

std::int64_t state_of(const HotStrategy& s) {
  std::int64_t count = 0;
  std::memcpy(&count, s.record(InstrumentId{0}) + 8, sizeof count);
  return count;
}

}  // namespace

TEST_CASE("hot strategy: float levels go to the nearest 1e-8, then passively to the tick and lot") {
  Harness h;
  REQUIRE(h.strategy().attach(program(&float_levels), h.engine().instruments()));
  // The largest double below 99.95: 99.95 after the 1e-8 step, 99.94 if it were floored directly.
  g_bid = std::nextafter(99.95, 0.0);
  REQUIRE(g_bid < 99.95);
  h.book("100.00", "100.02");
  h.advance(milliseconds(1));
  const auto orders = h.working_orders();
  REQUIRE(orders.size() == 2);
  for (const Order& o : orders) {
    if (o.side == Side::Buy) {
      CHECK(o.price == *Price::from_decimal("99.95"));
      CHECK(o.qty == *Qty::from_decimal("0.001"));
    } else {
      CHECK(o.price == *Price::from_decimal("100.03"));  // 100.024 rounds up
      CHECK(o.qty == *Qty::from_decimal("0.002"));
    }
  }
}

TEST_CASE("hot strategy: raw levels with keep_passive shift the ladder inside the touch") {
  Harness h;
  REQUIRE(h.strategy().attach(program(&raw_crossing), h.engine().instruments()));
  h.book("100.00", "100.02");
  h.advance(milliseconds(1));
  const auto orders = h.working_orders();
  REQUIRE(orders.size() == 2);
  Price top{};
  Price second{};
  for (const Order& o : orders) {
    if (o.price > top) {
      second = top;
      top = o.price;
    } else {
      second = o.price;
    }
  }
  CHECK(top == *Price::from_decimal("100.01"));  // one tick inside the best ask
  CHECK(second == *Price::from_decimal("99.99"));
}

TEST_CASE(
    "hot strategy: parameters are copied before every call, State persists, warm-up is scratch") {
  g_calls = 0;
  Harness h;
  HotStrategy& s = h.strategy();
  REQUIRE(s.attach(program(&self_record), h.engine().instruments()));
  s.warm_up(h.engine().instruments());
  CHECK(g_calls == 1);
  CHECK(state_of(s) == 0);
  h.book("100.00", "100.02");
  h.book("100.01", "100.03");
  h.book("100.02", "100.04");
  CHECK(g_calls == 4);
  CHECK(state_of(s) == 3);
  CHECK_FALSE(s.failed());
  CHECK(s.calls() == 3);
}

TEST_CASE(
    "hot strategy: a failing hook trips the kill switch with StrategyError and stops calling") {
  g_calls = 0;
  Harness h;
  HotStrategy& s = h.strategy();
  REQUIRE(s.attach(program(&fails_second), h.engine().instruments()));
  h.book("100.00", "100.02");
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders().size() == 2);
  h.book("100.01", "100.03");
  CHECK(s.failed());
  CHECK(s.error().status == FASTMM_HOT_EXCEPTION);
  CHECK(s.error().hook == static_cast<std::int32_t>(HotHook::Book));
  CHECK(h.engine().kill_reason() == KillReason::StrategyError);
  CHECK(to_string(KillReason::StrategyError) == "strategy_error");
  h.advance(milliseconds(1));
  CHECK(h.working_orders().empty());
  h.book("100.02", "100.04");
  CHECK(g_calls == 2);
}

TEST_CASE("hot strategy: a float level that is not finite is a failure") {
  Harness h;
  HotStrategy& s = h.strategy();
  REQUIRE(s.attach(program(&not_finite), h.engine().instruments()));
  h.book("100.00", "100.02");
  CHECK(s.error().status == FASTMM_HOT_BAD_VALUE);
  CHECK(h.engine().kill_reason() == KillReason::StrategyError);
}

TEST_CASE("hot strategy: attach rejects a record smaller than its parameter block") {
  Harness h;
  HotProgram p = program(&self_record);
  p.param_bytes = 32;
  CHECK_FALSE(h.strategy().attach(p, h.engine().instruments()));
}

TEST_CASE("hot strategy: the level arrays are copied only with book_depth") {
  for (const bool depth : {true, false}) {
    CAPTURE(depth);
    Harness h;
    HotProgram p = program(&capture_book);
    p.book_depth = depth;
    REQUIRE(h.strategy().attach(p, h.engine().instruments()));
    h.book("100.00", "100.02");
    CHECK(g_book.valid == 1);
    CHECK(g_book.n_bids == 1);
    CHECK(g_book.n_asks == 1);
    CHECK(g_book.best_bid_raw == Price::from_decimal("100.00")->raw);
    CHECK(g_book.bid_px_raw[0] == (depth ? g_book.best_bid_raw : 0));
    CHECK(g_book.ask_px[0] == (depth ? 100.02 : 0.0));
  }
}
