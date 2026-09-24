// Recovery, proved rather than asserted: a session is broken in flight and has to come back to
// correct service. Every case ends on the same invariants - the engine's position, open orders and
// PnL agree with the simulator's, no fill is lost, no client order id is used twice, nothing is
// left resting - and each one breaks the session differently:
//
//   1 market data      the stream is cut, and cut with a sequence gap: the book is unusable, the
//                      quotes come off the venue, and both come back
//   2 order channel    cut with orders resting, with an order in flight and with a cancel in
//                      flight; reconciliation must leave the two sides holding the same orders
//   3 fills in the dark  the simulator fills a resting order while the private stream is muted;
//                      the cum_qty jump has to become a synthetic fill (no fee: see NOTES.md)
//   4 kill -9          a child fastmm-live is killed with orders resting and restarted
//   5 uncertain        a lost response, a response after the engine gave up, a duplicated event,
//                      a cancel that races a fill, a replace after the original filled
//   6 venue chaos      429, a 418 hard stop, a revoked key, malformed frames, clock skew
//
// Cases 1-3, 5 and 6 drive BinanceVenue on this thread's reactor (VenueHarness) so every step is
// synchronised on an observable fact rather than a sleep, with OmsMirror standing in for the
// engine's order state; 4 runs whole processes.
#include "integration_util.hpp"

#include "fastmm/core/oms.hpp"
#include "fastmm/core/position.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;

namespace {

const Qty kLot = Qty::from_decimal("0.001").value();

// Reconciliation snapshots the connector has finished emitting.
std::size_t reconcile_ends(const VenueHarness& h) {
  return h.oc.count_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& r) {
    return r.kind == ReconcileMsg::Kind::End;
  });
}

// No market orders from the counter-party flow: nothing sweeps a resting order except the test, so
// a scenario that puts an order on the book and breaks something decides by itself when that order
// trades. Limit and cancel flow still run, so the book moves and depth updates keep coming.
sim::server::SimServerConfig quiet_server() {
  sim::server::SimServerConfig c = test_server_config();
  c.generator.market_rate_per_s = 0.0;
  return c;
}

// A connected harness with reference data loaded, market data synced, both WS API channels up and
// the connector's start-up reconciliation done, so a scenario counts snapshots from a known state.
struct ReadyHarness : VenueHarness {
  std::size_t reconciles = 0;

  explicit ReadyHarness(const ServerFixture& fx) : VenueHarness(venue_config(fx)) {
    REQUIRE(venue->load_reference_data(instruments));
    connect();
    REQUIRE(pump([&] {
      return venue->md_feed()->synced_count() == 1 && live_order_channels() >= 2 &&
             reconcile_ends(*this) >= 1;
    }));
    reconciles = reconcile_ends(*this);
  }
};

// A bid far enough behind the touch that the generator's flow will not reach it: an order resting
// there moves only when the test moves it.
Price resting_bid(const ServerFixture& fx, int ticks) {
  const Price tick = Price::from_decimal("0.01").value();
  return Price::from_raw(fx.server.stats().best_bid.price.raw - (tick.raw * ticks));
}

// Places one post-only order and waits for the venue and the mirror to agree it is open.
void place_resting(ReadyHarness& h, OmsMirror& m, ClientOrderId id, Price px, Qty qty) {
  const OutNewOrderMsg o = new_order(id, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, px, qty);
  m.submit(o);
  h.send(o.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.acks(id) >= 2;  // the WS API response and the executionReport NEW
  }));
}

// Waits for the next reconciliation snapshot to be applied to the mirror.
void await_reconcile(ReadyHarness& h, OmsMirror& m) {
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return reconcile_ends(h) > h.reconciles;
  }));
  m.drain(h.oc);
  h.reconciles = reconcile_ends(h);
}

// The invariant every scenario ends on: the two sides hold the same orders.
void check_orders_agree(const ServerFixture& fx, OmsMirror& m) {
  std::vector<std::string> venue_ids = fx.server.open_client_order_ids();
  std::vector<std::string> engine_ids;
  for (const ClientOrderId id : m.open_ids()) engine_ids.emplace_back(encode_cl_ord_id(id).view());
  std::sort(venue_ids.begin(), venue_ids.end());
  std::sort(engine_ids.begin(), engine_ids.end());
  INFO("venue holds " << venue_ids.size() << ", engine believes " << engine_ids.size());
  CHECK(venue_ids == engine_ids);
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
}

}  // namespace

