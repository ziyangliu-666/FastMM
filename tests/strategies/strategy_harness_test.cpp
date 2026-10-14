// StrategyHarness: BasicMM through the real engine and simulated venue, driven by the harness's
// market data, fills, control and connection events and virtual time.
#include "fastmm/testing/strategy_harness.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/basic_mm.hpp"

#include <optional>
#include <vector>

using namespace fastmm;
using fastmm::sim::StrategyHarness;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
const ParamMap kParams{{"half_spread_bps", "10"},
                       {"quote_qty", "0.01"},
                       {"max_inventory", "0.05"},
                       {"skew_bps_per_unit", "0"},
                       {"pull_on_stale_ms", "0"}};
}  // namespace

TEST_CASE("strategies.harness: BasicMM quotes, fills, pauses, resumes and reconnects") {
  StrategyHarness<BasicMM> h(kParams);
  const InstrumentId id = h.instrument();
  h.book("100.00", "100.02");
  CHECK(h.working_orders().empty());  // sent, not yet acknowledged
  h.advance(milliseconds(1));
  std::vector<Order> w = h.working_orders();
  REQUIRE(w.size() == 2);
  CHECK(w[0].side == Side::Buy);
  CHECK(w[0].price == px("99.90"));
  CHECK(w[1].side == Side::Sell);
  CHECK(w[1].price == px("100.12"));

  // A taker at the venue fills our bid; the fill reaches the engine and BasicMM requotes.
  REQUIRE(h.fill(Side::Buy));
  CHECK(h.engine().position(id).qty == qt("0.01"));
  h.advance(milliseconds(1));
  w = h.working_orders();
  REQUIRE(w.size() == 2);
  CHECK(w[0].price == px("99.90"));  // no skew configured: the same bid again

  // Pulled: everything is cancelled, book updates are ignored.
  h.pull_quotes();
  h.advance(milliseconds(1));
  CHECK(h.working_orders().empty());
  h.book("100.00", "100.02");
  h.advance(milliseconds(1));
  CHECK(h.working_orders().empty());
  // Resumed with the mid unchanged: BasicMM requotes at once (on_quoting).
  h.resume_quotes();
  h.advance(milliseconds(1));
  CHECK(h.working_orders().size() == 2);

  // The market-data channel drops: books cleared, quotes pulled; back Live, a book requotes.
  h.disconnect();
  h.advance(milliseconds(1));
  CHECK(h.working_orders().empty());
  CHECK_FALSE(h.engine().book(id).is_valid());
  h.reconnect();
  h.book("100.10", "100.12");
  h.advance(milliseconds(1));
  CHECK(h.working_orders().size() == 2);
  // A partial fill of the ask.
  REQUIRE(h.fill(Side::Sell, qt("0.004")));
  CHECK(h.engine().position(id).qty == qt("0.006"));
}

TEST_CASE("strategies.harness: advance fires timers and trades reach the engine") {
  ParamMap p = kParams;
  p["pull_on_stale_ms"] = "500";
  StrategyHarness<BasicMM> h(p);
  h.book("100.00", "100.02");
  h.trade(px("100.02"), qt("0.5"), Side::Buy);
  CHECK(h.engine().stats().trades == 1);
  h.advance(milliseconds(10));
  REQUIRE(h.working_orders().size() == 2);
  const Timestamp before = h.now();
  h.advance(seconds(1));  // no book updates: the stale timer pulls the quotes
  CHECK(h.now() - before == seconds(1));
  CHECK(h.engine().stats().timers_fired >= 5);
  CHECK(h.working_orders().empty());
  CHECK_THROWS_AS(StrategyHarness<BasicMM>({{"levels", "99"}}), std::invalid_argument);
}

namespace {
// Keeps who traded in each trade it sees.
struct PartiesProbe {
  std::vector<std::optional<TradeParties>> seen;
  void on_trade(auto&, InstrumentId, const TradeMsg& m) noexcept {
    const TradeParties* p = m.parties();
    seen.push_back(p != nullptr ? std::optional<TradeParties>(*p) : std::nullopt);
  }
};
}  // namespace

TEST_CASE("strategies.harness: on_trade reads the buyer and the seller the venue named") {
  StrategyHarness<PartiesProbe> h;
  h.book("100.00", "100.02");
  alignas(64) std::byte buf[TradeMsg::kSizeWithParties]{};
  auto& t = *reinterpret_cast<TradeMsg*>(buf);
  init_header(t, EventType::Trade, h.instrument(), VenueId{0});
  t.price = px("100.02");
  t.qty = qt("0.5");
  t.aggressor = Side::Buy;
  const std::uint8_t addr[20] = {0x35, 0xf1, 0x3d, 0xc7, 0x14, 0x26, 0xd4, 0xa4, 0x29, 0xcf,
                                 0x8b, 0xad, 0x69, 0xc4, 0xb3, 0x9a, 0xbb, 0xbf, 0x98, 0x96};
  const TradeParties named{PartyId::of_address(addr), PartyId::of_account(751368)};
  t.set_parties(named);
  CHECK(t.hdr.len == TradeMsg::kSizeWithParties);
  h.push(t.hdr);
  h.trade(px("100.00"), qt("0.1"), Side::Sell);  // a trade that names nobody
  const auto& seen = h.strategy().seen;
  REQUIRE(seen.size() == 2);
  REQUIRE(seen[0].has_value());
  CHECK(*seen[0] == named);
  CHECK(seen[0]->buyer.kind == PartyIdKind::Address);
  CHECK(seen[0]->buyer.len == 20);
  CHECK(seen[0]->seller.account() == 751368);
  CHECK_FALSE(seen[1].has_value());
}
