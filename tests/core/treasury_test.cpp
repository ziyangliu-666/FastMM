// Pool treasury: the plan (targets, floors, rounding, caps, unknown balances), the limiter, the
// ledger's text form, and the driver against a scripted port (one transfer at a time, restart
// recovery, a failure that is not repeated, timeouts, dry run).
#include "fastmm/core/treasury.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kS = 1'000'000'000;
constexpr std::int64_t kT0 = 1'790'000'000 * kS;  // a wall-clock time

Notional N(const char* s) {
  return *Notional::parse(s);
}
VenueId V(std::uint8_t v) {
  return VenueId{v};
}

TreasuryConfig pool(std::size_t n) {
  TreasuryConfig c;
  c.enabled = true;
  c.asset = "USDT";
  for (std::size_t i = 0; i < n; ++i) {
    c.members.venue[c.members.count++] = V(static_cast<std::uint8_t>(i));
    c.names[i] = i == 0 ? "main" : "acct" + std::to_string(i);
  }
  c.step = N("0.01");
  return c;
}

std::vector<TreasuryAccount> accounts(std::initializer_list<const char*> free) {
  std::vector<TreasuryAccount> out;
  std::uint8_t v = 0;
  for (const char* f : free) out.push_back({V(v++), N(f), true});
  return out;
}

struct ScriptedPort final : TreasuryPort {
  std::vector<TransferRequest> submitted;
  std::vector<std::string> status_asked;
  std::vector<VenueId> refreshed;
  std::vector<TreasuryEventKind> events;
  TransferResult on_submit{TransferState::Pending, "1", ""};
  TransferResult on_status{TransferState::Done, "1", ""};

  TransferResult submit(const TransferRequest& r) override {
    submitted.push_back(r);
    return on_submit;
  }
  TransferResult status(const TransferRequest& r) override {
    status_asked.push_back(r.client_id);
    return on_status;
  }
  void refresh(VenueId v) override { refreshed.push_back(v); }
  void record(const TreasuryEvent& e) override { events.push_back(e.kind); }
};

std::vector<TreasuryBalance> rows(std::initializer_list<const char*> free, std::int64_t as_of) {
  std::vector<TreasuryBalance> out;
  std::uint8_t v = 0;
  for (const char* f : free)
    out.push_back({V(v++), FixedString<8>(std::string_view("USDT")), N(f), true, as_of});
  return out;
}

}  // namespace

TEST_CASE("core.treasury: equal weights move the surplus to the short account") {
  const TreasuryConfig c = pool(3);
  // Total 3000, target 1000 each; floor 800. acct2 at 100 is short by 900.
  const TreasuryPlan p = plan_transfers(c, accounts({"1900", "1000", "100"}));
  REQUIRE(p.status == TreasuryPlan::Status::Transfers);
  REQUIRE(p.count == 1);
  CHECK(p.transfers[0] == PlannedTransfer{V(0), V(2), N("900")});
}

TEST_CASE("core.treasury: within the threshold nothing moves") {
  const TreasuryConfig c = pool(3);
  // Targets 1000, floor 800: 810 is not short.
  const TreasuryPlan p = plan_transfers(c, accounts({"1190", "1000", "810"}));
  CHECK(p.status == TreasuryPlan::Status::Balanced);
  CHECK(p.count == 0);
}

TEST_CASE("core.treasury: one need is filled from several givers, largest first") {
  const TreasuryConfig c = pool(4);
  // Total 4000, target 1000: acct3 needs 1000, main gives 600, acct1 gives 400.
  const TreasuryPlan p = plan_transfers(c, accounts({"1600", "1400", "1000", "0"}));
  REQUIRE(p.count == 2);
  CHECK(p.transfers[0] == PlannedTransfer{V(0), V(3), N("600")});
  CHECK(p.transfers[1] == PlannedTransfer{V(1), V(3), N("400")});
}