// ---- 1. market data ---------------------------------------------------------------------------

TEST_CASE("recovery: a market-data cut and a sequence gap pull the quotes and the book recovers") {
  ServerFixture fx;
  LiveEngine live(sim_local_config(fx, false));
  live.start();
  auto status = [&] { return live.venue().status(); };
  INFO(describe(status(), fx.server.stats()));
  REQUIRE(wait_until(
      [&] { return status().books_synced == 1 && fx.server.stats().open_orders >= 1; }, 20000));

  // (a) The connection is cut. The engine clears the book on the Disconnected state, so no quote
  //     may rest at the venue until the stream is back and re-snapshotted.
  // The status is republished once a second, so "synced" alone can still be the state from before
  // the cut: wait for the new session's snapshot too.
  const std::uint64_t snapshots_at_cut = fx.server.stats().depth_snapshots;
  fx.server.mark();
  fx.server.drop_market_data_connections();
  REQUIRE(wait_until(
      [&] {
        const sim::server::SimServerStats x = fx.server.stats();
        return x.md_sessions_opened_since_mark >= 1 && x.md_sessions == 1 &&
               x.depth_snapshots > snapshots_at_cut;
      },
      20000));
  REQUIRE(wait_until(
      [&] {
        const VenueStatus v = status();
        return v.md == ChannelState::Live && v.books_synced == 1;
      },
      20000));
  CHECK(fx.server.stats().min_open_orders_since_mark == 0);  // the quotes came off
  REQUIRE(wait_until([&] { return fx.server.stats().orders_since_mark >= 1; }, 20000));

  // (b) The stream stays up but one depthUpdate is dropped: U != prev_u + 1 is a resync, which is
  //     the same "book unusable" state reached without losing the connection. The snapshot comes
  //     first, then the connector's status carries the resync (it republishes once a second).
  const std::uint64_t resyncs_before = status().resyncs;
  const std::uint64_t snapshots_before = fx.server.stats().depth_snapshots;
  fx.server.mark();
  fx.server.skip_next_depth_update();
  REQUIRE(wait_until([&] { return fx.server.stats().depth_snapshots > snapshots_before; }, 20000));
  REQUIRE(wait_until(
      [&] {
        const VenueStatus v = status();
        return v.resyncs > resyncs_before && v.books_synced == 1;
      },
      20000));
  REQUIRE(wait_until([&] { return fx.server.stats().orders_since_mark >= 1; }, 20000));
  CHECK(fx.server.stats().depth_updates_skipped == 1);

  live.stop();
  CHECK(live.cancel_all_ok());
  REQUIRE(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  const auto& e = live.engine();
  CHECK(e.position(InstrumentId{0}).qty == ss.position);
  CHECK(e.oms().open_count() == 0);
  CHECK(fx.server.open_client_order_ids().empty());
  CHECK(ss.duplicate_client_order_ids == 0);
  CHECK(ss.max_abs_position <= e.risk().limits().max_position);
  CHECK(ss.max_order_qty <= e.risk().limits().max_order_qty);
  CHECK(ss.max_open_orders <= e.risk().limits().max_open_orders);
}

// ---- 2. order and user stream -----------------------------------------------------------------

TEST_CASE(
    "recovery: the order channel is cut with an order resting, in flight and being cancelled") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  SUBCASE("an order is resting") {
    place_resting(h, m, cid(1), resting_bid(fx, 100), kLot);
    REQUIRE(fx.server.stats().open_orders == 1);

    // Both WS API connections go: the order channel and the user stream with it.
    fx.server.mark();
    fx.server.drop_ws_api_connections(true);
    // The connector cancels everything over REST on order-channel loss, reconnects and asks for
    // the open orders. Wait for the reconciliation, not for a clock.
    await_reconcile(h, m);
    CHECK(fx.server.stats().cancel_all_since_mark >= 1);
    CHECK(m.unknown_orders() == 0);  // nothing at the venue the engine did not know
    check_orders_agree(fx, m);
    CHECK(m.oms().open_count() == 0);  // cancelled over REST, and the engine knows
    CHECK(m.position() == fx.server.stats().position);
  }

  SUBCASE("an order is in flight") {
    // The response is held back for longer than the outage, so the order is unacknowledged when
    // the connection dies: the engine cannot know whether the venue took it.
    fx.server.set_ack_delay_ms(2000);
    const OutNewOrderMsg o = new_order(
        cid(2), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 120), kLot);
    m.submit(o);
    h.send(o.hdr);
    REQUIRE(h.pump([&] { return fx.server.stats().orders_accepted >= 1; }));
    CHECK(h.acks(cid(2)) == 0);  // accepted at the venue, not yet acknowledged to us
    fx.server.drop_ws_api_connections(true);
    fx.server.set_ack_delay_ms(0);

    await_reconcile(h, m);
    // The venue took the order and the REST cancel-all removed it again; the engine never saw
    // either. Whatever the sequence was, the two sides end holding the same orders and the engine
    // is left with nothing it believes is working.
    check_orders_agree(fx, m);
    CHECK(m.oms().open_count() == 0);
    CHECK(m.position() == fx.server.stats().position);
  }

  SUBCASE("a cancel is in flight") {
    const Price px = resting_bid(fx, 140);
    place_resting(h, m, cid(3), px, kLot);
    fx.server.set_ack_delay_ms(2000);
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    c.cl_ord_id = cid(3);
    c.venue_order_id = h.last_ack(cid(3))->venue_order_id;
    m.request_cancel(cid(3));
    h.send(c.hdr);
    REQUIRE(h.pump([&] { return fx.server.stats().cancels >= 1; }));
    fx.server.drop_ws_api_connections(true);
    fx.server.set_ack_delay_ms(0);

    await_reconcile(h, m);
    check_orders_agree(fx, m);
    CHECK(m.position() == fx.server.stats().position);
  }

  CHECK(h.venue->cancel_all());
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return fx.server.stats().open_orders == 0;
  }));
  m.drain(h.oc);
  CHECK_FALSE(h.venue->fatal());
}

