// Deribit testnet (opt-in: FASTMM_LIVE_TESTS=1). Public part, no keys: reference data for a live
// BTC option and BTC-PERPETUAL, both books synchronised and an OptionTicker received. With
// FASTMM_DERIBIT_CLIENT_ID / FASTMM_DERIBIT_CLIENT_SECRET (testnet API key): a post-only buy at the
// minimum price is placed, edited, cancelled, then cancel_all runs.
// Endpoints: https://docs.deribit.com/articles/json-rpc-overview (test.deribit.com only: the test
// refuses any other host so production can never be reached from here).
#include "live_test_util.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/deribit/deribit_rest_decoder.hpp"
#include "fastmm/venues/deribit/deribit_venue.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
using namespace fastmm::venues::test;

namespace {
constexpr const char* kWs = "wss://test.deribit.com/ws/api/v2";
constexpr const char* kRest = "https://test.deribit.com/api/v2";

// Option names expire daily: pick a call about a week or more out with a middle strike.
std::string pick_option() {
  BlockingHttp http(kRest);
  const HttpReply r = http.get("/public/get_instruments?currency=BTC&kind=option&expired=false");
  REQUIRE_MESSAGE(r.ok(), r.error << " " << r.status);
  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(r.body, infos).empty());
  const std::int64_t min_expiry_ms = wall_now().ns / 1'000'000 + 6LL * 86'400'000;
  std::int64_t expiry = 0;
  for (const InstrumentInfo& i : infos) {
    if (i.option_type == "call" && i.expiration_ms >= min_expiry_ms &&
        i.name.size() <= Symbol::kCapacity && (expiry == 0 || i.expiration_ms < expiry))
      expiry = i.expiration_ms;
  }
  REQUIRE(expiry != 0);
  std::vector<const InstrumentInfo*> calls;
  for (const InstrumentInfo& i : infos) {
    if (i.option_type == "call" && i.expiration_ms == expiry && i.name.size() <= Symbol::kCapacity)
      calls.push_back(&i);
  }
  std::sort(calls.begin(), calls.end(), [](auto* a, auto* b) { return a->strike < b->strike; });
  return calls[calls.size() / 2]->name;
}
}  // namespace

TEST_CASE("live.deribit: testnet option book and ticker, then place, edit and cancel") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Deribit testnet");
    return;
  }
  REQUIRE(std::string_view(kWs).find("://test.deribit.com/") != std::string_view::npos);
  REQUIRE(std::string_view(kRest).find("://test.deribit.com/") != std::string_view::npos);
  const std::string client_id = env_or_empty("FASTMM_DERIBIT_CLIENT_ID");
  const std::string secret = env_or_empty("FASTMM_DERIBIT_CLIENT_SECRET");
  const bool with_keys = !client_id.empty() && !secret.empty();
  const std::string option = pick_option();
  MESSAGE("option under test: " << option);

  VenueSection s;
  s.name = "deribit-testnet";
  s.kind = "deribit";
  s.ws_url = kWs;
  s.rest_url = kRest;
  s.api_key = client_id;
  s.api_secret = secret;
  s.supports_replace = true;
  DeribitVenueConfig cfg = make_deribit_config(s, !with_keys);
  cfg.cancel_on_order_channel_loss = false;

  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument(option.c_str(), 0, "BTC", "BTC")));
  REQUIRE(instruments.add(make_instrument("BTC-PERPETUAL", 0, "BTC", "USD")));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  DeribitVenue venue(VenueId{0}, cfg);
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  CHECK(instruments.get(InstrumentId{0}).asset_class == AssetClass::Option);
  CHECK(instruments.get(InstrumentId{1}).contract_multiplier == Qty::from_int(10));
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
  venue.subscribe(ids);
  venue.connect(reactor);

  Collected mdc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        mdc.take(md);
        return venue.md_feed()->synced_count() == 2 && mdc.count(EventType::OptionTicker) > 0;
      },
      60'000));
  const auto* ot = mdc.last<OptionTickerMsg>(EventType::OptionTicker);
  CHECK(ot->underlying_price.is_positive());
  CHECK(ot->mark_iv > 0.0);
  CHECK(ot->mark_iv < 10.0);
  CHECK(ot->delta >= -1.0);
  CHECK(ot->delta <= 1.0);

  if (!with_keys) {
    MESSAGE("skipped order entry: FASTMM_DERIBIT_CLIENT_ID / FASTMM_DERIBIT_CLIENT_SECRET not set");
    venue.disconnect();
    return;
  }
  Collected oc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        for (const auto& m : oc.all) {
          if (RecordingSink::type_of(m) == EventType::ConnectionState &&
              RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
            return true;
        }
        return false;
      },
      30'000));
  const Instrument& inst = instruments.get(InstrumentId{0});
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, inst.id, inst.venue);
  n.cl_ord_id = make_cl_ord_id(static_cast<std::uint16_t>(wall_now().ns % 60000), 1);
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = inst.tick;  // the minimum price: far below any live bid
  n.qty = inst.lot;
  REQUIRE(outbound.try_push(&n, n.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return oc.count(EventType::OrderAck) > 0 || oc.count(EventType::OrderReject) > 0;
      },
      15'000));
  REQUIRE_MESSAGE(oc.count(EventType::OrderReject) == 0,
                  (oc.count(EventType::OrderReject)
                       ? std::string(oc.last<OrderRejectMsg>(EventType::OrderReject)->text.view())
                       : std::string()));
  const VenueOrderId venue_id = oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id;

  OutReplaceMsg rp{};
  init_header(rp, EventType::OutReplace, inst.id, inst.venue);
  rp.cl_ord_id = make_cl_ord_id(cl_ord_id_epoch(n.cl_ord_id), 2);
  rp.orig_cl_ord_id = n.cl_ord_id;
  rp.venue_order_id = venue_id;
  rp.price = inst.tick + inst.tick;
  rp.qty = inst.lot;
  REQUIRE(outbound.try_push(&rp, rp.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        const auto* a = oc.last<OrderAckMsg>(EventType::OrderAck);
        return (a != nullptr && a->cl_ord_id == rp.cl_ord_id) ||
               oc.count(EventType::OrderReject) > 0;
      },
      15'000));
  CHECK(oc.count(EventType::OrderReject) == 0);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, inst.id, inst.venue);
  c.cl_ord_id = rp.cl_ord_id;
  c.venue_order_id = venue_id;
  REQUIRE(outbound.try_push(&c, c.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return oc.count(EventType::OrderCancelAck) > 0 ||
               oc.count(EventType::OrderCancelReject) > 0;
      },
      15'000));
  CHECK(oc.count(EventType::OrderCancelAck) > 0);
  CHECK(venue.cancel_all());
  CHECK_FALSE(venue.fatal());
  venue.disconnect();
}