TEST_CASE("core.treasury: weights, min_free, rounding and the caps") {
  TreasuryConfig c = pool(2);
  c.weight[0] = 3;
  c.weight[1] = 1;
  // Total 1000: targets 750 / 250; acct1 at 50 is short by 200.
  TreasuryPlan p = plan_transfers(c, accounts({"950", "50"}));
  REQUIRE(p.count == 1);
  CHECK(p.transfers[0] == PlannedTransfer{V(0), V(1), N("200")});

  // min_free raises the floor: total 1210, targets 907.5 / 302.5, acct1's floor is 300 (not 242),
  // so 260 is short and asks for its target.
  c.min_free[1] = N("300");
  p = plan_transfers(c, accounts({"950", "260"}));
  REQUIRE(p.count == 1);
  CHECK(p.transfers[0].amount == N("42.5"));

  // max_amount caps, step rounds down, min_amount drops what is left.
  c = pool(2);
  c.max_amount = N("123.456");
  c.step = N("1");
  p = plan_transfers(c, accounts({"1000", "0"}));
  REQUIRE(p.count == 1);
  CHECK(p.transfers[0].amount == N("123"));
  c.max_amount = Notional{};
  c.min_amount = N("600");
  p = plan_transfers(c, accounts({"1000", "0"}));  // 500 < 600
  CHECK(p.status == TreasuryPlan::Status::Balanced);
}

TEST_CASE("core.treasury: an unknown or missing balance stops the plan") {
  const TreasuryConfig c = pool(3);
  std::vector<TreasuryAccount> a = accounts({"1000", "0", "500"});
  a[1].known = false;
  CHECK(plan_transfers(c, a).status == TreasuryPlan::Status::Unknown);
  a.pop_back();
  a[1].known = true;
  CHECK(plan_transfers(c, a).status == TreasuryPlan::Status::Unknown);
  CHECK(plan_transfers(pool(1), accounts({"1"})).status == TreasuryPlan::Status::Empty);
}

TEST_CASE("core.treasury: the plan is deterministic, ties go to the account listed first") {
  const TreasuryConfig c = pool(4);
  // Two equal givers and two equal needs.
  const auto a = accounts({"1500", "1500", "500", "500"});
  const TreasuryPlan p = plan_transfers(c, a);
  REQUIRE(p.count == 2);
  CHECK(p.transfers[0] == PlannedTransfer{V(0), V(2), N("500")});
  CHECK(p.transfers[1] == PlannedTransfer{V(1), V(3), N("500")});
  for (int i = 0; i < 10; ++i) {
    const TreasuryPlan q = plan_transfers(c, a);
    CHECK(q.view()[0] == p.view()[0]);
    CHECK(q.view()[1] == p.view()[1]);
  }
}

TEST_CASE("core.treasury: the limiter's interval, hourly count and cool-down") {
  TreasuryConfig c = pool(2);
  c.min_interval_ns = 60 * kS;
  c.max_per_hour = 2;
  c.cooldown_ns = 300 * kS;
  TreasuryLimiter l;
  CHECK(l.check(c, kT0) == TreasuryLimiter::Verdict::Ok);
  l.on_sent(kT0);
  CHECK(l.check(c, kT0 + 59 * kS) == TreasuryLimiter::Verdict::Interval);
  CHECK(l.check(c, kT0 + 60 * kS) == TreasuryLimiter::Verdict::Ok);
  l.on_sent(kT0 + 60 * kS);
  CHECK(l.check(c, kT0 + 200 * kS) == TreasuryLimiter::Verdict::Hourly);
  CHECK(l.check(c, kT0 + 3601 * kS) == TreasuryLimiter::Verdict::Ok);  // the first left the hour
  l.cool_down(c, kT0 + 3601 * kS);
  CHECK(l.check(c, kT0 + 3700 * kS) == TreasuryLimiter::Verdict::Cooldown);
  CHECK(l.check(c, kT0 + 3901 * kS) == TreasuryLimiter::Verdict::Ok);
}

TEST_CASE("core.treasury: the ledger round-trips and refuses an account it does not know") {
  const TreasuryConfig c = pool(3);
  TreasuryLedger l;
  l.seq = 7;
  l.limiter.last_sent_ns = kT0;
  l.limiter.cooldown_until_ns = kT0 + 5;
  l.limiter.sent = {kT0 - 10, kT0};
  TreasuryTransfer t;
  t.req.client_id = "fm0abc";
  t.req.from = V(2);
  t.req.to = V(0);
  t.req.asset = "USDT";
  t.req.amount = N("12.5");
  t.req.created_ms = kT0 / 1'000'000;
  t.phase = TreasuryTransfer::Phase::Unknown;
  t.created_ns = kT0;
  l.in_flight.push_back(t);
  const std::string text = l.serialize(c);
  const auto back = TreasuryLedger::parse(c, text);
  REQUIRE(back.has_value());
  CHECK(back->seq == 7);
  CHECK(back->limiter.cooldown_until_ns == kT0 + 5);
  CHECK(back->limiter.sent.size() == 2);
  REQUIRE(back->in_flight.size() == 1);
  CHECK(back->in_flight[0].req.client_id == "fm0abc");
  CHECK(back->in_flight[0].req.from == V(2));
  CHECK(back->in_flight[0].req.amount == N("12.5"));
  CHECK(back->in_flight[0].phase == TreasuryTransfer::Phase::Unknown);
  CHECK(back->serialize(c) == text);

  CHECK_FALSE(TreasuryLedger::parse(pool(2), text).has_value());  // acct2 is gone
  CHECK_FALSE(TreasuryLedger::parse(c, "something else\n").has_value());
}