// ---- 3. fills nobody was listening for --------------------------------------------------------

TEST_CASE("recovery: a fill while the private stream is down is booked from the venue's trades") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);
  const Qty two = Qty::from_raw(kLot.raw * 2);
  const Price px = resting_bid(fx, 160);
  place_resting(h, m, cid(1), px, two);

  // The stream goes quiet, and half the order trades while nobody is listening.
  fx.server.set_user_stream_muted(true);
  const std::string wire = std::string(encode_cl_ord_id(cid(1)).view());
  CHECK(fx.server.fill_open_order(wire, kLot) == kLot);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return fx.server.stats().user_events_dropped >= 1;
  }));
  CHECK(m.position().is_zero());  // the engine has heard nothing
  fx.server.set_user_stream_muted(false);

  // The reconciliation asks the venue what it filled before it asks what is open, so the execution
  // arrives as an ordinary fill with its real price and its real fee. Nothing is estimated: the
  // snapshot's executedQty is already covered by the time it is read.
  h.venue->request_open_orders();
  await_reconcile(h, m);
  CHECK(m.replayed_fills() == 1);
  CHECK(m.synthetic_fills() == 0);
  CHECK(m.exact_reconciles() >= 1);
  CHECK(m.position() == kLot);
  CHECK(m.position() == fx.server.stats().position);
  CHECK(m.fees() == fx.server.stats().fees);
  check_orders_agree(fx, m);

  // The rest of the order fills, still in the dark. The venue now has nothing open to report - the
  // order is gone from the snapshot entirely - so only its trade history can say what happened.
  fx.server.set_user_stream_muted(true);
  CHECK(fx.server.fill_open_order(wire, kLot) == kLot);
  REQUIRE(h.pump([&] { return fx.server.stats().open_orders == 0; }));
  fx.server.set_user_stream_muted(false);
  h.venue->request_open_orders();
  await_reconcile(h, m);
  CHECK(m.oms().open_count() == 0);  // the order is gone from both views
  check_orders_agree(fx, m);
  CHECK(m.replayed_fills() == 2);
  CHECK(m.synthetic_fills() == 0);
  CHECK(m.oms().stats().reconcile_unresolved == 0);  // nothing left to guess at
  CHECK(fx.server.stats().position == two);
  CHECK(m.position() == two);
  CHECK(m.fees() == fx.server.stats().fees);
}

