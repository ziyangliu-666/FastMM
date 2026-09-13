#pragma once
// Venue-side state of the simulated exchange that is independent of the network: accounts with
// balances, the order index (Binance order id / client order id / matching-engine id) and the
// fixed-window rate-limit counters Binance reports in X-MBX-USED-WEIGHT-1M and
// X-MBX-ORDER-COUNT-10S. Control path: allocation is fine here.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/sim/server/binance_json.hpp"
#include "fastmm/sim/sim_book.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fastmm::sim::server {

// Binance rate limits are fixed windows aligned to the interval ("per 1 MINUTE").
class FixedWindowCounter {
 public:
  explicit FixedWindowCounter(std::int64_t window_ms = 60'000) noexcept
      : window_ms_(window_ms > 0 ? window_ms : 1) {}

  std::uint64_t add(std::uint64_t n, std::int64_t now_ms) noexcept {
    roll(now_ms);
    count_ += n;
    return count_;
  }
  [[nodiscard]] std::uint64_t count(std::int64_t now_ms) noexcept {
    roll(now_ms);
    return count_;
  }
  // Milliseconds until the current window resets (>= 1).
  [[nodiscard]] std::int64_t ms_until_reset(std::int64_t now_ms) const noexcept {
    const std::int64_t start = window_start(now_ms);
    return start + window_ms_ - now_ms;
  }
  [[nodiscard]] std::int64_t window_ms() const noexcept { return window_ms_; }

 private:
  [[nodiscard]] std::int64_t window_start(std::int64_t now_ms) const noexcept {
    std::int64_t r = now_ms % window_ms_;
    if (r < 0) r += window_ms_;
    return now_ms - r;
  }
  void roll(std::int64_t now_ms) noexcept {
    const std::int64_t start = window_start(now_ms);
    if (start != start_) {
      start_ = start;
      count_ = 0;
    }
  }
  std::int64_t window_ms_;
  std::int64_t start_ = -1;
  std::uint64_t count_ = 0;
};

// Asset amounts use the core 1e-8 fixed point (Qty) regardless of the asset.
struct Balance {
  Qty free{};
  Qty locked{};
};

struct Account {
  AccountId id = kStrategyAccount;
  std::string api_key;
  std::string api_secret;  // never logged
  std::map<std::string, Balance, std::less<>> balances;
  FixedWindowCounter orders_10s{10'000};
  FixedWindowCounter orders_1d{86'400'000};
  bool balance_dirty = false;

  Balance& balance(std::string_view asset) {
    auto it = balances.find(asset);
    if (it == balances.end()) it = balances.emplace(std::string(asset), Balance{}).first;
    return it->second;
  }
};

struct OrderRecord {
  std::int64_t order_id = 0;  // Binance orderId (server-assigned, monotonic)
  ClientOrderId internal{};   // id inside the MatchingEngine (changes on amend)
  std::uint32_t symbol = 0;   // index into the server's symbol table
  AccountId account = kStrategyAccount;
  std::string client_order_id;  // Binance clientOrderId
  Side side = Side::Buy;
  BinanceOrderType type = BinanceOrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price{};
  Qty orig_qty{};
  Qty executed{};
  Notional cum_quote{};
  BinanceOrderStatus status = BinanceOrderStatus::New;
  std::int64_t time_ms = 0;
  std::int64_t update_ms = 0;
  Qty locked{};              // still-reserved amount (quote notional for buys, base for sells)
  bool locks_quote = false;  // which asset `locked` refers to
  bool acked = false;        // MatchingSink::on_ack seen

  [[nodiscard]] bool terminal() const noexcept { return is_terminal(status); }
  [[nodiscard]] Qty leaves() const noexcept { return orig_qty - executed; }
};

// Order index: Binance order id -> record (ordered, so listings are deterministic), plus
// lookups by matching-engine id and by (account, symbol, clientOrderId). Terminal records stay
// until erase_terminal() so callers holding a pointer during one request remain valid.
class OrderIndex {
 public:
  OrderRecord& insert(OrderRecord r);
  [[nodiscard]] OrderRecord* by_order_id(std::int64_t id) noexcept;
  [[nodiscard]] OrderRecord* by_internal(ClientOrderId id) noexcept;
  [[nodiscard]] OrderRecord* by_client_id(AccountId account,
                                          std::uint32_t symbol,
                                          std::string_view client_order_id) noexcept;
  void rekey_internal(OrderRecord& r, ClientOrderId new_internal);
  void rekey_client(OrderRecord& r, std::string new_client_order_id);
  // Drops terminal records from every index; returns how many were erased.
  std::size_t erase_terminal();

  // Open (non-terminal) orders of `account`, ascending order id; symbol < 0 = all symbols.
  [[nodiscard]] std::vector<OrderRecord*> open_orders(AccountId account, std::int64_t symbol);
  [[nodiscard]] std::size_t open_count(AccountId account) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }

 private:
  [[nodiscard]] static std::string client_key(AccountId account,
                                              std::uint32_t symbol,
                                              std::string_view id);
  std::map<std::int64_t, OrderRecord> orders_;
  std::unordered_map<std::uint64_t, std::int64_t> by_internal_;
  std::unordered_map<std::string, std::int64_t> by_client_;
};

}  // namespace fastmm::sim::server
