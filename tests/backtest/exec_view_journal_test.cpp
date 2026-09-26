// The execution view on a recorded live session: what the engine computes online equals what the
// backtest tools compute over the whole journal. A hand-built journal always runs; a real session
// runs when FASTMM_EXEC_VIEW_JOURNAL names one (a live .fmj with its configuration embedded).
#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/fill_check.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/own_quantity.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/lead_mm.hpp"

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kV0 = 1'700'000'000'000 * kMs;

struct OwnAgreement {
  std::uint64_t levels = 0;  // depth levels compared
  std::uint64_t own = 0;     // ... with our quantity in them
  std::uint64_t differ = 0;  // online != whole-journal
};

// OwnQuantity fed the journal in recorded order, asked at each depth message's venue time for each
// of its levels (what the engine's ctx.own_qty answers then), against the stripper that read the
// whole journal first.
OwnAgreement own_online_vs_journal(const std::string& path) {
  JournalReader r;
  REQUIRE(r.open(path));
  const OwnOrderStripper whole(r);
  OwnQuantity online;
  OwnAgreement a;
  r.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0) {
      if ((h->flags & EventHeader::kDropped) == 0) online.on_outbound(*h);
      return;
    }
    online.on_inbound(*h);
    if (h->type != EventType::BookDelta && h->type != EventType::BookSnapshot) return;
    const auto& d = msg_cast<BookDeltaMsg>(h);
    const Timestamp t = venue_ts(*h);
    for (std::uint32_t k = 0; k < d.bid_count + d.ask_count; ++k) {
      const Side s = k < d.bid_count ? Side::Buy : Side::Sell;
      const Price p = d.levels()[k].price;
      const Qty mine = online.own_at(h->instrument, s, p, t);
      const Qty all = whole.own_at(h->instrument, s, p, t);
      ++a.levels;
      if (all.is_positive()) ++a.own;
      if (mine != all) ++a.differ;
    }
  });
  return a;
}

// LeadMM as recorded, plus the queue estimate of each order at its ack.
std::unordered_map<std::uint64_t, Qty>* g_ahead_at_ack = nullptr;

class LeadProbe : public LeadMM {
 public:
  static constexpr std::string_view name() noexcept { return "lead_mm"; }
  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    LeadMM::on_start(ctx);
    static_cast<void>(ctx.queue_ahead(ClientOrderId{}));
  }
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    if (u.prev != OrderState::PendingNew || u.order.state != OrderState::Live) return;
    if (const auto q = ctx.queue_ahead(u.order.cl_ord_id)) {
      (*g_ahead_at_ack)[u.order.cl_ord_id.value] = *q;
    }
  }
};

}  // namespace

TEST_CASE("backtest.exec_view: own quantity online equals the stripper's on a live journal") {
  const std::string path = tmp_path("exec_view_own.fmj");
  {
    JournalBuilder j(path, /*live=*/true);
    j.venue = kV0;
    j.book(true, {{"100.00", "1"}}, {{"100.05", "1"}});
    j.out_new(1, Side::Buy, "100.00", "1");
    j.out_new(2, Side::Buy, "100.01", "2");
    j.venue = kV0 + kMs;
    j.ack(1);
    j.ack(2);
    j.venue = kV0 + 2 * kMs;
    j.book(false, {{"100.01", "2"}, {"100.00", "2"}}, {});
    j.venue = kV0 + 3 * kMs;
    j.fill(2, "100.01", "0.5", "1.5", "7");
    j.venue = kV0 + 3 * kMs + 400'000;
    j.book(false, {{"100.01", "1.5"}}, {});
    j.out_cancel(1);
    j.venue = kV0 + 5 * kMs;
    j.cancel_ack(1);
    j.venue = kV0 + 4 * kMs;  // stamped before the cancel, received after it
    j.book(false, {{"100.00", "2"}}, {});
    j.venue = kV0 + 6 * kMs;
    j.book(false, {{"100.00", "1"}}, {});
    j.close();
  }
  const OwnAgreement a = own_online_vs_journal(path);
  CHECK(a.levels == 7);
  CHECK(a.own == 4);
  CHECK(a.differ == 0);
}

TEST_CASE("backtest.exec_view: a recorded session (FASTMM_EXEC_VIEW_JOURNAL)") {
  const char* env = std::getenv("FASTMM_EXEC_VIEW_JOURNAL");
  if (env == nullptr || *env == '\0') {
    MESSAGE("FASTMM_EXEC_VIEW_JOURNAL is not set: skipped");
    return;
  }
  const std::string path = env;

  // Our quantity in the feed: the engine's online answer against the whole-journal stripper.
  const OwnAgreement a = own_online_vs_journal(path);
  MESSAGE("own quantity: " << a.levels << " depth levels, " << a.own << " with ours, " << a.differ
                           << " differ online");
  CHECK(a.own > 0);

  // Queue position at the ack: the replayed engine (LeadMM, the recorded config) against
  // fill-check at the same conservatism.
  const BacktestConfig cfg = journal_config(path);
  const double c = static_cast<double>(cfg.transport.queue_conservatism_bps) / 10'000.0;
  std::unordered_map<std::uint64_t, Qty> ahead;
  g_ahead_at_ack = &ahead;
  ReplayStrategy probe;
  probe.name = "lead_mm";
  probe.schema = &LeadMM::schema();
  probe.make = [](RunnerDeps& deps) {
    return static_cast<sim::ReplayBackend*>(deps.backend)->make_runner<LeadProbe>(deps);
  };
  const ReplayResult rr = replay_journal(path, cfg, ReplayOptions{}, probe);
  g_ahead_at_ack = nullptr;
  CHECK(rr.ok());
  const double cs[] = {c};
  const FillCheckResult fc = fill_check(path, cs);
  std::uint64_t both = 0;
  std::uint64_t equal = 0;
  for (const FillCheckOrder& o : fc.orders) {
    const auto it = ahead.find(o.cl_ord_id.value);
    if (it == ahead.end()) {
      MESSAGE("order " << o.cl_ord_id.value << " has no engine estimate at its ack");
      continue;
    }
    ++both;
    if (it->second == o.queue_ahead) {
      ++equal;
    } else {
      MESSAGE("order " << o.cl_ord_id.value << " at " << o.ack_ts.ns << ": engine "
                       << it->second.raw << ", fill-check " << o.queue_ahead.raw);
    }
  }
  MESSAGE("queue ahead at the ack: " << ahead.size() << " orders placed by the engine, "
                                     << fc.orders.size() << " by fill-check, " << both
                                     << " in both, " << equal << " equal");
  CHECK(both > 0);
  CHECK(equal * 100 >= both * 90);
}
