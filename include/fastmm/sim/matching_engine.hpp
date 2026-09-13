#pragma once
// MatchingEngine (8.1): a deterministic, allocation-free continuous double auction.
//
//   * price-time priority; every instrument has a SimBook of price levels, each an
//     intrusive FIFO of SimOrders drawn from one Pool (no heap after construction);
//   * Limit / Market / PostOnly (rejected when it would cross == Binance LIMIT_MAKER);
//   * GTC / IOC / FOK (FOK is pre-checked against resting liquidity and expires whole);
//   * cancel / replace (replace == cancel + new; queue priority is kept only when the
//     price is unchanged and the new quantity does not exceed the current leaves);
//   * self-trade prevention per account: None | CancelTaker | CancelMaker | CancelBoth;
//   * per-instrument update_id incremented on every level mutation, reported through the
//     Sink so an aggregator can build Binance-style depthUpdate batches (U / u).
//
// All randomness lives in the caller; the engine is a pure function of its inputs.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/sim_book.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace fastmm::sim {

struct NewOrder {
  AccountId account = kStrategyAccount;
  ClientOrderId cl_ord_id{};
  InstrumentId instrument{};
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price{};  // ignored for Market
  Qty qty{};
};

enum class StpMode : std::uint8_t { None = 0, CancelTaker = 1, CancelMaker = 2, CancelBoth = 3 };

enum class CancelReason : std::uint8_t {
  Requested = 0,    // explicit cancel()
  Replaced = 1,     // old leg of a replace
  Ioc = 2,          // IOC remainder expired
  Fok = 3,          // FOK could not be filled whole
  NoLiquidity = 4,  // market order remainder, book exhausted
  Stp = 5,          // self-trade prevention
};
[[nodiscard]] constexpr bool is_expiry(CancelReason r) noexcept {
  return r == CancelReason::Ioc || r == CancelReason::Fok || r == CancelReason::NoLiquidity;
}

// Every observable effect of the engine is reported here (venue -> world). Default no-ops
// so tests and benchmarks can subscribe to just what they need.
class MatchingSink {
 public:
  virtual ~MatchingSink() = default;
  virtual void on_ack(const SimOrder&, Timestamp) {}
  virtual void on_reject(const NewOrder&, RejectReason, Timestamp) {}
  virtual void on_cancel(const SimOrder&, CancelReason, Timestamp) {}
  virtual void on_cancel_reject(AccountId, ClientOrderId, InstrumentId, Timestamp) {}
  virtual void on_fill(
      const SimOrder& /*maker*/, const SimOrder& /*taker*/, Price, Qty, Timestamp) {}
  virtual void on_book_change(InstrumentId, Side, Price, Qty /*new level qty*/, std::uint64_t) {}
  virtual void on_trade(
      InstrumentId, Price, Qty, Side /*aggressor*/, std::uint64_t /*trade_id*/, Timestamp) {}
};

struct SubmitResult {
  std::uint64_t order_id = 0;                // 0 when rejected
  RejectReason reason = RejectReason::None;  // None == accepted
  Qty filled{};                              // executed on arrival
  Qty resting{};                             // left on the book (0 for IOC/FOK/market)
  [[nodiscard]] bool accepted() const noexcept { return reason == RejectReason::None; }
};

// Per-account fill totals; the backtest reconciles the strategy's positions against them.
struct AccountLedger {
  Qty bought{};
  Qty sold{};
  Notional buy_notional{};
  Notional sell_notional{};
  std::uint64_t fills = 0;
  [[nodiscard]] Qty net() const noexcept { return bought - sold; }
};

struct MatchingStats {
  std::uint64_t submits = 0;
  std::uint64_t rejects = 0;
  std::uint64_t cancels = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t replaces = 0;
  std::uint64_t fills = 0;
  std::uint64_t trades = 0;
  std::uint64_t stp_cancels = 0;
  std::uint64_t expired = 0;
  std::uint64_t pool_exhausted = 0;
  std::uint64_t level_full = 0;
};

class MatchingEngine {
 public:
  using OrderPool = Pool<SimOrder, kMaxSimOrders>;

  explicit MatchingEngine(std::size_t instrument_count, MatchingSink* sink = nullptr);
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  void set_sink(MatchingSink* s) noexcept { sink_ = s; }
  void set_stp(AccountId account, StpMode mode) noexcept;
  [[nodiscard]] StpMode stp(AccountId account) const noexcept;

  // ---- order entry ------------------------------------------------------------------------
  SubmitResult submit(const NewOrder& o, Timestamp now) noexcept;
  // false (and Sink::on_cancel_reject) when the order is unknown.
  bool cancel(AccountId account, ClientOrderId id, Timestamp now) noexcept;
  // Cancel + new with priority kept when price is unchanged and qty <= leaves. The new leg
  // inherits side/type/tif and gets `new_id`. Unknown orig -> cancel reject + reject.
  SubmitResult replace(AccountId account,
                       ClientOrderId orig,
                       ClientOrderId new_id,
                       Price price,
                       Qty qty,
                       Timestamp now) noexcept;

