#pragma once
// StrategyContext: the API a strategy sees (ADR-0012, section 2). A thin, non-owning facade over
// the Engine, so strategies never include engine.hpp and cannot reach into internals. Every
// order-API call marks the strategy's decision time for the serialize latency interval.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/balance_book.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/order_budget.hpp"
#include "fastmm/core/perp_book.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/risk_limits.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/venue_health.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace fastmm {

template <class Engine>
class StrategyContext {
 public:
  using Book = typename Engine::Book;

  explicit StrategyContext(Engine* e) noexcept : e_(e) {}

  // ---- time and reference data --------------------------------------------------------------

  [[nodiscard]] Timestamp now() const noexcept { return e_->now(); }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const noexcept {
    return e_->instrument(id);
  }
  [[nodiscard]] const InstrumentTable& instruments() const noexcept { return e_->instruments(); }
  [[nodiscard]] bool contains(InstrumentId id) const noexcept {
    return e_->instruments().contains(id);
  }

  // ---- market data --------------------------------------------------------------------------

  [[nodiscard]] const Book& book(InstrumentId id) const noexcept { return e_->book(id); }

  // Our resting quantity that the venue's feed shows at (id, side, px): the book and the tickers of
  // a live venue include our own orders. As of the book's last update, or of venue time `at` (a
  // BookTicker's exch_ts). An order counts from its ack's venue time to its end's, less its fills,
  // so a depth update stamped before our cancel still includes it. Zero in the simulator, whose
  // feed does not contain our orders.
  [[nodiscard]] Qty own_qty(InstrumentId id, Side side, Price px) const noexcept {
    return e_->own_qty(id, side, px);
  }
  [[nodiscard]] Qty own_qty(InstrumentId id, Side side, Price px, Timestamp at) const noexcept {
    return e_->own_qty(id, side, px, at);
  }
  // The book's best level on one side with own_qty taken out; a level that was only ours is
  // skipped. Level{} when none is left.
  [[nodiscard]] Level best_ex_self(InstrumentId id, Side side) const noexcept {
    return e_->best_ex_self(id, side);
  }

