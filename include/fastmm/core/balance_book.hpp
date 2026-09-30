#pragma once
// BalanceBook: the account's balances per venue and asset (BalanceMsg), and what the engine's own
// orders and fills did to them since the venue last reported. The engine keeps one for its own
// orders; fastmm-gateway keeps one per venue for the orders of every attached strategy.
//
// Rows: one per (venue, asset) a spot instrument names as base or quote or a derivative settles
// in, resolved when the table is built, and one account row per venue with derivatives
// (BalanceMsg::kAccount). A report for
// another asset is not kept (counted). Nothing here allocates after build().
//
// The estimate. A venue report is the account as of its venue time (hdr.exch_ts). It counts the
// orders the venue had acknowledged; an order still waiting for its ack keeps its hold on top of
// the report, until its ack says whether the report had it (stamped at or before the report's
// time). From there each of our own events moves the estimate, unless the venue stamped the event
// at or before the report's time (the report has it already); the events of an order the venue has
// not acknowledged always count:
//   * an order holds part of a row: a spot buy its notional plus the taker fee in the quote asset,
//     a spot sell its quantity in the base asset, a derivative its initial margin (notional times
//     [[instruments]] initial_margin; reduce-only orders hold nothing) in its margin row. A
//     derivative's buys and sells on one instrument net: the larger side holds. Holding moves an
//     amount from free to locked; an order's end, fill or amend moves it back or changes it.
//   * a fill moves the assets: a spot buy adds the base (less a base-asset fee) and takes the
//     notional (and a quote-asset fee) from the quote; a sell the reverse. A derivative fill moves
//     the position's initial margin at the fill price into locked, and takes the fee.
// The next report replaces the estimate. What it can miss: an event that reaches the engine after a
// report that already counted it and carries no venue time, and an ack without a venue time (its
// hold stays counted twice until the next report).
//
// The pre-trade check (covers): what the order adds to its row's holds (its hold less what the
// order it replaces held; for a derivative, what it adds to the larger side) must not exceed the
// row's free estimate, and a derivative needs a free estimate of zero or more. A derivative order
// that is reduce-only or reduces its position passes; one on an instrument without an initial
// margin rate passes while the available margin is positive. A row the venue has not reported
// does not refuse anything.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace fastmm {

// [risk] check_balance and the initial margin rate of each instrument ([[instruments]]
// initial_margin, a fraction of the notional; zero: none configured).
struct BalanceConfig {
  bool check = true;
  std::array<Ratio, kMaxInstruments> initial_margin{};
};

// One asset on one venue now (BalanceBook::balance, StrategyContext::balance). free, locked and
// total are the estimate; equity and maintenance are the venue's last report.
struct Balance {
  Notional free;
  Notional locked;
  Notional total;
  Notional equity;
  Notional maintenance;
  Timestamp as_of;     // venue time of the last report
  bool known = false;  // the venue has reported it
};

// A venue's margin (StrategyContext::margin): its account row when the venue reports one, else the
// row of the settlement asset of its first derivative.
struct Margin {
  Notional available;  // Balance::free
  Notional initial;    // Balance::locked
  Notional maintenance;
  Notional equity;
  Notional wallet;  // Balance::total
  FixedString<8> asset;
  Timestamp as_of;
  bool account = false;  // the account's cross-collateral margin (BalanceMsg::kAccount)
  bool known = false;
};

struct BalanceStats {
  std::uint64_t reports = 0;    // BalanceMsg applied
  std::uint64_t untracked = 0;  // ... naming an asset no instrument of the venue uses
  std::uint64_t snapshots = 0;  // snapshots ended (kSnapshotEnd)
};

class BalanceBook {
 public:
  static constexpr std::uint16_t kNone = 0xFFFF;
  static constexpr std::size_t kMaxVenueSlots = 256;

  // One row as the status file and the monitors read it.
  struct Row {
    VenueId venue{};
    FixedString<8> asset{};
    bool account = false;
    bool reported = false;
    std::int64_t free = 0;  // raw amounts in the asset
    std::int64_t locked = 0;
    std::int64_t total = 0;
    std::int64_t equity = 0;
    std::int64_t maintenance = 0;
    Timestamp as_of{};
    std::uint32_t gen = 0;  // the venue's snapshot that last named it
  };