  // ---- queries ----------------------------------------------------------------------------
  [[nodiscard]] const SimOrder* find(AccountId account, ClientOrderId id) const noexcept;
  [[nodiscard]] const SimBook& book(InstrumentId id) const noexcept { return books_[id.value]; }
  [[nodiscard]] std::uint64_t update_id(InstrumentId id) const noexcept {
    return books_[id.value].update_id();
  }
  [[nodiscard]] std::size_t l2_snapshot(InstrumentId id,
                                        Side side,
                                        Level* out,
                                        std::size_t max_levels) const noexcept {
    return books_[id.value].snapshot(side, out, max_levels);
  }
  struct TopOfBook {
    Level bid;
    Level ask;
  };
  [[nodiscard]] TopOfBook top_of_book(InstrumentId id) const noexcept {
    const SimBook& b = books_[id.value];
    return {b.best_level(Side::Buy), b.best_level(Side::Sell)};
  }
  [[nodiscard]] Qty level_qty(InstrumentId id, Side side, Price px) const noexcept {
    const PriceLevel* l = books_[id.value].find(side, px);
    return l == nullptr ? Qty{} : l->qty;
  }
  [[nodiscard]] std::size_t instrument_count() const noexcept { return instrument_count_; }
  [[nodiscard]] std::size_t open_orders() const noexcept { return pool_.size(); }
  [[nodiscard]] const AccountLedger& ledger(AccountId a) const noexcept {
    return ledgers_[a < kMaxAccounts ? a : kMaxAccounts - 1];
  }
  [[nodiscard]] const MatchingStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint64_t next_order_id() const noexcept { return next_order_id_; }
  // Ascending handle order (deterministic). F(const SimOrder&).
  template <class F>
  void for_each_open_order(F&& f) const noexcept {
    pool_.for_each([&](Handle<SimOrder>, const SimOrder& o) { f(o); });
  }
  // Open orders of one account on one instrument/side: count and total leaves.
  struct SideExposure {
    std::uint32_t orders = 0;
    Qty leaves{};
    Price best{};
  };
  [[nodiscard]] SideExposure exposure(AccountId account, InstrumentId id, Side side) const noexcept;
  // Leaves of `account`'s orders resting at exactly (id, side, px).
  [[nodiscard]] Qty account_qty_at(AccountId account,
                                   InstrumentId id,
                                   Side side,
                                   Price px) const noexcept;
  // Removes `by` from `account`'s orders at (id, side, px), newest first (the last order may
  // be reduced in place). Used to mirror historical level decreases into the book.
  void reduce_account_qty(
      AccountId account, InstrumentId id, Side side, Price px, Qty by, Timestamp now) noexcept;

 private:
  using Handle32 = Handle<SimOrder>;
  [[nodiscard]] static constexpr std::uint64_t key(AccountId a, ClientOrderId id) noexcept {
    return (static_cast<std::uint64_t>(a) << 48) | (id.value & 0xFFFF'FFFF'FFFFULL);
  }
  [[nodiscard]] static constexpr bool crosses(Side taker, Price px, Price maker_px) noexcept {
    return taker == Side::Buy ? px >= maker_px : px <= maker_px;
  }

  // Sweeps the opposite side until the order is done or nothing more crosses. Returns
  // false when STP cancelled (and freed) the taker; `filled` is its executed quantity.
  bool match(Handle32 th, SimOrder& taker, Timestamp now, Qty& filled) noexcept;
  void rest(Handle32 h, SimOrder& o) noexcept;
  void unlink(SimOrder& o, PriceLevel& l) noexcept;
  void remove_resting(Handle32 h, SimOrder& o, CancelReason why, Timestamp now) noexcept;
  void book_changed(InstrumentId id, Side side, const PriceLevel& l) noexcept;
  void book_level_gone(InstrumentId id, Side side, Price px) noexcept;
  void record_fill(const SimOrder& maker, const SimOrder& taker, Price px, Qty qty) noexcept;
  void free_order(Handle32 h, const SimOrder& o) noexcept;

  std::size_t instrument_count_;
  MatchingSink* sink_;
  std::unique_ptr<SimBook[]> books_;
  OrderPool pool_;
  OpenHashMap<std::uint64_t, Handle32, kMaxSimOrders * 2> by_key_;
  StpMode stp_[kMaxAccounts] = {};
  AccountLedger ledgers_[kMaxAccounts] = {};
  MatchingStats stats_{};
  std::uint64_t next_order_id_ = 1;
  std::uint64_t next_trade_id_ = 1;
};

}  // namespace fastmm::sim
