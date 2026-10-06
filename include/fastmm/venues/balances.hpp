#pragma once
// What a connector needs to report the account's balances (BalanceMsg, core/messages.hpp):
//   * VenueAssets: the assets the engine keeps for one venue (the base and quote of its spot
//     instruments, the settlement asset of its derivatives; core/balance_book.hpp), so a connector
//     forwards those and not the hundreds a venue account lists;
//   * BalanceFields and emit_balance(): one asset from the private stream, on the order sink.
// A snapshot (every asset at once, from REST) goes through ReconcileDriver's balance leg instead,
// which marks it kSnapshot / kSnapshotEnd.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <x86intrin.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fastmm::venues {

// The amounts of one asset, as BalanceMsg documents them. Spot: free and locked, total = free +
// locked (balance() fills total and equity when they are left zero). Derivatives: free = available,
// locked = initial margin, total = wallet balance, equity, maintenance.
struct BalanceFields {
  Notional free;
  Notional locked;
  Notional total;
  Notional equity;
  Notional maintenance;
  // What can leave the account by an internal transfer, when the venue says
  // (BalanceMsg::kWithdrawable); otherwise `free`.
  Notional withdrawable;
  bool has_withdrawable = false;

  // A spot holding: total and equity from free + locked.
  [[nodiscard]] static BalanceFields spot(Notional free, Notional locked) noexcept {
    BalanceFields f;
    f.free = free;
    f.locked = locked;
    f.total = free + locked;
    f.equity = f.total;
    return f;
  }
};

class VenueAssets {
 public:
  VenueAssets() = default;
  VenueAssets(const InstrumentTable& insts, VenueId venue) { build(insts, venue); }

  void build(const InstrumentTable& insts, VenueId venue) {
    names_.clear();
    for (const Instrument& i : insts) {
      if (i.venue != venue) continue;
      if (i.is_derivative()) {
        add(i.settlement_ccy());
      } else {
        add(i.base.view());
        add(i.quote.view());
      }
    }
  }
  // The engine's name of `asset` (its instruments' spelling), or empty when it keeps no such asset.
  // Case-insensitive.
  [[nodiscard]] std::string_view find(std::string_view asset) const noexcept {
    for (const FixedString<8>& n : names_) {
      if (same_currency(n.view(), asset)) return n.view();
    }
    return {};
  }
  [[nodiscard]] bool tracks(std::string_view asset) const noexcept { return !find(asset).empty(); }
  [[nodiscard]] const std::vector<FixedString<8>>& names() const noexcept { return names_; }

 private:
  void add(std::string_view a) {
    if (a.empty() || tracks(a)) return;
    names_.emplace_back(a);
  }
  std::vector<FixedString<8>> names_;
};

// Fills a BalanceMsg for `asset` stamped with the venue's time `venue_ms` (Unix ms; 0: unknown).
inline void fill_balance(BalanceMsg& m,
                         VenueId venue,
                         std::string_view asset,
                         const BalanceFields& f,
                         std::int64_t venue_ms,
                         std::uint8_t flags) noexcept {
  init_header(m, EventType::Balance, InstrumentId::invalid(), venue);
  if (venue_ms > 0) m.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
  m.free = f.free;
  m.locked = f.locked;
  m.total = f.total;
  m.equity = f.equity;
  m.maintenance = f.maintenance;
  m.asset.assign(asset);
  m.flags = flags;
  if (f.has_withdrawable) {
    m.withdrawable = f.withdrawable;
    m.flags = static_cast<std::uint8_t>(m.flags | BalanceMsg::kWithdrawable);
  }
  m.hdr.recv_ts = wall_now();
}

// One asset from the private stream (an update, not a snapshot) on the order sink. `flags`:
// BalanceMsg::kAccount for the account-wide margin. An asset `assets` does not keep is dropped
// (returns false), unless it is the account row.
inline bool emit_balance(EventSink& sink,
                         const VenueAssets& assets,
                         VenueId venue,
                         std::string_view asset,
                         const BalanceFields& f,
                         std::int64_t venue_ms,
                         std::uint8_t flags = 0) noexcept {
  std::string_view name = asset;
  if ((flags & BalanceMsg::kAccount) == 0) {
    name = assets.find(asset);
    if (name.empty()) return false;
  }
  BalanceMsg m{};
  fill_balance(m, venue, name, f, venue_ms, flags);
  m.hdr.t0_cycles = rdtscp();
  return sink.push(m.hdr);
}

}  // namespace fastmm::venues