  // Builds the rows from the instruments. `fees`: the taker rate a spot buy holds on top of its
  // notional. Allocates; call before the session runs.
  void build(const InstrumentTable& insts,
             const FeeTable& fees = {},
             const BalanceConfig& cfg = {}) {
    rows_.clear();
    rows_.reserve(2 * insts.size() + 16);
    inst_.fill(Inst{});
    for (VenueState& v : venues_) v = VenueState{};
    check_ = cfg.check;
    for (const Instrument& i : insts) {
      if (i.id.value >= kMaxInstruments) continue;
      Inst& in = inst_[i.id.value];
      in.venue = i.venue;
      in.derivative = i.is_derivative();
      in.taker_cbps = std::max<std::int32_t>(fees.schedule(i.id).taker_cbps, 0);
      in.im_raw = cfg.initial_margin[i.id.value].raw > 0 ? cfg.initial_margin[i.id.value].raw : 0;
      if (!in.derivative) {
        in.base = row_for(i.venue, i.base.view(), false);
        in.quote = row_for(i.venue, i.quote.view(), false);
      } else {
        in.settle = row_for(i.venue, i.settlement_ccy(), false);
        in.margin = in.settle;
        VenueState& vs = venues_[i.venue.value];
        if (vs.account_row == kNone) vs.account_row = add_row(i.venue, "", true);
        if (vs.margin_default == kNone) vs.margin_default = in.settle;
      }
    }
  }

