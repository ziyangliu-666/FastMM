// ReconcileDriver against a fake hook set: one snapshot at a time, retry after a failure, the
// generation after a disconnect, the start-up sweep and the shadow sweep.
#include "fastmm/venues/reconcile_driver.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/net/reactor.hpp"

#include <algorithm>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::test;

namespace {

struct FakeHooks final : ReconcileHooks {
  FakeHooks() = default;
  FakeHooks(const FakeHooks&) = delete;
  FakeHooks& operator=(const FakeHooks&) = delete;
  virtual ~FakeHooks() = default;

  int fetches = 0;
  std::uint64_t generation = 0;  // of the last fetch
  bool can_fetch = true;
  int replays = 0;
  bool replay_runs = false;  // the replay answers later (ReconcileDriver::replay_done)
  std::vector<SentShadow> shadows;
  std::vector<ClientOrderId> dropped;

  bool fetch_snapshot(std::uint64_t g) override {
    ++fetches;
    generation = g;
    return can_fetch;
  }
  bool replay_executions() override {
    ++replays;
    return replay_runs;
  }
  void shadow_ids(std::vector<SentShadow>& out) override {
    out.insert(out.end(), shadows.begin(), shadows.end());
  }
  void drop_shadow(ClientOrderId id) override {
    dropped.push_back(id);
    std::erase_if(shadows, [&](const SentShadow& s) { return s.id == id; });
  }
  [[nodiscard]] std::vector<ClientOrderId> shadow_list() const {
    std::vector<ClientOrderId> out;
    for (const SentShadow& s : shadows) out.push_back(s.id);
    std::sort(out.begin(), out.end());
    return out;
  }
};

ClientOrderId id_of(std::uint16_t epoch, std::uint32_t seq) {
  return make_cl_ord_id(epoch, seq);
}

// An order sent, as the connector records it: its shadow stamped with its send sequence.
void sent_only(SentWatermark& sent, FakeHooks& hooks, ClientOrderId id) {
  OrderCommand c;
  c.kind = OrderCommandKind::New;
  c.cl_ord_id = id;
  sent.note(c, net::Reactor::now_ns());
  hooks.shadows.push_back(SentShadow{id, sent.last_seq()});
}

// An order sent and answered: the watermark moves up to it.
void sent_and_answered(SentWatermark& sent, FakeHooks& hooks, ClientOrderId id) {
  sent_only(sent, hooks, id);
  sent.answered(id);
}

struct Rig {
  RecordingSink rs;
  SentWatermark sent;
  FakeHooks hooks;
  ReconcileDriver driver{hooks, sent};
  Collected oc;

  Rig() {
    driver.attach("fake", VenueId{0}, &rs.sink);
    driver.open(true);
  }
  std::vector<const ReconcileMsg*> of_kind(ReconcileMsg::Kind k) {
    oc.take(rs);
    std::vector<const ReconcileMsg*> out;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) != EventType::Reconcile) continue;
      const auto& r = RecordingSink::as<ReconcileMsg>(m);
      if (r.kind == k) out.push_back(&r);
    }
    return out;
  }
  std::size_t begins() { return of_kind(ReconcileMsg::Kind::Begin).size(); }
  // Answers the fetch in flight with one open order.
  void answer(ClientOrderId named = ClientOrderId{}) {
    if (named.valid()) {
      ReconcileMsg& m = driver.add_order(InstrumentId{0});
      m.cl_ord_id = named;
    }
    driver.fetched(hooks.generation, true);
  }
};

}  // namespace

