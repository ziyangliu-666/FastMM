#pragma once
// Coinbase Exchange `user` channel decoder (https://docs.cdp.coinbase.com/exchange/websocket-feed/
// channels, read 2026-09-30): the full channel's messages about the authenticated profile's orders.
// Only `received` names the client order id (client_oid); the others name the venue's order_id,
// so the parser keeps a table of this connector's orders by order id (OrderKey), filled from
// `received`, from the REST replies (learn()) and from reconciliation snapshots.
//
//   received  (client_oid is FastMM's)   -> OrderAckMsg; the order is tracked (size)
//   open                                  -> nothing
//   match     (maker_order_id or taker_order_id tracked)
//                                         -> OrderFillMsg: exec id trade_id, size, price, cum and
//                                            leaves from the tracked size; side the maker's for
//                                            the maker and the other for the taker; fee = price x
//                                            size x maker_fee_rate or taker_fee_rate, in the quote
//   done      reason filled               -> nothing (the fills carried it); untracked
//             reason canceled             -> OrderExpiredMsg when cancel_reason is 101 (time in
//                                            force: IOC, FOK, post-only), else OrderCancelAckMsg
//                                            (cum = the size filled)
//   change    (STP, modify)               -> the tracked size becomes new_size
//   heartbeat, subscriptions, error       -> control
//
// and the `balance` channel's messages (same page, "Balance Channel"; subscribed by account_ids):
//   balance   {account_id, currency, holds, available, updated, timestamp}
//                                         -> BalanceMsg for `currency` as the venue spells it:
//                                            free = available, locked = holds; the venue time is
//                                            `updated` ("when last balance change is observed"),
//                                            else `timestamp`. The connector keeps only the assets
//                                            the engine tracks.
//
// Events about orders that are not tracked are counted and dropped (orders placed by hand or by
// other software on the same profile).
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm::venues::coinbase {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t received = 0;
  std::uint64_t fills = 0;
  std::uint64_t done = 0;
  std::uint64_t changes = 0;
  std::uint64_t balances = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign = 0;     // events about orders that are not FastMM's
  std::uint64_t table_full = 0;  // an order that could not be tracked
};

enum class PrivateControl : std::uint8_t { None = 0, Subscriptions, Heartbeat, Error, Other };

struct PrivateDecodeResult : DecodeResult {
  std::uint32_t count = 0;
  PrivateControl control = PrivateControl::None;
  std::string_view msg;     // error: message
  std::string_view reason;  // error: reason
};

// What the parser knows of one of this connector's orders.
struct TrackedOrder {
  ClientOrderId cl{};
  InstrumentId instrument{};
  Side side = Side::Buy;
  Qty size{};
  Qty filled{};
};

class CoinbasePrivateParser {
 public:
  static constexpr std::size_t kSlots = kShadowSlots;

  CoinbasePrivateParser(const SymbolTable& symbols,
                        const InstrumentTable& instruments,
                        VenueId venue,
                        std::size_t capacity = 1U << 20);
  ~CoinbasePrivateParser();
  CoinbasePrivateParser(const CoinbasePrivateParser&) = delete;
  CoinbasePrivateParser& operator=(const CoinbasePrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  // An order of this connector's, by the venue's order id (a REST reply, a snapshot row). Keeps
  // the fill count of an order already tracked. False when the id is not a UUID or the table is
  // full.
  bool learn(std::string_view order_id,
             ClientOrderId cl,
             InstrumentId instrument,
             Side side,
             Qty size,
             Qty filled = {}) noexcept;
  [[nodiscard]] const TrackedOrder* find(std::string_view order_id) const noexcept;
  // Drops every tracked order `keep` says no (the connector's shadow sweep). Control path.
  template <class Keep>
  std::size_t sweep(const Keep& keep) {
    std::vector<OrderKey> gone;
    orders_.for_each([&](OrderKey k, const TrackedOrder& o) {
      if (!keep(o.cl)) gone.push_back(k);
    });
    for (OrderKey k : gone) orders_.erase(k);
    return gone.size();
  }
  [[nodiscard]] std::size_t tracked() const noexcept { return orders_.size(); }

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  OpenHashMap<OrderKey, TrackedOrder, kSlots> orders_;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::coinbase
