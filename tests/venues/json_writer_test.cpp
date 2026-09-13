#include "fastmm/venues/json_writer.hpp"

#include "test_support.hpp"

#include "fastmm/venues/order_commands.hpp"

using namespace fastmm;
using namespace fastmm::venues;

TEST_CASE("venues.json_writer: nested objects, arrays, escaping, overflow") {
  char buf[256];
  JsonWriter w(buf);
  w.begin_object()
      .key("id")
      .string("a\"b\\c\n")
      .key("n")
      .integer(-42)
      .key("ok")
      .boolean(true)
      .key("arr")
      .begin_array()
      .integer(1)
      .string("x")
      .begin_object()
      .key("k")
      .null()
      .end_object()
      .end_array()
      .key("params")
      .begin_object()
      .end_object()
      .end_object();
  CHECK(w.ok());
  CHECK(w.view() == R"({"id":"a\"b\\c\n","n":-42,"ok":true,"arr":[1,"x",{"k":null}],"params":{}})");
  char small[8];
  JsonWriter s(small);
  s.begin_object().key("abc").string("defghij").end_object();
  CHECK_FALSE(s.ok());
}

TEST_CASE("venues.order_commands: view over Out*Msg") {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{3}, VenueId{1});
  n.cl_ord_id = ClientOrderId{0x1'0000'0001ULL};
  n.side = Side::Sell;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_int(100);
  n.qty = Qty::from_int(2);
  auto c = OrderCommand::from(n.hdr);
  REQUIRE(c.has_value());
  CHECK(c->kind == OrderCommandKind::New);
  CHECK(c->instrument == InstrumentId{3});
  CHECK(c->venue == VenueId{1});
  CHECK(c->side == Side::Sell);
  CHECK(c->type == OrderType::PostOnly);
  CHECK(c->price == Price::from_int(100));
  CHECK(c->qty == Qty::from_int(2));

  OutCancelMsg x{};
  init_header(x, EventType::OutCancel, InstrumentId{3}, VenueId{1});
  x.cl_ord_id = n.cl_ord_id;
  x.venue_order_id = "12345";
  auto cc = OrderCommand::from(x.hdr);
  REQUIRE(cc.has_value());
  CHECK(cc->kind == OrderCommandKind::Cancel);
  CHECK(cc->venue_order_id->view() == "12345");

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{3}, VenueId{1});
  r.cl_ord_id = ClientOrderId{0x1'0000'0002ULL};
  r.orig_cl_ord_id = n.cl_ord_id;
  r.price = Price::from_int(101);
  r.qty = Qty::from_int(1);
  auto rc = OrderCommand::from(r.hdr);
  REQUIRE(rc.has_value());
  CHECK(rc->kind == OrderCommandKind::Replace);
  CHECK(rc->orig_cl_ord_id == n.cl_ord_id);
  CHECK(rc->price == Price::from_int(101));

  TradeMsg t{};
  init_header(t, EventType::Trade);
  CHECK_FALSE(OrderCommand::from(t.hdr).has_value());
}