TEST_CASE("reconcile_driver: one snapshot at a time, a request meanwhile gets the next one") {
  Rig r;
  r.driver.request();
  CHECK(r.hooks.replays == 1);
  CHECK(r.hooks.fetches == 1);
  CHECK(r.driver.busy());
  const std::uint64_t first = r.hooks.generation;
  r.driver.request();
  r.driver.request();
  CHECK(r.hooks.fetches == 1);  // coalesced
  r.answer(id_of(1, 1));
  CHECK(r.begins() == 1);
  CHECK(r.of_kind(ReconcileMsg::Kind::OpenOrder).size() == 1);
  CHECK(r.of_kind(ReconcileMsg::Kind::End).size() == 1);
  // The two requests made while the first was in flight: one more snapshot, not two.
  CHECK(r.hooks.fetches == 2);
  CHECK(r.hooks.generation > first);
  r.answer();
  CHECK(r.begins() == 2);
  CHECK(r.hooks.fetches == 2);
  CHECK_FALSE(r.driver.busy());
  CHECK(r.driver.snapshots() == 2);
}

TEST_CASE("reconcile_driver: the replay goes first, and a request during it is served by it") {
  Rig r;
  r.hooks.replay_runs = true;
  r.driver.request();
  CHECK(r.hooks.replays == 1);
  CHECK(r.hooks.fetches == 0);
  r.driver.request();  // served by the snapshot the replay releases
  CHECK(r.hooks.replays == 1);
  r.driver.replay_done(true);
  CHECK(r.hooks.fetches == 1);
  r.answer();
  const auto begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 1);
  CHECK((begins[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
  CHECK(r.hooks.fetches == 1);

  // An incomplete replay: the snapshot goes out, not exact. No replay possible: not exact either.
  r.driver.request();
  r.driver.replay_done(false);
  r.answer();
  r.hooks.replay_runs = false;
  r.driver.request();
  r.answer();
  const auto all = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(all.size() == 3);
  CHECK((all[1]->flags & ReconcileMsg::kExecutionsExact) == 0);
  CHECK((all[2]->flags & ReconcileMsg::kExecutionsExact) == 0);
  // A replay nobody waits for (the periodic one) releases nothing.
  r.driver.replay_done(true);
  CHECK(r.hooks.fetches == 3);
}

TEST_CASE("reconcile_driver: a failed snapshot is asked again after the retry delay") {
  Rig r;
  r.driver.request();
  const std::uint64_t failed = r.hooks.generation;
  r.driver.fetched(failed, false);
  CHECK(r.begins() == 0);  // nothing of a failed snapshot reaches the engine
  CHECK_FALSE(r.driver.busy());
  CHECK(r.driver.retry_pending());
  const std::int64_t now = net::Reactor::now_ns();
  r.driver.on_timer(now);
  CHECK(r.hooks.fetches == 1);  // not yet
  r.driver.on_timer(now + ReconcileDriver::kRetryNs + 1'000'000'000);
  CHECK(r.hooks.fetches == 2);
  CHECK(r.hooks.replays == 2);  // the whole reconciliation again, replay first
  CHECK_FALSE(r.driver.retry_pending());
  r.driver.fetched(failed, true);  // a late answer to the failed one is not this snapshot
  CHECK(r.begins() == 0);
  r.answer();
  CHECK(r.begins() == 1);
  CHECK(r.driver.failures() == 1);

  // A snapshot that cannot even be asked for is retried the same way.
  r.hooks.can_fetch = false;
  r.driver.request();
  CHECK(r.driver.retry_pending());
  r.hooks.can_fetch = true;
  r.driver.on_timer(net::Reactor::now_ns() + ReconcileDriver::kRetryNs + 1'000'000'000);
  CHECK(r.hooks.fetches == 4);
  r.answer();
  CHECK(r.begins() == 2);
}

TEST_CASE("reconcile_driver: a lost transport and a fetch that never answers are retried") {
  Rig r;
  r.driver.request();
  const std::uint64_t lost = r.hooks.generation;
  r.driver.transport_lost();
  CHECK_FALSE(r.driver.busy());
  CHECK(r.driver.retry_pending());
  // The reconnect asks before the retry is due: it goes at once, and the retry is dropped.
  r.driver.request();
  CHECK(r.hooks.fetches == 2);
  CHECK_FALSE(r.driver.retry_pending());
  r.driver.fetched(lost, true);
  CHECK(r.begins() == 0);
  // No answer at all: abandoned after the timeout (and, this tick being past the retry delay
  // too, asked again at once).
  r.driver.on_timer(net::Reactor::now_ns() + ReconcileDriver::kFetchTimeoutNs + 1);
  CHECK(r.driver.failures() == 2);
  CHECK(r.hooks.fetches == 3);

  // A replay cut off with the connection: the snapshot waiting for it is asked again too.
  Rig w;
  w.hooks.replay_runs = true;
  w.driver.request();
  w.driver.transport_lost();
  CHECK(w.driver.retry_pending());
  w.driver.replay_done(true);  // whatever arrives from the old connection releases nothing
  CHECK(w.hooks.fetches == 0);
}

TEST_CASE("reconcile_driver: after close() nothing in flight counts and nothing is asked") {
  Rig r;
  r.driver.request();
  const std::uint64_t before = r.hooks.generation;
  r.driver.close();
  CHECK_FALSE(r.driver.busy());
  r.driver.fetched(before, true);  // aborted by the shutdown
  CHECK(r.begins() == 0);
  r.driver.request();
  r.driver.sweep();
  r.driver.replay_done(true);
  r.driver.on_timer(net::Reactor::now_ns() + ReconcileDriver::kRetryNs * 10);
  CHECK(r.hooks.fetches == 1);
  CHECK(r.hooks.replays == 1);

  r.driver.open(true);
  r.driver.request();
  CHECK(r.hooks.fetches == 2);
  CHECK(r.hooks.generation > before);
  r.answer();
  CHECK(r.begins() == 1);

  // Not enabled (dry run, no keys): nothing is ever asked.
  Rig off;
  off.driver.open(false);
  off.driver.request();
  off.driver.sweep();
  CHECK(off.hooks.replays == 0);
  CHECK(off.hooks.fetches == 0);
}

TEST_CASE("reconcile_driver: the start-up sweep carries an empty watermark") {
  Rig r;
  sent_and_answered(r.sent, r.hooks, id_of(5, 1));
  r.driver.sweep();
  r.answer(id_of(3, 7));  // an earlier session's order: the engine cancels it
  auto begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 1);
  CHECK((begins[0]->flags & ReconcileMsg::kSentWatermark) != 0);
  CHECK_FALSE(begins[0]->sent_watermark.valid());
  CHECK(r.of_kind(ReconcileMsg::Kind::OpenOrder)[0]->cl_ord_id == id_of(3, 7));

  // An ordinary request pending with it makes it an ordinary snapshot.
  r.hooks.replay_runs = true;
  r.driver.sweep();
  r.driver.request();
  r.driver.replay_done(true);
  r.answer();
  begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 2);
  CHECK(begins[1]->sent_watermark == id_of(5, 1));

  // A failed sweep is retried as a sweep.
  r.hooks.replay_runs = false;
  r.driver.sweep();
  r.driver.fetched(r.hooks.generation, false);
  r.driver.on_timer(net::Reactor::now_ns() + ReconcileDriver::kRetryNs + 1'000'000'000);
  r.answer();
  begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 3);
  CHECK_FALSE(begins[2]->sent_watermark.valid());
}

TEST_CASE("reconcile_driver: shadows the snapshot proves over are dropped, no others") {
  Rig r;
  const ClientOrderId resting = id_of(5, 2);
  const ClientOrderId lost_1 = id_of(5, 1);
  const ClientOrderId lost_3 = id_of(5, 3);
  const ClientOrderId in_flight = id_of(5, 4);
  for (const ClientOrderId id : {lost_1, resting, lost_3}) sent_and_answered(r.sent, r.hooks, id);
  sent_only(r.sent, r.hooks, in_flight);  // sent, not answered

  // The start-up sweep's empty watermark tells nothing apart: every shadow stays.
  r.driver.sweep();
  r.answer(resting);
  CHECK(r.hooks.dropped.empty());

  r.driver.request();
  r.answer(resting);
  const auto begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 2);
  CHECK(begins[1]->sent_watermark == lost_3);
  std::sort(r.hooks.dropped.begin(), r.hooks.dropped.end());
  CHECK(r.hooks.dropped == std::vector<ClientOrderId>{lost_1, lost_3});
  CHECK(r.driver.shadows_swept() == 2);
  CHECK(r.hooks.shadow_list() == std::vector<ClientOrderId>{resting, in_flight});
}

TEST_CASE("reconcile_driver: several engines' shadows are judged by send order, not by id") {
  // Behind fastmm-gateway the connector sends two engines' orders interleaved; their ids are in
  // send order within each epoch only. The sweep used to compare ids of the watermark's epoch and
  // left every other engine's shadows in the table for good.
  Rig r;
  const ClientOrderId a_resting = id_of(7, 500);
  const ClientOrderId b_lost_1 = id_of(3, 1);
  const ClientOrderId a_lost = id_of(7, 501);
  const ClientOrderId b_lost_2 = id_of(3, 2);
  const ClientOrderId a_in_flight = id_of(7, 502);
  const ClientOrderId b_after = id_of(3, 3);  // answered, but sent after a_in_flight
  for (const ClientOrderId id : {a_resting, b_lost_1, a_lost, b_lost_2})
    sent_and_answered(r.sent, r.hooks, id);
  sent_only(r.sent, r.hooks, a_in_flight);
  sent_and_answered(r.sent, r.hooks, b_after);

  r.driver.request();
  r.answer(a_resting);
  const auto begins = r.of_kind(ReconcileMsg::Kind::Begin);
  REQUIRE(begins.size() == 1);
  CHECK(begins[0]->sent_watermark == b_lost_2);  // the last sent before the one in flight
  CHECK(r.driver.shadows_swept() == 3);
  std::sort(r.hooks.dropped.begin(), r.hooks.dropped.end());
  CHECK(r.hooks.dropped == std::vector<ClientOrderId>{b_lost_1, b_lost_2, a_lost});
  CHECK(r.hooks.shadow_list() == std::vector<ClientOrderId>{b_after, a_resting, a_in_flight});

  // Once the one in flight is answered, the next snapshot judges it and the one after it.
  r.sent.answered(a_in_flight);
  r.driver.request();
  r.answer(a_resting);
  CHECK(r.driver.shadows_swept() == 5);
  CHECK(r.hooks.shadow_list() == std::vector<ClientOrderId>{a_resting});
}

TEST_CASE("reconcile_driver: positions follow the open orders between Begin and End") {
  Rig r;
  r.driver.request();
  ReconcileMsg& o = r.driver.add_order(InstrumentId{1});
  o.cl_ord_id = id_of(1, 1);
  r.driver.add_position(InstrumentId{1}, Qty::from_int(3), Price::from_int(100));
  r.driver.fetched(r.hooks.generation, true);
  r.oc.take(r.rs);
  std::vector<ReconcileMsg::Kind> kinds;
  for (const auto& m : r.oc.all) kinds.push_back(RecordingSink::as<ReconcileMsg>(m).kind);
  CHECK(kinds == std::vector<ReconcileMsg::Kind>{ReconcileMsg::Kind::Begin,
                                                 ReconcileMsg::Kind::OpenOrder,
                                                 ReconcileMsg::Kind::Position,
                                                 ReconcileMsg::Kind::End});
  const auto& pos = RecordingSink::as<ReconcileMsg>(r.oc.all[2]);
  CHECK(pos.hdr.instrument == InstrumentId{1});
  CHECK(pos.hdr.venue == VenueId{0});
  CHECK(pos.position_qty == Qty::from_int(3));
  CHECK(pos.avg_px == Price::from_int(100));
}
