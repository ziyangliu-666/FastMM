// FillAuditor: the windows it audits, what it counts, and "book" asking the venue's replay for
// the executions the engine missed. A scripted venue answers on the calling thread.
#include "fastmm/live/fill_auditor.hpp"

#include "test_support.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::live;

namespace {

AuditFill exec(const std::string& id, std::int64_t time_ms, std::int64_t qty_raw = 100) {
  AuditFill f;
  f.symbol = "BTCUSDT";
  f.exec_id = id;
  f.side = Side::Buy;
  f.price_raw = 7'000'000;
  f.qty_raw = qty_raw;
  f.time_ms = time_ms;
  f.order_id = "1";
  return f;
}

// Answers audit_executions() at once with `rows` (complete unless told otherwise); records the
// windows asked for and the replays requested.
class ScriptedVenue final : public venues::Venue {
 public:
  std::vector<AuditFill> rows;
  bool complete = true;
  bool refuse = false;
  std::vector<std::pair<std::int64_t, std::int64_t>> asked;
  std::vector<std::int64_t> replays;

  [[nodiscard]] VenueId id() const noexcept override { return VenueId{0}; }
  [[nodiscard]] std::string_view name() const noexcept override { return "scripted"; }
  [[nodiscard]] venues::VenueCaps caps() const noexcept override { return {}; }
  Result<void, std::string> load_reference_data(InstrumentTable&) override { return {}; }
  void attach(const venues::SymbolTable&,
              const InstrumentTable&,
              venues::EventSink&,
              venues::EventSink&,
              MsgRing*) override {}
  void connect(net::Reactor&) override {}
  void disconnect() override {}
  void subscribe(std::span<const InstrumentId>) override {}
  void on_timer(std::int64_t) override {}
  void on_wake() override {}
  void send_now(std::span<const EventHeader* const>) override {}
  void request_open_orders() override {}
  bool cancel_all() override { return true; }
  [[nodiscard]] venues::VenueStatus status() const noexcept override { return {}; }
  bool request_executions(std::int64_t since_venue_ms) override {
    replays.push_back(since_venue_ms);
    return true;
  }
  bool audit_executions(std::int64_t start_ms,
                        std::int64_t end_ms,
                        std::function<void(bool, std::vector<AuditFill>)> done) override {
    if (refuse) return false;
    asked.emplace_back(start_ms, end_ms);
    std::vector<AuditFill> in;
    for (const AuditFill& f : rows) {
      if (f.time_ms >= start_ms && f.time_ms <= end_ms) in.push_back(f);
    }
    done(complete, std::move(in));
    return true;
  }
  [[nodiscard]] bool can_audit_executions() const noexcept override { return true; }
};

struct Rig {
  ScriptedVenue venue;
  std::vector<AuditFill> booked;
  std::vector<std::pair<std::int64_t, std::int64_t>> read;
  FillAuditor auditor;