TEST_CASE(
    "recovery: a fill during an order-channel outage is booked when the cancel-all reconciles") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);
  const Qty two = Qty::from_raw(kLot.raw * 2);
  const std::string wire = std::string(encode_cl_ord_id(cid(1)).view());

  SUBCASE("the order is left part filled") {
    place_resting(h, m, cid(1), resting_bid(fx, 170), two);
    fx.server.set_user_stream_muted(true);
    CHECK(fx.server.fill_open_order(wire, kLot) == kLot);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return fx.server.stats().user_events_dropped >= 1;
    }));
    CHECK(m.position().is_zero());

    // Both WS API connections go with the fill still unheard of. The connector cancels what is left
    // over REST - so the cancel ack that would have carried executedQty is lost with the stream -
    // and reconciles when it comes back.
    fx.server.set_user_stream_muted(false);
    fx.server.drop_ws_api_connections(true);
    await_reconcile(h, m);
    CHECK(m.replayed_fills() >= 1);
    CHECK(m.position() == kLot);
    CHECK(m.position() == fx.server.stats().position);
    CHECK(m.fees() == fx.server.stats().fees);
    check_orders_agree(fx, m);
  }

  SUBCASE("the order is finished") {
    place_resting(h, m, cid(1), resting_bid(fx, 180), two);
    fx.server.set_user_stream_muted(true);
    CHECK(fx.server.fill_open_order(wire, two) == two);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return fx.server.stats().open_orders == 0;
    }));
    CHECK(m.position().is_zero());

    fx.server.set_user_stream_muted(false);
    fx.server.drop_ws_api_connections(true);
    await_reconcile(h, m);
    CHECK(m.replayed_fills() >= 1);
    CHECK(m.oms().open_count() == 0);
    CHECK(m.oms().stats().reconcile_unresolved == 0);
    CHECK(m.position() == two);
    CHECK(m.position() == fx.server.stats().position);
    CHECK(m.fees() == fx.server.stats().fees);
    check_orders_agree(fx, m);
  }
}

// A cancel ack that carries executedQty still books the missing quantity straight away - the engine
// cannot wait for a reconciliation to know its position. That booking is an estimate: the order's
// own price and no fee. The next reconciliation replays the execution that caused it, and the
// estimate must be replaced, not added to.
TEST_CASE("recovery: an execution replaces the synthetic fill a cancel ack booked") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);
  const Qty two = Qty::from_raw(kLot.raw * 2);
  place_resting(h, m, cid(1), resting_bid(fx, 190), two);
  const std::string wire = std::string(encode_cl_ord_id(cid(1)).view());

  // Half trades with the stream muted; unmuting does not replay it, so the cancel ack is the first
  // message that mentions the quantity.
  fx.server.set_user_stream_muted(true);
  CHECK(fx.server.fill_open_order(wire, kLot) == kLot);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return fx.server.stats().user_events_dropped >= 1;
  }));
  fx.server.set_user_stream_muted(false);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = cid(1);
  c.venue_order_id = h.last_ack(cid(1))->venue_order_id;
  m.request_cancel(cid(1));
  h.send(c.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return m.oms().open_count() == 0;
  }));
  CHECK(m.synthetic_fills() == 1);  // booked from the cancel ack's executedQty
  CHECK(m.position() == kLot);
  CHECK(m.fees().is_zero());  // ... at the order's own price and with no fee

  // The reconciliation replays the execution. The quantity is already in the position, so what the
  // execution changes is its price and its fee.
  h.venue->request_executions();
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return m.replayed_fills() >= 1;
  }));
  CHECK(m.corrected_fills() == 1);
  CHECK(m.position() == kLot);  // not two
  CHECK(m.position() == fx.server.stats().position);
  CHECK(m.fees() == fx.server.stats().fees);
  CHECK(m.oms().stats().corrected_fills == 1);
}

// ---- 5. uncertain order outcomes --------------------------------------------------------------