TEST_CASE("core.treasury: one transfer at a time, settled by fresh balance reports") {
  TreasuryConfig c = pool(3);
  c.interval_ns = 1 * kS;
  c.min_interval_ns = 0;
  ScriptedPort port;
  Treasury t(c);
  REQUIRE(t.open().has_value());

  t.step(kT0, rows({"1900", "1000", "100"}, kT0), port);
  REQUIRE(port.submitted.size() == 1);  // two transfers might be planned; one is sent
  CHECK(port.submitted[0].from == V(0));
  CHECK(port.submitted[0].to == V(2));
  CHECK(port.submitted[0].amount == N("900"));
  CHECK(port.submitted[0].client_id.size() <= TransferRequest::kMaxClientId);
  CHECK(t.stats().in_flight == 1);

  // In flight: the balances have not moved yet, and nothing new is planned.
  port.on_status = {TransferState::Pending, "1", ""};
  t.step(kT0 + 1 * kS, rows({"1900", "1000", "100"}, kT0), port);
  t.step(kT0 + 2 * kS, rows({"1900", "1000", "100"}, kT0), port);
  CHECK(port.submitted.size() == 1);
  CHECK(port.status_asked.size() == 2);

  // Done: both accounts are refreshed; reports older than the transfer do not count.
  port.on_status = {TransferState::Done, "1", ""};
  t.step(kT0 + 3 * kS, rows({"1900", "1000", "100"}, kT0), port);
  CHECK(t.stats().done == 1);
  CHECK(t.stats().moved == N("900"));
  CHECK(port.refreshed == std::vector<VenueId>{V(0), V(2)});
  t.step(kT0 + 4 * kS, rows({"1900", "1000", "100"}, kT0 + 2 * kS), port);
  CHECK(port.submitted.size() == 1);
  // The refreshed reports show it, and the pool is balanced.
  t.step(kT0 + 5 * kS, rows({"1000", "1000", "1000"}, kT0 + 4 * kS), port);
  CHECK(port.submitted.size() == 1);
  CHECK(t.stats().in_flight == 0);
  CHECK(port.events ==
        std::vector<TreasuryEventKind>{TreasuryEventKind::Sent, TreasuryEventKind::Done});
}

TEST_CASE("core.treasury: a failed transfer is not repeated, a new one follows the cool-down") {
  TreasuryConfig c = pool(2);
  c.interval_ns = 1 * kS;
  c.min_interval_ns = 0;
  c.cooldown_ns = 100 * kS;
  ScriptedPort port;
  port.on_submit = {TransferState::Failed, "", "insufficient balance"};
  Treasury t(c);
  REQUIRE(t.open().has_value());
  t.step(kT0, rows({"1000", "0"}, kT0), port);
  REQUIRE(port.submitted.size() == 1);
  CHECK(t.stats().failed == 1);
  CHECK(t.stats().in_flight == 0);
  for (int s = 1; s < 100; s += 10) t.step(kT0 + s * kS, rows({"1000", "0"}, kT0), port);
  CHECK(port.submitted.size() == 1);
  CHECK(t.stats().limited > 0);
  port.on_submit = {TransferState::Pending, "2", ""};
  t.step(kT0 + 101 * kS, rows({"1000", "0"}, kT0), port);
  REQUIRE(port.submitted.size() == 2);
  CHECK(port.submitted[1].client_id != port.submitted[0].client_id);
}

