// RestingOrders: the gateway's self-trade check between strategies on one shared instrument.
#include "fastmm/core/self_trade.hpp"

#include "test_support.hpp"

using namespace fastmm;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
constexpr ClientOrderId id(std::uint16_t epoch, std::uint32_t seq) {
  return make_cl_ord_id(epoch, seq);
}
}  // namespace

TEST_CASE("core.self_trade: an order crosses another epoch's resting order, never its own") {
  RestingOrders r;
  r.set(id(1, 1), Side::Buy, px("99"), 1);
  r.set(id(1, 2), Side::Sell, px("101"), 1);
  // Epoch 1's own orders are not the gateway's concern.
  CHECK_FALSE(r.crosses(1, Side::Buy, px("102"), false));
  CHECK_FALSE(r.crosses(1, Side::Sell, px("98"), true));
  // Epoch 2: through the ask or the bid, at it, or a market order crosses.
  CHECK(r.crosses(2, Side::Buy, px("101"), false));
  CHECK(r.crosses(2, Side::Buy, px("150"), false));
  CHECK(r.crosses(2, Side::Sell, px("99"), false));
  CHECK(r.crosses(2, Side::Sell, Price{}, true));
  // Inside the spread it does not.
  CHECK_FALSE(r.crosses(2, Side::Buy, px("100.99"), false));
  CHECK_FALSE(r.crosses(2, Side::Sell, px("99.01"), false));
}

TEST_CASE("core.self_trade: an entry moves when set again and goes when removed") {
  RestingOrders r;
  r.reserve(4);
  r.set(id(1, 1), Side::Sell, px("101"), 1);
  r.set(id(1, 1), Side::Sell, px("105"), 1);  // a replace's new price under the same id
  CHECK(r.size(Side::Sell) == 1);
  CHECK_FALSE(r.crosses(2, Side::Buy, px("104"), false));
  CHECK(r.crosses(2, Side::Buy, px("105"), false));
  r.set(id(3, 1), Side::Sell, px("103"), 3);
  CHECK(r.crosses(2, Side::Buy, px("104"), false));
  r.remove(id(3, 1), Side::Sell);
  r.remove(id(9, 9), Side::Sell);  // not there: nothing happens
  CHECK(r.size(Side::Sell) == 1);
  CHECK_FALSE(r.crosses(2, Side::Buy, px("104"), false));
  r.remove(id(1, 1), Side::Sell);
  CHECK(r.size(Side::Sell) == 0);
  CHECK_FALSE(r.crosses(2, Side::Buy, Price{}, true));
}
