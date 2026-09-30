#pragma once
// Coinbase Advanced Trade `user` channel decoder, wss://advanced-trade-ws-user.coinbase.com
// (AsyncAPI, read 2026-09-30). Every message is an envelope with events[{"type":"snapshot"|
// "update","orders":[..],"positions":{..}}]; an order is its whole state:
//   {order_id, client_order_id, cumulative_quantity, leaves_quantity, avg_price, total_fees,
//    status PENDING | OPEN | FILLED | CANCEL_QUEUED | CANCELLED | EXPIRED | FAILED, product_id,
//    order_side, order_type, time_in_force, cancel_reason, reject_reason, ...}
// The channel names no execution: a fill shows only as a larger cumulative_quantity. The parser
// reports that as a FillDue for the connector, which reads the order's executions
// (GET /orders/historical/fills?order_ids=) and emits them with their trade ids.
//
//   first message of an order (not FAILED)  -> OrderAckMsg (the venue's order_id)
//   cumulative_quantity above the last seen -> FillDue
//   CANCELLED                               -> OrderCancelAckMsg (cum = cumulative_quantity), or
//                                              OrderExpiredMsg for an IOC or FOK order
//   EXPIRED                                 -> OrderExpiredMsg
//   FAILED                                  -> OrderRejectMsg (reject_reason)
//   FILLED                                  -> nothing more (the executions close it)
// Orders whose client_order_id is not FastMM's are counted and dropped. The open orders of the
// account arrive as a snapshot on subscribing, 50 a message.
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm::venues::coinbase {

struct AdvancedUserStats {
  std::uint64_t frames = 0;
  std::uint64_t orders = 0;
  std::uint64_t fills_due = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign = 0;
  std::uint64_t table_full = 0;
};

// An order whose cumulative quantity grew: its executions are to be read.
struct FillDue {
  ClientOrderId cl{};
  InstrumentId instrument{};
  Qty cum{};
  VenueOrderId order_id{};
};

enum class UserControl : std::uint8_t { None = 0, Subscriptions, Heartbeat, Error, Other };

struct AdvancedUserResult : DecodeResult {
  static constexpr std::size_t kMaxDue = 64;
  std::uint32_t count = 0;  // order events written back to back
  UserControl control = UserControl::None;
  std::string_view msg;  // error: message
  bool has_sequence = false;
  std::uint64_t sequence = 0;
  std::uint32_t due_count = 0;
  std::array<FillDue, kMaxDue> due{};
};

class AdvancedUserParser {
 public:
  static constexpr std::size_t kSlots = kShadowSlots;

  AdvancedUserParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity = 1U << 20);
  ~AdvancedUserParser();
  AdvancedUserParser(const AdvancedUserParser&) = delete;
  AdvancedUserParser& operator=(const AdvancedUserParser&) = delete;

  // `out` must hold kDecoderScratchBytes. `r` is filled in place (it holds the FillDue list).
  void decode(std::string_view json,
              Timestamp recv_ts,
              Cycles t0,
              std::span<std::byte> out,
              AdvancedUserResult& r) noexcept;

  // Drops what it knows of the orders `keep` says no to (the connector's shadow sweep).
  template <class Keep>
  std::size_t sweep(const Keep& keep) {
    std::vector<ClientOrderId> gone;
    orders_.for_each_key([&](ClientOrderId id) {
      if (!keep(id)) gone.push_back(id);
    });
    for (ClientOrderId id : gone) orders_.erase(id);
    return gone.size();
  }
  [[nodiscard]] std::size_t tracked() const noexcept { return orders_.size(); }
  [[nodiscard]] const AdvancedUserStats& stats() const noexcept { return stats_; }

 private:
  struct Seen {
    Qty cum{};
    bool acked = false;
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  OpenHashMap<ClientOrderId, Seen, kSlots> orders_;
  AdvancedUserStats stats_;
};

}  // namespace fastmm::venues::coinbase