TEST_CASE("core.treasury: a restart asks for the transfer it left before planning another") {
  const auto path = (tmp_dir() / "treasury_restart.ledger").string();
  std::filesystem::remove(path);
  TreasuryConfig c = pool(2);
  c.interval_ns = 1 * kS;
  c.min_interval_ns = 0;
  c.state_file = path;
  std::string first_id;
  {
    ScriptedPort port;
    port.on_submit = {TransferState::Unknown, "", "connection reset"};
    Treasury t(c);
    REQUIRE(t.open().has_value());
    t.step(kT0, rows({"1000", "0"}, kT0), port);
    REQUIRE(port.submitted.size() == 1);
    first_id = port.submitted[0].client_id;
    CHECK(t.ledger().in_flight.size() == 1);
  }  // the process stops with the transfer's outcome unknown
  {
    ScriptedPort port;
    port.on_status = {TransferState::Pending, "9", ""};
    Treasury t(c);
    REQUIRE(t.open().has_value());
    REQUIRE(t.ledger().in_flight.size() == 1);
    t.step(kT0 + 10 * kS, rows({"1000", "0"}, kT0), port);
    CHECK(port.submitted.empty());  // asked for, not sent again
    CHECK(port.status_asked == std::vector<std::string>{first_id});
    port.on_status = {TransferState::Done, "9", ""};
    t.step(kT0 + 11 * kS, rows({"1000", "0"}, kT0), port);
    CHECK(t.stats().done == 1);
    CHECK(t.ledger().in_flight.empty());
    // A later transfer gets a new id: the counter survived the restart.
    t.step(kT0 + 50 * kS, rows({"1000", "0"}, kT0 + 49 * kS), port);
    REQUIRE(port.submitted.size() == 1);
    CHECK(port.submitted[0].client_id != first_id);
  }
  const auto l = TreasuryLedger::load(c);
  REQUIRE(l.has_value());
  CHECK(l->seq == 2);
}

TEST_CASE("core.treasury: no transfer under the id after the timeout is a failure") {
  TreasuryConfig c = pool(2);
  c.interval_ns = 1 * kS;
  c.timeout_ns = 30 * kS;
  ScriptedPort port;
  port.on_submit = {TransferState::Unknown, "", "timeout"};
  port.on_status = {TransferState::NotFound, "", ""};
  Treasury t(c);
  REQUIRE(t.open().has_value());
  t.step(kT0, rows({"1000", "0"}, kT0), port);
  t.step(kT0 + 10 * kS, rows({"1000", "0"}, kT0), port);
  CHECK(t.stats().in_flight == 1);  // not yet: the venue may not show it yet
  t.step(kT0 + 31 * kS, rows({"1000", "0"}, kT0), port);
  CHECK(t.stats().in_flight == 0);
  CHECK(t.stats().failed == 1);
}

TEST_CASE("core.treasury: a pending transfer past the timeout cools the pool down") {
  TreasuryConfig c = pool(2);
  c.interval_ns = 1 * kS;
  c.timeout_ns = 30 * kS;
  ScriptedPort port;
  port.on_status = {TransferState::Pending, "1", ""};
  Treasury t(c);
  REQUIRE(t.open().has_value());
  t.step(kT0, rows({"1000", "0"}, kT0), port);
  t.step(kT0 + 31 * kS, rows({"1000", "0"}, kT0), port);
  t.step(kT0 + 40 * kS, rows({"1000", "0"}, kT0), port);
  CHECK(t.stats().timed_out == 1);  // once
  CHECK(t.stats().in_flight == 1);
  CHECK(port.submitted.size() == 1);
}

TEST_CASE("core.treasury: a dry run logs the plan and sends nothing") {
  TreasuryConfig c = pool(2);
  c.dry_run = true;
  c.interval_ns = 1 * kS;
  c.min_interval_ns = 60 * kS;
  ScriptedPort port;
  Treasury t(c);
  REQUIRE(t.open().has_value());
  t.step(kT0, rows({"1000", "0"}, kT0), port);
  t.step(kT0 + 2 * kS, rows({"1000", "0"}, kT0), port);  // the same plan: not again yet
  CHECK(port.submitted.empty());
  CHECK(t.stats().dry_run_plans == 1);
  CHECK(port.events == std::vector<TreasuryEventKind>{TreasuryEventKind::Planned});
  t.step(kT0 + 61 * kS, rows({"1000", "0"}, kT0), port);
  CHECK(t.stats().dry_run_plans == 2);
}

TEST_CASE("core.treasury: disabled does nothing") {
  TreasuryConfig c = pool(2);
  c.enabled = false;
  ScriptedPort port;
  Treasury t(c);
  t.step(kT0, rows({"1000", "0"}, kT0), port);
  CHECK(port.submitted.empty());
  CHECK(port.events.empty());
}