  explicit Rig(bool book, std::int64_t start_ms = 100'000)
      : auditor(
            {FillAuditTarget{
                "spot", &venue, [](std::function<void()> task) { task(); }, 10'000, 5'000, book}},
            [this](const std::string& v,
                   std::int64_t from,
                   std::int64_t to) -> Result<std::vector<AuditFill>, std::string> {
              CHECK(v == "spot");
              read.emplace_back(from, to);
              std::vector<AuditFill> out;
              for (const AuditFill& f : booked) {
                if (f.time_ms >= from && f.time_ms <= to) out.push_back(f);
              }
              return out;
            },
            start_ms) {}
};

}  // namespace

TEST_CASE("integration.fill_auditor: windows follow each other and a clean audit counts") {
  Rig rig(false);
  rig.venue.rows = {exec("1", 101'000), exec("2", 120'000)};
  rig.booked = rig.venue.rows;
  // Nothing to audit until the lag has passed the session's start.
  CHECK_FALSE(rig.auditor.audit_once(0, 104'000));
  CHECK(rig.venue.asked.empty());

  auto r = rig.auditor.audit_once(0, 115'000);  // [100000, 110000]
  REQUIRE(r);
  CHECK(r->clean());
  CHECK(r->matched == 1);
  REQUIRE(rig.venue.asked.size() == 1);
  CHECK(rig.venue.asked[0] ==
        std::pair<std::int64_t, std::int64_t>{100'000 - FillAuditor::kMarginMs,
                                              110'000 + FillAuditor::kMarginMs});
  r = rig.auditor.audit_once(0, 130'000);  // [110001, 125000]
  REQUIRE(r);
  CHECK(r->from_ms == 110'001);
  CHECK(r->to_ms == 125'000);
  CHECK(r->matched == 1);
  const FillAuditCounters c = rig.auditor.counters(0);
  CHECK(c.audits == 2);
  CHECK(c.failures == 0);
  CHECK(c.missing + c.phantom + c.mismatched + c.duplicates == 0);
}

TEST_CASE("integration.fill_auditor: differences are counted once, a failed read is read again") {
  Rig rig(false);
  rig.venue.rows = {exec("1", 101'000), exec("2", 102'000), exec("3", 103'000, 100)};
  rig.booked = {exec("1", 101'000), exec("3", 103'000, 90), exec("4", 104'000)};
  rig.venue.complete = false;
  CHECK_FALSE(rig.auditor.audit_once(0, 115'000));
  CHECK(rig.auditor.counters(0).failures == 1);
  rig.venue.refuse = true;
  CHECK_FALSE(rig.auditor.audit_once(0, 115'000));
  CHECK(rig.auditor.counters(0).failures == 2);
  rig.venue.refuse = false;
  rig.venue.complete = true;
  auto r = rig.auditor.audit_once(0, 115'000);
  REQUIRE(r);
  CHECK(r->from_ms == 100'000);  // the failed window, read again
  REQUIRE(r->missing.size() == 1);
  CHECK(r->missing[0].exec_id == "2");
  REQUIRE(r->phantom.size() == 1);
  CHECK(r->phantom[0].exec_id == "4");
  REQUIRE(r->mismatched.size() == 1);
  FillAuditCounters c = rig.auditor.counters(0);
  CHECK(c.audits == 1);
  CHECK(c.missing == 1);
  CHECK(c.phantom == 1);
  CHECK(c.mismatched == 1);
  CHECK(c.booked == 0);
  CHECK(rig.venue.replays.empty());  // "report" books nothing
}

TEST_CASE("integration.fill_auditor: book asks the replay once and reads the window again") {
  Rig rig(true);
  rig.venue.rows = {exec("1", 101'000), exec("2", 106'000)};
  rig.booked = {exec("1", 101'000)};
  auto r = rig.auditor.audit_once(0, 115'000);
  REQUIRE(r);
  REQUIRE(rig.venue.replays.size() == 1);
  CHECK(rig.venue.replays[0] == 106'000);
  CHECK(rig.auditor.counters(0).booked == 1);
  // The replay booked it: the next audit starts at the missing execution and finds it.
  rig.booked.push_back(exec("2", 106'000));
  r = rig.auditor.audit_once(0, 125'000);
  REQUIRE(r);
  CHECK(r->from_ms == 106'000);
  CHECK(r->clean());
  CHECK(r->matched == 1);
  // One the replay does not book is asked for once, and counted once.
  rig.venue.rows.push_back(exec("3", 121'000));
  r = rig.auditor.audit_once(0, 135'000);
  REQUIRE(r);
  CHECK(r->missing.size() == 1);
  r = rig.auditor.audit_once(0, 145'000);
  REQUIRE(r);
  CHECK(r->from_ms == 121'000);
  CHECK(r->missing.size() == 1);
  CHECK(rig.venue.replays.size() == 2);
  const FillAuditCounters c = rig.auditor.counters(0);
  CHECK(c.missing == 2);
  CHECK(c.booked == 2);
  r = rig.auditor.audit_once(0, 155'000);
  REQUIRE(r);
  CHECK(r->from_ms == 140'001);  // past it now
}

TEST_CASE("integration.fill_auditor: the thread starts and stops") {
  Rig rig(false);
  rig.auditor.start();
  rig.auditor.stop();
  CHECK(rig.auditor.counters(0).audits == 0);
}
