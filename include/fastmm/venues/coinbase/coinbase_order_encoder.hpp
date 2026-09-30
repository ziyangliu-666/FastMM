#pragma once
// Coinbase Exchange REST requests (https://docs.cdp.coinbase.com/api-reference/exchange-api/
// rest-api/orders/*, read 2026-09-30). Order entry is REST only: the Exchange's WebSocket feed
// carries no order operations, and FIX order entry is not used by this connector.
//
//   POST   /orders  {"client_oid","product_id","side":"buy"|"sell","type":"limit"|"market",
//                    "price","size","time_in_force":"GTC"|"IOC"|"FOK","post_only":true,"stp"}
//   DELETE /orders/client:<client_oid>?product_id=P      cancel by the client id
//   DELETE /orders?product_id=P                          cancel all of a product (best effort:
//                                                        "may require ... multiple times")
//   GET    /orders?status=open&status=pending&status=active&limit=1000[&after=C]
//   GET    /fills?product_id=P&start_date=T[&end_date=T][&after=C]&limit=100
//   GET    /orders/<order_id>                            an order a fill names (client_oid)
//   GET    /products/<P>, GET /time
//
// The venue has no replace for REST orders; the connector sends cancel and new.
// encode_new() and encode_cancel() write into fixed buffers and do not allocate; the control
// requests build std::strings.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fastmm::venues::coinbase {

// Self-trade prevention: decrease and cancel (the venue's default), cancel oldest, cancel newest,
// cancel both.
enum class Stp : std::uint8_t { Dc = 0, Co = 1, Cn = 2, Cb = 3 };
[[nodiscard]] constexpr std::string_view to_string(Stp s) noexcept {
  switch (s) {
    case Stp::Co:
      return "co";
    case Stp::Cn:
      return "cn";
    case Stp::Cb:
      return "cb";
    case Stp::Dc:
      return "dc";
  }
  return "dc";
}

// One request, in fixed buffers.
struct OrderRequest {
  std::string_view method;  // "POST" | "DELETE"
  std::array<char, 160> path{};
  std::size_t path_n = 0;
  std::array<char, 384> body{};
  std::size_t body_n = 0;
  [[nodiscard]] std::string_view target() const noexcept { return {path.data(), path_n}; }
  [[nodiscard]] std::string_view payload() const noexcept { return {body.data(), body_n}; }
};

class CoinbaseOrderEncoder {
 public:
  explicit CoinbaseOrderEncoder(const SymbolTable& symbols, Stp stp = Stp::Dc) noexcept
      : symbols_(symbols), stp_(stp) {}

  // POST /orders for a New; false for an unknown instrument or a request that does not fit.
  bool encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept;
  // DELETE /orders/client:<client_oid>?product_id=P for a Cancel.
  bool encode_cancel(const OrderCommand& cmd, OrderRequest& out) const noexcept;

  // Control requests: the path with its query.
  static std::string open_orders_path(std::string_view after);
  static std::string fills_path(std::string_view product,
                                std::int64_t start_ms,
                                std::int64_t end_ms,
                                std::string_view after,
                                int limit);
  static std::string order_path(std::string_view order_id);
  static std::string cancel_all_path(std::string_view product);

  [[nodiscard]] Stp stp() const noexcept { return stp_; }

 private:
  const SymbolTable& symbols_;
  Stp stp_;
};

}  // namespace fastmm::venues::coinbase