  // ---- portfolio ----------------------------------------------------------------------------

  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return e_->position(id);
  }
  // PnL totals over every instrument (a loop over all positions; not for every event).
  [[nodiscard]] Portfolio portfolio() const noexcept { return e_->positions().portfolio(); }

  // ---- quoting: the QuoteManager diffs the desired ladder against working orders --------------

  // Returns false when the quotes were ignored: quoting is disabled (control pull, kill switch,
  // reconciliation; on_quoting reports the changes) or the instrument is not in the table.
  bool set_quotes(InstrumentId id, const DesiredQuotes& q) noexcept {
    e_->mark_decision();
    return e_->set_quotes(id, q);
  }
  void pull_quotes(InstrumentId id) noexcept {
    e_->mark_decision();
    e_->pull_quotes(id);
  }
  // Why the strategy asks what it asks on a side, for the operator: a short reason
  // ("inventory_cap", "model_off"; empty clears it, at most 23 bytes kept) and the budget it gave
  // the side in the settlement currency. The status file shows it next to what the engine finds
  // in the way (core/quote_diag.hpp); the store records each change. No allocation.
  void note_quote(InstrumentId id,
                  Side side,
                  std::string_view reason,
                  Notional budget = {}) noexcept {
    e_->note_quote(id, side, reason, budget);
  }
  // A number the strategy publishes by name, for the status file (a budget split, a model's
  // state): the first 32 names are kept, a name at most 23 bytes. No allocation.
  void metric(std::string_view name, double value) noexcept { e_->metric(name, value); }
  void pull_all_quotes() noexcept {
    e_->mark_decision();
    e_->pull_all_quotes();
  }
  // The open order in a quote slot (level 0 is closest to the mid), or nullptr.
  [[nodiscard]] const Order* working_quote(InstrumentId id,
                                           Side side,
                                           std::uint32_t level = 0) const noexcept {
    if (!contains(id) || level >= kMaxQuoteLevels) return nullptr;
    const Handle<Order> h = e_->quote_manager().slot_handle(id, side, level);
    const Oms& oms = e_->oms();
    return h.valid() && oms.is_live(h) ? &oms.get(h) : nullptr;
  }

  // ---- direct orders (risk-checked, journaled) ------------------------------------------------

  // RejectReason::InvalidTag: user_tag lies in the QuoteManager's tag range.
  [[nodiscard]] Result<ClientOrderId, RejectReason> send(const NewOrderRequest& req) noexcept {
    e_->mark_decision();
    if (FASTMM_UNLIKELY(QuoteManager::is_quote_tag(req.user_tag))) {
      return fail(RejectReason::InvalidTag);
    }
    return e_->send_order(req);
  }
  [[nodiscard]] Result<void, RejectReason> cancel(ClientOrderId id) noexcept {
    e_->mark_decision();
    return e_->cancel_order(id);
  }
  [[nodiscard]] Result<void, RejectReason> replace(ClientOrderId id, Price px, Qty qty) noexcept {
    e_->mark_decision();
    return e_->replace_order(id, px, qty);
  }
  // An open order by id, or nullptr once it is terminal. Valid until the next context call.
  [[nodiscard]] const Order* order(ClientOrderId id) const noexcept {
    const Oms& oms = e_->oms();
    const Handle<Order> h = oms.find(id);
    return h.valid() ? &oms.get(h) : nullptr;
  }
  // Estimated quantity resting ahead of an open order at its price: the l2_queue fill model's
  // queue model on the market data the strategy sees, with [engine] queue_conservatism, from the
  // order's ack. nullopt while it is not acknowledged or once it is terminal. The engine tracks
  // queues from the first call on (call it in on_start to cover every order from its ack); an
  // order already resting at the first call starts at the back of its level as the book shows it.
  [[nodiscard]] std::optional<Qty> queue_ahead(ClientOrderId id) const noexcept {
    return e_->queue_ahead(id);
  }
  // Send and ack times of an open order, or nullptr (OmsUpdate::times has them in on_order_update
  // and, through Fill::update, in on_fill).
  [[nodiscard]] const OrderTimes* order_times(ClientOrderId id) const noexcept {
    const Oms& oms = e_->oms();
    const Handle<Order> h = oms.find(id);
    return h.valid() ? &oms.times(h) : nullptr;
  }
  // Unfilled quantity of our open orders (quotes included) on one side.
  [[nodiscard]] Qty open_qty(InstrumentId id, Side side) const noexcept {
    return e_->oms().open_qty(id, side);
  }
  [[nodiscard]] const Oms& oms() const noexcept { return e_->oms(); }

  // ---- timers: journaled when they fire, so replay reproduces them ---------------------------

  [[nodiscard]] TimerId every(Duration period, std::uint64_t tag = 0) noexcept {
    return e_->add_timer(period, true, tag);
  }
  [[nodiscard]] TimerId once(Duration delay, std::uint64_t tag = 0) noexcept {
    return e_->add_timer(delay, false, tag);
  }
  bool cancel_timer(TimerId id) noexcept { return e_->cancel_timer(id); }

  // ---- control --------------------------------------------------------------------------------

  [[nodiscard]] bool quoting_enabled() const noexcept { return e_->quoting_enabled(); }
  // A venue's orders and position are being reconciled, or have not been yet since the session
  // started (a live session waits for every venue that replays executions; new orders are refused
  // with NotReconciled until then). Positions may be missing fills while this holds.
  [[nodiscard]] bool reconciling() const noexcept { return e_->reconciling(); }
  [[nodiscard]] bool killed() const noexcept { return e_->risk().killed(); }
  // One venue's kill switch: new orders to it are refused and set_quotes ignores its instruments.
  [[nodiscard]] bool venue_killed(VenueId v) const noexcept { return e_->risk().venue_killed(v); }
  void request_stop() noexcept { e_->stop(); }
  // Asks for one on_batch_end call once the engine has handled every event that is waiting now
  // (the feed is empty or the step's max_events_per_step is used up). Work a burst of events would
  // each trigger (a requote after every balance report) is done once, after the burst. Repeated
  // requests before the call make one call.
  void request_batch_end() noexcept { e_->request_batch_end(); }
  // Trips the global kill switch: quoting stops, quotes are pulled and every working order is
  // cancelled. The adapter for Python hot hooks uses KillReason::StrategyError.
  void trip_kill(KillReason reason) noexcept { e_->trip_kill(reason); }

  // ---- venue state: fees, risk headroom, venue health -------------------------------------------

  // Maker and taker rates of the instrument, in cbps of the notional (positive: a fee). The
  // configuration's ([[instruments]] maker_bps / taker_bps, else [venues.<x>.fees]), or the
  // account's own where the connector fetched them (fetch_fees). A backtest charges these.
  [[nodiscard]] const FeeRates& fees(InstrumentId id) const noexcept { return e_->fees(id); }
  // What each [risk] limit still admits on the instrument now: the next check uses the same
  // inputs, so an order of exactly a room passes that limit. RiskHeadroom::kUnlimited (or the
  // type's max()) where the limit is off.
  [[nodiscard]] RiskHeadroom risk_headroom(InstrumentId id) const noexcept {
    return e_->risk_headroom(id);
  }
  // Feed lag and order round trip of a venue (core/venue_health.hpp), and whether the feed-lag
  // gate ([risk] max_feed_lag_ms) holds it now.
  [[nodiscard]] VenueHealthView venue_health(VenueId v) const noexcept {
    return e_->venue_health(v);
  }
  // What the [risk] rate limit and the venue's own limits still admit (core/order_budget.hpp):
  // the local token bucket, and the venue's order counts and request weight by window as its
  // connector last published them (live only; venue_known is false elsewhere).
  [[nodiscard]] OrderBudget order_budget(VenueId v) const noexcept { return e_->order_budget(v); }

  // ---- balances (core/balance_book.hpp) ---------------------------------------------------------

  // One asset of a venue's account: the venue's last report (on_balance) less what this engine's
  // orders hold and its fills moved since. Balance::known is false until the venue reports it, and
  // for an asset no instrument of the venue names.
  [[nodiscard]] Balance balance(VenueId v, std::string_view asset) const noexcept {
    return e_->balance(v, asset);
  }
  // The venue's margin: its account-wide margin where it reports one, else the settlement asset of
  // its first derivative.
  [[nodiscard]] Margin margin(VenueId v) const noexcept { return e_->margin(v); }
  // A venue has reported balances (before that, balance() is unknown everywhere).
  [[nodiscard]] bool balances_live() const noexcept { return e_->balances_live(); }
  // Largest quantity of `id` on `side` at `px` the balance covers (a spot buy's quote with the
  // taker fee, a spot sell's base, a derivative's initial margin), rounded down to the lot.
  // Qty::max() while the venue has not reported the balance that side draws on.
  [[nodiscard]] Qty balance_room(InstrumentId id, Side side, Price px) const noexcept {
    return e_->balance_room(id, side, px);
  }
  // ... on one account of the instrument's pool (its own venue when `account` is not one).
  [[nodiscard]] Qty balance_room(InstrumentId id,
                                 Side side,
                                 Price px,
                                 VenueId account) const noexcept {
    return e_->balance_room(id, side, px, account);
  }

  // ---- account pools (core/account_pool.hpp) ---------------------------------------------------

  // The accounts that take orders for a venue's instruments ([venues.<x>] pool_of), the primary
  // first; just the venue itself without a pool. Each has its own balance(), margin() and
  // order_budget(); an order goes to the one NewOrderRequest::account names, else to the usable
  // member whose balance covers it with the most order window left, and Order::venue keeps it.
  [[nodiscard]] PoolMembers pool(VenueId primary) const noexcept { return e_->pool(primary); }
  // The venue whose instruments `v` takes orders for: its primary, or `v` itself.
  [[nodiscard]] VenueId pool_primary(VenueId v) const noexcept { return e_->pool_primary(v); }
  // The automatic routing would send to `v` now: its order link is live and its kill switch is
  // not engaged (its balance and window are the order's to decide).
  [[nodiscard]] bool account_usable(VenueId v) const noexcept { return e_->account_usable(v); }

  // ---- perpetuals (core/perp_book.hpp)
  // -----------------------------------------------------------

  // The venue's mark and index price of a derivative (on_perp_state), with the time each arrived.
  // `stale`: older than [accounting] stale_mark_ms, or never reported; usable() is neither.
  [[nodiscard]] RefPrice mark(InstrumentId id) const noexcept { return e_->mark(id); }
  [[nodiscard]] RefPrice index(InstrumentId id) const noexcept { return e_->index(id); }
  // The venue's funding of a perpetual: the rate it will apply at `next`, per `interval`, and
  // over(d), the rate over a holding time d. `stale` against [accounting] stale_funding_ms.
  [[nodiscard]] FundingView funding(InstrumentId id) const noexcept { return e_->funding(id); }
  // Every field as last reported, open interest included, without the staleness applied.
  [[nodiscard]] const PerpRow& perp_state(InstrumentId id) const noexcept {
    return e_->perps().row(id);
  }

  // ---- randomness: seeded from EngineConfig::rng_seed, replay-deterministic --------------------

  [[nodiscard]] Xoshiro256ss& rng() noexcept { return e_->rng(); }

 private:
  Engine* e_;
};

}  // namespace fastmm