TEST_CASE("recovery: an order whose response never arrives is settled by reconciliation") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  // The venue takes the order and its reply is dropped; the executionReport goes too, so nothing
  // tells the engine what happened.
  fx.server.set_user_stream_muted(true);
  fx.server.swallow_next_ws_api_responses(1);
  const OutNewOrderMsg o = new_order(
      cid(1), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 100), kLot);
  m.submit(o);
  h.send(o.hdr);
  REQUIRE(h.pump([&] { return fx.server.stats().orders_accepted >= 1; }));
  CHECK(fx.server.stats().responses_swallowed == 1);
  CHECK(h.acks(cid(1)) == 0);
  fx.server.set_user_stream_muted(false);

  h.venue->request_open_orders();
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return m.oms().open_count() == 1;
  }));
  m.drain(h.oc);
  // The snapshot lifts it out of PendingNew: one order, known to both, not duplicated.
  check_orders_agree(fx, m);
  CHECK(fx.server.stats().orders_accepted == 1);
  CHECK(h.venue->cancel_all());
}

TEST_CASE("recovery: a duplicated ack and a duplicated fill change the engine's view only once") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  // Every user event of this request is sent twice: the executionReport NEW arrives three times in
  // all (the WS API response also acks).
  fx.server.duplicate_next_user_events(4);
  const Price px = resting_bid(fx, 100);
  const OutNewOrderMsg o =
      new_order(cid(1), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, px, kLot);
  m.submit(o);
  h.send(o.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.acks(cid(1)) >= 3;
  }));
  m.drain(h.oc);
  CHECK(m.oms().open_count() == 1);
  CHECK(m.oms().stats().acked == 1);  // one transition out of PendingNew, however many acks came

  // A fill delivered twice: the exec id dedupe has to keep the position at one lot.
  fx.server.duplicate_next_user_events(4);
  CHECK(fx.server.fill_open_order(std::string(encode_cl_ord_id(cid(1)).view()), kLot) == kLot);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return !m.position().is_zero();
  }));
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return m.oms().stats().duplicates >= 1;
  }));
  m.drain(h.oc);
  CHECK(m.position() == kLot);
  CHECK(m.position() == fx.server.stats().position);
  CHECK(m.oms().stats().duplicates >= 1);
  check_orders_agree(fx, m);
}

TEST_CASE("recovery: a cancel that races a fill, and a replace after the original filled") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  SUBCASE("the cancel loses the race") {
    const Price px = resting_bid(fx, 100);
    place_resting(h, m, cid(1), px, kLot);
    // The order fills; the cancel the engine sends next can only be refused.
    CHECK(fx.server.fill_open_order(std::string(encode_cl_ord_id(cid(1)).view()), kLot) == kLot);
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    c.cl_ord_id = cid(1);
    c.venue_order_id = h.last_ack(cid(1))->venue_order_id;
    m.request_cancel(cid(1));
    h.send(c.hdr);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return m.oms().open_count() == 0;
    }));
    m.drain(h.oc);
    CHECK(m.position() == kLot);  // the fill is booked, not thrown away with the cancel
    CHECK(m.position() == fx.server.stats().position);
    check_orders_agree(fx, m);
  }

  SUBCASE("the replace loses the race") {
    const Price px = resting_bid(fx, 100);
    place_resting(h, m, cid(2), px, kLot);
    CHECK(fx.server.fill_open_order(std::string(encode_cl_ord_id(cid(2)).view()), kLot) == kLot);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return m.oms().open_count() == 0;  // the fill message ends it first
    }));
    // A replace for an order the venue has already forgotten.
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
    r.cl_ord_id = cid(3);
    r.orig_cl_ord_id = cid(2);
    r.venue_order_id = h.last_ack(cid(2))->venue_order_id;
    r.price = Price::from_raw(px.raw - Price::from_decimal("0.01").value().raw);
    r.qty = kLot;
    h.send(r.hdr);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return h.reject(cid(3)) != nullptr || h.cancel_acks(cid(2)) >= 1;
    }));
    m.drain(h.oc);
    CHECK(m.oms().open_count() == 0);
    CHECK(m.position() == fx.server.stats().position);
    check_orders_agree(fx, m);
  }
}

