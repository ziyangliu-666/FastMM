#pragma once
// SimAccounts: the account the simulated venue keeps for the strategy, per venue and asset.
//
// A venue has an account when SimTransportConfig::accounts names it; the other venues accept every
// order as before. What the account does is what a spot exchange or a derivatives venue does with
// its own ledger, and it answers with the messages a live venue sends (sim_transport.hpp):
//
//   spot         a buy holds its notional at the order price in the quote asset (a market buy at
//                the venue's opposite touch), a sell its quantity in the base asset. A fill
//                releases that part of the hold and moves the assets: a buy adds the base and takes
//                the notional at the fill price plus the fee from the quote; a sell the reverse.
//   derivatives  one row per settlement asset: total is the wallet, locked the initial margin
//                (notional x [[instruments]] initial_margin): the open position at its entry price,
//                plus the larger side of the instrument's orders. A reduce-only order, or one that
//                only takes the position towards zero, holds nothing. Closing realises PnL into the
//                wallet; fees come out of it. The open positions' unrealised PnL at their mark
//                (mark()) counts in the row's free amount and equity, as on Binance USD-M
//                (available balance = wallet + unrealised PnL - initial margin).
//
// admit() is the venue's balance check: an order whose hold (less what the order it replaces held)
// is above the row's free amount is refused (RejectReason::InsufficientBalance). A derivative
// without an initial margin rate passes while the row's free amount is not negative.
//
// Every change marks its rows; publish() hands each row whose amounts moved since it was last
// published to a callback as a BalanceMsg (a Binance outboundAccountPosition), snapshot() every row
// of a venue flagged as one snapshot. Nothing allocates after construction.
//
// Account pools (core/account_pool.hpp): a member venue's account holds the rows and the
// per-instrument state (positions, holds) of its primary's instruments once more; the order calls
// name the venue the order went to (`venue`, invalid: the instrument's own).
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm::sim {

// One asset's starting amount on a simulated venue (the total: nothing is locked at the start).
struct SimBalance {
  FixedString<8> asset;
  Notional amount;
};

// The account of one simulated venue ([backtest.balances], [backtest.venues.<name>.balances], or a
// journal's first balance snapshot). Assets its instruments use and it does not name start at 0.
struct SimAccountConfig {
  VenueId venue{};
  std::vector<SimBalance> balances;
};

struct SimAccountStats {
  std::uint64_t refused = 0;    // orders admit() refused
  std::uint64_t published = 0;  // BalanceMsg handed to publish() / snapshot() callbacks
};

class SimAccounts {
 public:
  static constexpr std::size_t kMaxOrders = 1U << 14;  // open orders of the strategy, all venues

  // `initial_margin`: per instrument id, the derivative's margin rate (may be shorter than the
  // table: missing rates are 0). `pools`: the account pools, whose members trade their primary's
  // instruments. Throws std::invalid_argument for an account on a venue no instrument trades (nor
  // its pool) or an asset named twice.
  SimAccounts(const InstrumentTable& instruments,
              std::span<const SimAccountConfig> accounts,
              std::span<const Ratio> initial_margin,
              const PoolPlan* pools = nullptr);
  SimAccounts(const SimAccounts&) = delete;
  SimAccounts& operator=(const SimAccounts&) = delete;

  [[nodiscard]] bool enabled(VenueId v) const noexcept {
    return v.value < kMaxVenues && venue_on_[v.value];
  }
  // The instrument's own venue has an account.
  [[nodiscard]] bool enabled(InstrumentId id) const noexcept {
    return id.value < n_inst_ && slot_for(instruments_.get(id).venue) != kNoSlot;
  }

  // The venue checks a new order (or the new leg of a replace of `replaces`) at `px` for `qty`; a
  // market order is held at the venue's opposite touch, which the caller passes as `px`. True: the
  // order is accepted and holds its amount under `id` until fill() takes its last quantity or
  // close() ends it. `venue`: the account it goes to (a pool member; invalid: the instrument's
  // own venue). A venue without an account admits everything and records nothing.
  bool admit(ClientOrderId id,
             InstrumentId inst,
             Side side,
             Price px,
             Qty qty,
             bool reduce_only,
             ClientOrderId replaces = {},
             VenueId venue = {}) noexcept;
  // The new leg `id` of a replace of `orig` at `px` for `qty`: admit() with `orig`'s instrument and
  // side. True when the account does not know `orig` (the venue answers that itself).
  bool admit_replace(ClientOrderId orig, ClientOrderId id, Price px, Qty qty) noexcept;
  // `qty` of order `id` filled at `px` with `leaves` left, paying `fee` (quote asset for spot, the
  // settlement asset for a derivative).
  void fill(ClientOrderId id, Price px, Qty qty, Qty leaves, Notional fee) noexcept;
  // The order ended (cancel, expiry, reject): its hold goes back to free.
  void close(ClientOrderId id) noexcept;
  // A derivative's mark price: the venue's (`venue`: a mark price stream), or the book's mid, which
  // is used only while the venue has sent none. Every account of the instrument's pool takes it.
  void mark(InstrumentId id, Price px, bool venue) noexcept {
    if (!enabled(id) || !px.is_positive()) return;
    for (std::size_t k = 0; k < n_slots_; ++k) {
      Inst& in = at(k, id);
      if (!in.on || !in.derivative || (in.venue_mark && !venue)) continue;
      in.mark = px;
      in.venue_mark = in.venue_mark || venue;
    }
  }
  [[nodiscard]] bool derivative(InstrumentId id) const noexcept {
    return enabled(id) && at(slot_for(instruments_.get(id).venue), id).derivative;
  }

  // Each row of `venue` as one snapshot (kSnapshot, the last one also kSnapshotEnd), stamped `ts`.
  template <class F>
  void snapshot(VenueId venue, Timestamp ts, F emit) noexcept {
    std::size_t n = 0;
    for (const Row& r : rows_) n += r.venue == venue ? 1U : 0U;
    std::size_t k = 0;
    for (Row& r : rows_) {
      if (r.venue != venue) continue;
      BalanceMsg m = message(r, ts);
      m.flags = static_cast<std::uint8_t>(BalanceMsg::kSnapshot |
                                          (++k == n ? BalanceMsg::kSnapshotEnd : 0U));
      r.pub_total = r.total;
      r.pub_locked = r.locked;
      ++stats_.published;
      emit(m);
    }
  }
  // Each row of `venue` whose amounts changed since it was last published, stamped `ts`.
  template <class F>
  void publish(VenueId venue, Timestamp ts, F emit) noexcept {
    if (!enabled(venue) || !dirty_[venue.value]) return;
    dirty_[venue.value] = false;
    for (Row& r : rows_) {
      if (r.venue != venue || (r.total == r.pub_total && r.locked == r.pub_locked)) continue;
      r.pub_total = r.total;
      r.pub_locked = r.locked;
      ++stats_.published;
      BalanceMsg m = message(r, ts);
      emit(m);
    }
  }

  // An internal transfer between two accounts of one pool (Venue::transfer): `amount` of `asset`
  // leaves `from`'s total and joins `to`'s at once. False, nothing moved, when either venue has no
  // account or no row of the asset, the amount is not positive, or it is more than `from` has free.
  // The rows are published with the next publish() of each venue.
  bool transfer(VenueId from, VenueId to, std::string_view asset, Notional amount) noexcept;

  // The venue's amounts (raw, in the asset); zero for a row it does not keep.
  [[nodiscard]] Notional free(VenueId venue, std::string_view asset) const noexcept;
  [[nodiscard]] Notional locked(VenueId venue, std::string_view asset) const noexcept;
  [[nodiscard]] Notional total(VenueId venue, std::string_view asset) const noexcept;
  // The open positions' unrealised PnL at their marks, in the asset (zero for a spot asset).
  [[nodiscard]] Notional unrealized(VenueId venue, std::string_view asset) const noexcept;
  // A derivative's position at the venue (contracts, signed), over every account of its pool.
  [[nodiscard]] Qty position(InstrumentId id) const noexcept;
  [[nodiscard]] std::size_t open_orders() const noexcept { return holds_.size(); }
  [[nodiscard]] const SimAccountStats& stats() const noexcept { return stats_; }
  // Counts an order the caller refused on admit()'s answer.
  void count_refused() noexcept { ++stats_.refused; }

 private:
  static constexpr std::uint16_t kNone = 0xFFFF;
  struct Row {
    VenueId venue{};
    FixedString<8> asset{};
    std::int64_t total = 0;  // raw amounts in the asset; free = total - locked
    std::int64_t locked = 0;
    std::int64_t pub_total = 0;  // as last published
    std::int64_t pub_locked = 0;
  };
  struct Inst {
    bool on = false;
    bool derivative = false;
    std::uint16_t base = kNone;
    std::uint16_t quote = kNone;
    std::uint16_t settle = kNone;
    std::int64_t im_raw = 0;
    std::int64_t position = 0;  // raw contracts, signed (derivatives)
    Price entry{};              // average entry price of the position
    std::int64_t position_margin = 0;
    std::array<std::int64_t, 2> side_hold{};  // sum of the orders' holds per side (derivatives)
    Price mark{};                             // zero: none yet (no unrealised PnL)
    bool venue_mark = false;                  // `mark` is the venue's, not the book's mid
  };
  struct Hold {
    InstrumentId inst{};
    VenueId venue{};        // the account it is held in ...
    std::uint8_t slot = 0;  // ... and its slot (slot_for)
    Side side = Side::Buy;
    Price price{};
    Qty leaves{};
    std::int64_t amount = 0;  // raw, in the row's asset
    bool reduce_only = false;
  };

  [[nodiscard]] std::int64_t hold_of(const Inst& in,
                                     const Instrument& i,
                                     Side side,
                                     Price px,
                                     Qty qty,
                                     bool reduces) const noexcept;
  // A derivative's row locked = sum over its instruments of position margin + larger order side.
  void relock(std::uint16_t row) noexcept;
  // Unrealised PnL of the positions settling in `row` at their marks (raw, in the row's asset).
  [[nodiscard]] std::int64_t upnl(std::uint16_t row) const noexcept;
  void touch(std::uint16_t row) noexcept;
  [[nodiscard]] std::uint16_t find(VenueId venue, std::string_view asset) const noexcept;
  std::uint16_t row_for(VenueId venue, std::string_view asset);
  [[nodiscard]] BalanceMsg message(const Row& r, Timestamp ts) const noexcept;

  static constexpr std::uint8_t kNoSlot = 0xFF;
  // The account of `venue`'s slot; kNoSlot without one.
  [[nodiscard]] std::uint8_t slot_for(VenueId venue) const noexcept {
    return venue.value < kMaxVenues ? slot_of_venue_[venue.value] : kNoSlot;
  }
  // The state of instrument `id` in account slot `k`.
  [[nodiscard]] Inst& at(std::size_t k, InstrumentId id) noexcept {
    return inst_[k * n_inst_ + id.value];
  }
  [[nodiscard]] const Inst& at(std::size_t k, InstrumentId id) const noexcept {
    return inst_[k * n_inst_ + id.value];
  }

  const InstrumentTable& instruments_;
  std::vector<Row> rows_;
  // Per account slot, per instrument: inst_[slot * n_inst_ + id] (at()).
  std::vector<Inst> inst_;
  std::size_t n_inst_ = 0;
  std::size_t n_slots_ = 0;
  std::array<std::uint8_t, kMaxVenues> slot_of_venue_{};
  std::array<bool, kMaxVenues> venue_on_{};
  std::array<bool, kMaxVenues> dirty_{};
  OpenHashMap<std::uint64_t, Hold, kMaxOrders> holds_;
  SimAccountStats stats_{};
};

}  // namespace fastmm::sim
