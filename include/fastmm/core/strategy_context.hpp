#pragma once
// StrategyContext: the API a strategy sees (ADR-0012, section 2). A thin, non-owning facade over
// the Engine, so strategies never include engine.hpp and cannot reach into internals. Every
// order-API call marks the strategy's decision time for the serialize latency interval.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>

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
  [[nodiscard]] bool killed() const noexcept { return e_->risk().killed(); }
  void request_stop() noexcept { e_->stop(); }

  // ---- randomness: seeded from EngineConfig::rng_seed, replay-deterministic --------------------

  [[nodiscard]] Xoshiro256ss& rng() noexcept { return e_->rng(); }

 private:
  Engine* e_;
};

}  // namespace fastmm