TEST_CASE(
    "recovery: a response that arrives after the engine gave up does not resurrect an order") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  // The engine's ack sweep force-cancels an order that never acknowledged. Here the sweep is the
  // mirror's: request_cancel_unacked, exactly what Engine::sweep_acks does.
  fx.server.set_ack_delay_ms(1500);
  const OutNewOrderMsg o = new_order(
      cid(1), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 100), kLot);
  m.submit(o);
  h.send(o.hdr);
  REQUIRE(h.pump([&] { return fx.server.stats().orders_accepted >= 1; }));
  CHECK(h.acks(cid(1)) == 0);

  // Give up on it: the engine cancels an order it has no ack for.
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = cid(1);
  h.send(c.hdr);
  fx.server.set_ack_delay_ms(0);

  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.cancel_acks(cid(1)) >= 1 && h.acks(cid(1)) >= 1;
  }));
  m.drain(h.oc);
  // The late ack arrives after the cancel: it must not put the order back on the books.
  CHECK(m.oms().open_count() == 0);
  check_orders_agree(fx, m);
  CHECK(h.venue->cancel_all());
}

// ---- 6. venue-side chaos ----------------------------------------------------------------------

TEST_CASE("recovery: a rate limit costs an order, not the engine's view of it") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  // 429 / -1003 on the order: the connector cools down and reports the order as rejected, so the
  // engine's slot is freed and nothing is left in flight.
  const std::uint64_t cooldowns = h.venue->status().rate_limit_cooldowns;
  fx.server.rate_limit_next_requests(1);
  const OutNewOrderMsg limited = new_order(
      cid(1), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 100), kLot);
  m.submit(limited);
  h.send(limited.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.reject(cid(1)) != nullptr;
  }));
  m.drain(h.oc);
  CHECK(h.reject(cid(1))->venue_code == -1003);
  // The status is republished on the connector's one-second timer.
  REQUIRE(h.pump([&] { return h.venue->status().rate_limit_cooldowns > cooldowns; }));
  CHECK(m.oms().open_count() == 0);
  CHECK(fx.server.stats().orders_accepted == 0);  // the order was never taken
  CHECK(fx.server.stats().rate_limited == 1);

  // The cost is the cooldown the connector took from Retry-After, which the venue sets from what
  // is left of its rate-limit window; how long the next order waits is the venue's business, and
  // the engine's view of the order is settled either way.
  check_orders_agree(fx, m);
  CHECK_FALSE(h.venue->fatal());
}

TEST_CASE("recovery: clock skew past recvWindow is learned and the orders flow again") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);

  // Clock skew past recvWindow: signed requests answer -1021 until the connector re-reads the
  // venue's time and stamps with it.
  const std::uint64_t times_before = fx.server.stats().time_requests;
  fx.server.set_clock_offset_ms(30'000);
  const OutNewOrderMsg o = new_order(
      cid(1), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 100), kLot);
  m.submit(o);
  h.send(o.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.reject(cid(1)) != nullptr;
  }));
  CHECK(h.reject(cid(1))->venue_code == -1021);
  REQUIRE(h.pump([&] { return fx.server.stats().time_requests > times_before; }));
  REQUIRE(h.pump([&] { return h.venue->status().clock_offset_ms >= 25'000; }));

  // With the offset learned, the next order is accepted again.
  const OutNewOrderMsg o2 = new_order(
      cid(2), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 110), kLot);
  m.submit(o2);
  h.send(o2.hdr);
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return h.acks(cid(2)) >= 1;
  }));
  m.drain(h.oc);
  check_orders_agree(fx, m);
  CHECK(m.position() == fx.server.stats().position);
  CHECK(h.venue->cancel_all());  // the learned offset holds for the independent REST connection
  CHECK_FALSE(h.venue->fatal());
  fx.server.set_clock_offset_ms(0);
}

TEST_CASE("recovery: malformed frames are counted and the streams keep working") {
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  const std::uint64_t md_before = h.venue->status().md_messages;
  fx.server.send_malformed_frames(true, true);
  REQUIRE(h.pump([&] { return h.venue->status().md_malformed >= 1; }));
  // The connection is not dropped and the book is still in sync: a bad frame is data, not a fault.
  REQUIRE(h.pump([&] { return h.venue->status().md_messages > md_before + 5; }));
  CHECK(h.venue->status().md == ChannelState::Live);
  CHECK(h.venue->md_feed()->synced_count() == 1);
  CHECK(h.venue->md_feed()->resync_count() == 0);
  CHECK_FALSE(h.venue->fatal());

  OmsMirror m(h.instruments);
  place_resting(h, m, cid(1), resting_bid(fx, 100), kLot);
  check_orders_agree(fx, m);
  CHECK(h.venue->cancel_all());
}

