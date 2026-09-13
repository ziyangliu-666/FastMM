#pragma once
// StrategyContext: the API a strategy sees (5.6). A thin, non-owning facade over the
// Engine so strategies never include engine.hpp and cannot reach into internals.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
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

  [[nodiscard]] Timestamp now() const noexcept { return e_->now(); }
  [[nodiscard]] const Book& book(InstrumentId id) const noexcept { return e_->book(id); }
  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return e_->position(id);
  }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const noexcept {
    return e_->instrument(id);
  }
  [[nodiscard]] const InstrumentTable& instruments() const noexcept { return e_->instruments(); }
  [[nodiscard]] std::size_t instrument_count() const noexcept { return e_->instruments().size(); }

  // Direct order API (risk-checked, journaled).
  [[nodiscard]] Result<ClientOrderId, RejectReason> send(const NewOrderRequest& req) noexcept {
    return e_->send_order(req);
  }
  [[nodiscard]] Result<void, RejectReason> cancel(ClientOrderId id) noexcept {
    return e_->cancel_order(id);
  }
  [[nodiscard]] Result<void, RejectReason> replace(ClientOrderId id, Price px, Qty qty) noexcept {
    return e_->replace_order(id, px, qty);
  }
  // Quote ladder API (QuoteManager does the diffing).
  void set_quotes(InstrumentId id, const DesiredQuotes& q) noexcept { e_->set_quotes(id, q); }
  void pull_quotes(InstrumentId id) noexcept { e_->pull_quotes(id); }
  void pull_all_quotes() noexcept { e_->pull_all_quotes(); }

  [[nodiscard]] TimerId add_timer(Duration period,
                                  bool repeat,
                                  std::uint64_t user_data = 0) noexcept {
    return e_->add_timer(period, repeat, user_data);
  }
  bool cancel_timer(TimerId id) noexcept { return e_->cancel_timer(id); }

  [[nodiscard]] Xoshiro256ss& rng() noexcept { return e_->rng(); }
  [[nodiscard]] const Oms& oms() const noexcept { return e_->oms(); }
  [[nodiscard]] bool quoting_enabled() const noexcept { return e_->quoting_enabled(); }
  [[nodiscard]] bool killed() const noexcept { return e_->risk().killed(); }
  void request_stop() noexcept { e_->stop(); }

 private:
  Engine* e_;
};

}  // namespace fastmm