  [[nodiscard]] bool live() const noexcept { return live_; }
  [[nodiscard]] bool check_enabled() const noexcept { return check_; }
  [[nodiscard]] const BalanceStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }
  [[nodiscard]] const Row& row(std::size_t i) const noexcept { return rows_[i]; }

  // A venue report. False when it names an asset the table does not keep (still ends a snapshot).
  bool on_report(const BalanceMsg& m) noexcept {
    live_ = true;
    ++stats_.reports;
    VenueState& vs = venues_[m.hdr.venue.value];
    if ((m.flags & (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd)) != 0 && !vs.in_snapshot) {
      vs.in_snapshot = true;
      ++vs.gen;
    }
    const bool account = (m.flags & BalanceMsg::kAccount) != 0;
    std::uint16_t r = kNone;
    if (account) {
      r = vs.account_row;
    } else if (!m.asset.empty()) {
      r = find(m.hdr.venue, m.asset.view());
    }
    if (r != kNone) {
      Row& row = rows_[r];
      row.free = m.free.raw;
      row.locked = m.locked.raw;
      row.total = m.total.raw;
      row.equity = m.equity.raw;
      row.maintenance = m.maintenance.raw;
      row.as_of = m.hdr.exch_ts;
      row.reported = true;
      row.gen = vs.gen;
      if (account) {
        row.asset = m.asset;
        if (!vs.account_reported) use_account(m.hdr.venue, r);
      }
      add_in_flight(r);
    } else if (!m.asset.empty()) {
      ++stats_.untracked;
    }
    if ((m.flags & BalanceMsg::kSnapshotEnd) != 0) end_snapshot(m.hdr.venue, m.hdr.exch_ts);
    return r != kNone;
  }

  // What an order of `leaves` at `px` holds, in its row's asset (raw); see the header.
  [[nodiscard]] std::int64_t hold(InstrumentId id,
                                  const Instrument& inst,
                                  Side side,
                                  Price px,
                                  Qty leaves,
                                  bool reduce_only) const noexcept {
    if (id.value >= kMaxInstruments || !leaves.is_positive()) return 0;
    const Inst& in = inst_[id.value];
    if (!in.derivative) {
      if (side == Side::Sell) return leaves.raw;
      return with_fee(inst.notional(px, leaves).raw, in.taker_cbps);
    }
    if (reduce_only || in.im_raw == 0) return 0;
    return margin_of(inst.notional(px, leaves).raw, in.im_raw);
  }

  // An order's hold went from `before` to `after` (hold()) at venue time `venue_ts` (invalid: not
  // stamped). `unacked`: the venue has not acknowledged the order, so no report has its hold. An
  // event of an acknowledged order that the row's last report already counted changes nothing.
  void move_hold(InstrumentId id,
                 Side side,
                 std::int64_t before,
                 std::int64_t after,
                 Timestamp venue_ts,
                 bool unacked) noexcept {
    if (id.value >= kMaxInstruments || before == after) return;
    Inst& in = inst_[id.value];
    std::uint16_t r = kNone;
    std::int64_t delta = after - before;
    if (unacked) in.unacked[static_cast<std::size_t>(side)] += delta;
    if (!in.derivative) {
      r = side == Side::Buy ? in.quote : in.base;
    } else {
      const std::int64_t was = std::max(in.side_hold[0], in.side_hold[1]);
      in.side_hold[static_cast<std::size_t>(side)] += delta;
      delta = std::max(in.side_hold[0], in.side_hold[1]) - was;
      r = in.margin;
    }
    if (r == kNone || delta == 0) return;
    Row& row = rows_[r];
    if (!unacked && counted(row, venue_ts)) return;
    row.free -= delta;
    row.locked += delta;
  }

  // The venue acknowledged an order holding `amount`, at venue time `venue_ts`. Its hold was kept
  // on top of the reports since it was sent; one stamped at or before the row's last report had it,
  // and the estimate gives the extra back.
  void acknowledge(InstrumentId id, Side side, std::int64_t amount, Timestamp venue_ts) noexcept {
    if (id.value >= kMaxInstruments || amount == 0) return;
    Inst& in = inst_[id.value];
    const std::uint16_t r = row_of(in, side);
    const std::int64_t before = r == kNone ? 0 : in_flight(in, side, r);
    in.unacked[static_cast<std::size_t>(side)] -= amount;
    if (r == kNone || !counted(rows_[r], venue_ts)) return;
    const std::int64_t back = before - in_flight(in, side, r);
    rows_[r].free += back;
    rows_[r].locked -= back;
  }

  // A fill of `qty` at `px` and what it moved. `pos_before` / `pos_after`: the instrument's
  // position (raw contracts) around it, for a derivative's margin. `fee` in `fee_asset`.
  void on_fill(InstrumentId id,
               const Instrument& inst,
               Side side,
               Price px,
               Qty qty,
               Notional fee,
               FeeAsset fee_asset,
               std::int64_t pos_before,
               std::int64_t pos_after,
               Timestamp venue_ts) noexcept {
    if (id.value >= kMaxInstruments) return;
    const Inst& in = inst_[id.value];
    if (!in.derivative) {
      std::int64_t base = side == Side::Buy ? qty.raw : -qty.raw;
      const std::int64_t n = inst.notional(px, qty).raw;
      std::int64_t quote = side == Side::Buy ? -n : n;
      if (fee_asset == FeeAsset::Base) base -= fee.raw;
      if (fee_asset == FeeAsset::Quote) quote -= fee.raw;
      move_asset(in.base, base, venue_ts);
      move_asset(in.quote, quote, venue_ts);
      return;
    }
    if (in.margin == kNone) return;
    Row& row = rows_[in.margin];
    if (counted(row, venue_ts)) return;
    std::int64_t im = 0;
    if (in.im_raw != 0) {
      const auto abs = [](std::int64_t v) { return v < 0 ? -v : v; };
      im = margin_of(inst.notional(px, Qty::from_raw(abs(pos_after))).raw, in.im_raw) -
           margin_of(inst.notional(px, Qty::from_raw(abs(pos_before))).raw, in.im_raw);
    }
    // The settlement asset: the quote of a linear contract, the base of an inverse one.
    const bool settles =
        inst.inverse() ? fee_asset == FeeAsset::Base : fee_asset == FeeAsset::Quote;
    const std::int64_t f = settles ? fee.raw : 0;
    row.free -= im + f;
    row.locked += im;
    row.total -= f;
    row.equity -= f;
  }

  // Does the balance cover an order of `qty` at `px` on `side`? `replaced_hold`: what the order it
  // replaces holds (0 for a new order). `reduces`: it only takes the position towards zero.
  [[nodiscard]] bool covers(InstrumentId id,
                            const Instrument& inst,
                            Side side,
                            Price px,
                            Qty qty,
                            std::int64_t replaced_hold,
                            bool reduce_only,
                            bool reduces) const noexcept {
    if (id.value >= kMaxInstruments) return true;
    const Inst& in = inst_[id.value];
    const std::uint16_t r = in.derivative ? in.margin : side == Side::Buy ? in.quote : in.base;
    if (r == kNone || !rows_[r].reported) return true;
    if (in.derivative && (reduce_only || reduces)) return true;
    const std::int64_t free = rows_[r].free;
    if (!in.derivative) {
      const std::int64_t need = hold(id, inst, side, px, qty, false) - replaced_hold;
      return need <= 0 || need <= free;
    }
    if (in.im_raw == 0) return free > 0;
    // What the instrument's larger side would add.
    std::array<std::int64_t, 2> sides = in.side_hold;
    sides[static_cast<std::size_t>(side)] += hold(id, inst, side, px, qty, false) - replaced_hold;
    const std::int64_t need =
        std::max(sides[0], sides[1]) - std::max(in.side_hold[0], in.side_hold[1]);
    return need < 0 || (free >= 0 && need <= free);
  }

  // The largest quantity of `id` on `side` at `px` the balance covers, rounded down to the lot;
  // Qty::max() while its row is unreported (or a derivative without a margin rate has margin left).
  [[nodiscard]] Qty room(InstrumentId id,
                         const Instrument& inst,
                         Side side,
                         Price px) const noexcept {
    if (id.value >= kMaxInstruments) return Qty::max();
    const Inst& in = inst_[id.value];
    const std::uint16_t r = in.derivative ? in.margin : side == Side::Buy ? in.quote : in.base;
    if (r == kNone || !rows_[r].reported) return Qty::max();
    const std::int64_t free = rows_[r].free;
    if (free <= 0) return Qty{};
    std::int64_t q = 0;
    if (!in.derivative && side == Side::Sell) {
      q = free;
    } else {
      if (in.derivative && in.im_raw == 0) return Qty::max();
      if (!px.is_positive() || !inst.contract_multiplier.is_positive()) return Qty{};
      // What one unit of quantity holds: its notional (price * multiplier, or multiplier / price
      // for an inverse contract) times (1 + taker fee) or the margin rate. An estimate for sizing,
      // in double; covers() is the exact check.
      const double p = static_cast<double>(px.raw) / kFixedScale;
      const double mult = static_cast<double>(inst.contract_multiplier.raw) / kFixedScale;
      const double unit = inst.inverse() ? mult / p : p * mult;
      const double rate = in.derivative ? static_cast<double>(in.im_raw) / kFixedScale
                                        : 1.0 + static_cast<double>(in.taker_cbps) / 1e6;
      const double per = unit * rate;
      if (!(per > 0.0)) return Qty{};
      const double v = static_cast<double>(free) / per;  // raw quantity
      q = v >= 9.2e18 ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(v);
      // The double may round up across a boundary covers() then refuses: one lot less is safe.
      if (q > 0 && !covers(id,
                           inst,
                           side,
                           px,
                           Qty::from_raw(q - (q % std::max<std::int64_t>(inst.lot.raw, 1))),
                           0,
                           false,
                           false))
        q -= std::max<std::int64_t>(inst.lot.raw, 1);
    }
    if (inst.lot.is_positive()) q -= q % inst.lot.raw;
    return Qty::from_raw(q);
  }

  // One asset of a venue; unknown for an asset the table does not keep or the venue has not
  // reported.
  [[nodiscard]] Balance balance(VenueId venue, std::string_view asset) const noexcept {
    const std::uint16_t r = find(venue, asset);
    return r == kNone ? Balance{} : to_balance(rows_[r]);
  }
  [[nodiscard]] Margin margin(VenueId venue) const noexcept {
    const VenueState& vs = venues_[venue.value];
    const std::uint16_t r = vs.account_reported ? vs.account_row : vs.margin_default;
    Margin m;
    if (r == kNone) return m;
    const Row& row = rows_[r];
    m.asset = row.asset;
    m.account = row.account;
    if (!row.reported) return m;
    m.known = true;
    m.available = Notional::from_raw(row.free);
    m.initial = Notional::from_raw(row.locked);
    m.maintenance = Notional::from_raw(row.maintenance);
    m.equity = Notional::from_raw(row.equity);
    m.wallet = Notional::from_raw(row.total);
    m.as_of = row.as_of;
    return m;
  }
  [[nodiscard]] static Balance to_balance(const Row& row) noexcept {
    Balance b;
    if (!row.reported) return b;
    b.known = true;
    b.free = Notional::from_raw(row.free);
    b.locked = Notional::from_raw(row.locked);
    b.total = Notional::from_raw(row.total);
    b.equity = Notional::from_raw(row.equity);
    b.maintenance = Notional::from_raw(row.maintenance);
    b.as_of = row.as_of;
    return b;
  }

 private:
  struct Inst {
    VenueId venue{};
    bool derivative = false;
    std::int32_t taker_cbps = 0;
    std::int64_t im_raw = 0;
    std::uint16_t base = kNone;
    std::uint16_t quote = kNone;
    std::uint16_t settle = kNone;
    std::uint16_t margin = kNone;  // the settlement asset's row, or the account row once reported
    std::array<std::int64_t, 2> side_hold{};  // derivative: sum of its orders' holds per side
    std::array<std::int64_t, 2> unacked{};    // ... of the orders the venue has not acknowledged
  };
  struct VenueState {
    std::uint16_t account_row = kNone;
    std::uint16_t margin_default = kNone;  // the first derivative's settlement row
    std::uint32_t gen = 0;
    bool in_snapshot = false;
    bool account_reported = false;
  };

  // The row an order of `in` on `side` holds from.
  [[nodiscard]] static std::uint16_t row_of(const Inst& in, Side side) noexcept {
    if (in.derivative) return in.margin;
    return side == Side::Buy ? in.quote : in.base;
  }
  // What the unacknowledged orders of `in` hold of row `r` on top of a report: a spot side's own
  // holds; for a derivative, what they add to the larger side.
  [[nodiscard]] static std::int64_t in_flight(const Inst& in, Side side, std::uint16_t r) noexcept {
    if (row_of(in, side) != r) return 0;
    if (!in.derivative) return in.unacked[static_cast<std::size_t>(side)];
    return std::max(in.side_hold[0], in.side_hold[1]) -
           std::max(in.side_hold[0] - in.unacked[0], in.side_hold[1] - in.unacked[1]);
  }
  // A report set row `r`: the orders the venue has not acknowledged hold on top of it.
  void add_in_flight(std::uint16_t r) noexcept {
    std::int64_t extra = 0;
    for (const Inst& in : inst_) {
      if (in.venue != rows_[r].venue) continue;
      if (in.derivative) {
        extra += in_flight(in, Side::Buy, r);
      } else {
        extra += in_flight(in, Side::Buy, r) + in_flight(in, Side::Sell, r);
      }
    }
    rows_[r].free -= extra;
    rows_[r].locked += extra;
  }
  // An event the report already counted: the venue stamped it at or before the report's time.
  [[nodiscard]] static bool counted(const Row& row, Timestamp venue_ts) noexcept {
    return row.reported && venue_ts.valid() && row.as_of.valid() && venue_ts <= row.as_of;
  }
  void move_asset(std::uint16_t r, std::int64_t d, Timestamp venue_ts) noexcept {
    if (r == kNone || d == 0) return;
    Row& row = rows_[r];
    if (counted(row, venue_ts)) return;
    row.free += d;
    row.total += d;
    row.equity += d;
  }
  // notional * (1 + cbps / 1e6), the fee rounded up.
  [[nodiscard]] static std::int64_t with_fee(std::int64_t n, std::int32_t cbps) noexcept {
    if (cbps <= 0 || n <= 0) return n;
    return n + static_cast<std::int64_t>((static_cast<Int128>(n) * cbps + 999'999) / 1'000'000);
  }
  // notional * rate, rounded up.
  [[nodiscard]] static std::int64_t margin_of(std::int64_t n, std::int64_t rate_raw) noexcept {
    if (n <= 0) return 0;
    return static_cast<std::int64_t>((static_cast<Int128>(n) * rate_raw + kFixedScale - 1) /
                                     kFixedScale);
  }
  [[nodiscard]] std::uint16_t find(VenueId venue, std::string_view asset) const noexcept {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      const Row& r = rows_[i];
      if (!r.account && r.venue == venue && same_currency(r.asset.view(), asset))
        return static_cast<std::uint16_t>(i);
    }
    return kNone;
  }
  std::uint16_t row_for(VenueId venue, std::string_view asset, bool account) {
    if (asset.empty()) return kNone;
    if (const std::uint16_t r = find(venue, asset); r != kNone) return r;
    return add_row(venue, asset, account);
  }
  std::uint16_t add_row(VenueId venue, std::string_view asset, bool account) {
    Row r;
    r.venue = venue;
    r.asset = FixedString<8>(asset);
    r.account = account;
    rows_.push_back(r);
    return static_cast<std::uint16_t>(rows_.size() - 1);
  }
  // The venue's derivatives draw on its account row from now on.
  void use_account(VenueId venue, std::uint16_t r) noexcept {
    VenueState& vs = venues_[venue.value];
    vs.account_reported = true;
    for (Inst& in : inst_) {
      if (in.derivative && in.venue == venue) in.margin = r;
    }
  }
  // Rows of the venue the snapshot did not name hold nothing. An account row that was never
  // reported stays unknown: the venue does not report one.
  void end_snapshot(VenueId venue, Timestamp ts) noexcept {
    VenueState& vs = venues_[venue.value];
    vs.in_snapshot = false;
    ++stats_.snapshots;
    for (Row& r : rows_) {
      if (r.venue != venue || r.gen == vs.gen) continue;
      if (r.account && !r.reported) continue;
      r.free = r.locked = r.total = r.equity = r.maintenance = 0;
      r.as_of = ts;
      r.reported = true;
      r.gen = vs.gen;
      add_in_flight(static_cast<std::uint16_t>(&r - rows_.data()));
    }
  }

  std::vector<Row> rows_;
  std::array<Inst, kMaxInstruments> inst_{};
  std::array<VenueState, kMaxVenueSlots> venues_{};
  BalanceStats stats_{};
  bool live_ = false;
  bool check_ = true;
};

}  // namespace fastmm