TEST_CASE("recovery: a 418 hard stop and a revoked key stop new orders but never the cancels") {
  SUBCASE("418") {
    ServerFixture fx(quiet_server());
    ReadyHarness h(fx);
    OmsMirror m(h.instruments);
    place_resting(h, m, cid(1), resting_bid(fx, 100), kLot);

    fx.server.ban_next_requests(1);
    const OutNewOrderMsg o = new_order(
        cid(2), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 110), kLot);
    m.submit(o);
    h.send(o.hdr);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return h.oc.count_if<ControlMsg>(EventType::Control, [](const ControlMsg& c) {
        return c.command == ControlCommand::TripVenueKill &&
               static_cast<KillReason>(c.arg) == KillReason::VenueHardStop;
      }) >= 1;
    }));
    CHECK(fx.server.stats().banned_requests >= 1);
    // The kill switch's whole remedy is to cancel, so cancels must still go out.
    CHECK(h.venue->cancel_all());
    REQUIRE(h.pump([&] { return fx.server.stats().open_orders == 0; }));
    CHECK(fx.server.open_client_order_ids().empty());
  }

  SUBCASE("a revoked key") {
    ServerFixture fx(quiet_server());
    ReadyHarness h(fx);
    OmsMirror m(h.instruments);
    place_resting(h, m, cid(1), resting_bid(fx, 100), kLot);

    fx.server.fail_next_auth(1);
    const OutNewOrderMsg o = new_order(
        cid(2), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, resting_bid(fx, 110), kLot);
    m.submit(o);
    h.send(o.hdr);
    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return h.venue->fatal();
    }));
    CHECK(fx.server.stats().key_errors >= 1);
    CHECK(h.oc.count_if<ControlMsg>(EventType::Control, [](const ControlMsg& c) {
      return c.command == ControlCommand::TripVenueKill &&
             static_cast<KillReason>(c.arg) == KillReason::VenueFatal;
    }) >= 1);
    CHECK(h.venue->cancel_all());
    REQUIRE(h.pump([&] { return fx.server.stats().open_orders == 0; }));
  }
}

TEST_CASE("recovery: shadows of orders whose terminal events were lost are swept by the snapshot") {
  // The fixed-size shadow table leaked a slot per order whose end the connector never heard about;
  // enough of them and a new order had no shadow, so its replace was refused as "original unknown".
  ServerFixture fx(quiet_server());
  ReadyHarness h(fx);
  OmsMirror m(h.instruments);
  place_resting(h, m, cid(41), resting_bid(fx, 100), kLot);
  place_resting(h, m, cid(42), resting_bid(fx, 110), kLot);
  REQUIRE(fx.server.stats().open_orders == 2);

  // Both orders fill completely while nobody is listening: their executionReports are dropped, the
  // execution replay books the fills, and nothing ever tells the connector the orders are over.
  fx.server.set_user_stream_muted(true);
  const std::vector<std::string> open = fx.server.open_client_order_ids();
  for (const std::string& id : open) static_cast<void>(fx.server.fill_open_order(id));
  REQUIRE(fx.server.stats().user_events_dropped >= 2);
  REQUIRE(h.venue->shadow_count() == 2);  // the leak: nothing told the connector they ended
  fx.server.set_user_stream_muted(false);
  REQUIRE(fx.server.stats().open_orders == 0);
  fx.server.drop_ws_api_connections(true);
  await_reconcile(h, m);
  check_orders_agree(fx, m);
  CHECK(m.position() == fx.server.stats().position);
  CHECK(h.venue->shadow_count() == 0);  // the snapshot proved them over

  // A new order after the sweep still has its shadow: it can be cancelled.
  place_resting(h, m, cid(43), resting_bid(fx, 120), kLot);
  REQUIRE(h.venue->cancel_all());
  REQUIRE(h.pump([&] {
    m.drain(h.oc);
    return fx.server.stats().open_orders == 0;
  }));
}
