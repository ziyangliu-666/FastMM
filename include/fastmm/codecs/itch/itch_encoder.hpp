#pragma once
// ItchEncoder: writes TotalView-ITCH 5.0 messages (simulation and replay).
//
// Layouts come from itch_messages.hpp (spec: Nasdaq TotalView-ITCH 5.0). Every writer returns
// the number of bytes written, or 0 when `out` is too small or a value cannot be represented
// exactly on the wire (timestamp >= 2^48, negative or sub-0.0001 price, fractional or
// out-of-range share count). Writers never allocate.
//
// The typed writers cover the messages the decoder turns into events plus S, R and B. Any
// other message can be filled in its packed struct and written with write().
#include "fastmm/codecs/itch/itch_messages.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace fastmm::codecs::itch {

class ItchEncoder {
 public:
  // Tracking Number ("Nasdaq internal tracking number") stamped on every message.
  void set_tracking_number(std::uint16_t t) noexcept { tracking_ = t; }

  std::size_t system_event(std::span<std::byte> out,
                           std::uint64_t ts_ns,
                           char event_code) const noexcept;
  std::size_t stock_directory(std::span<std::byte> out,
                              std::uint16_t locate,
                              std::uint64_t ts_ns,
                              std::string_view stock,
                              std::uint32_t round_lot_size = 100,
                              char market_category = 'Q') const noexcept;
  std::size_t add_order(std::span<std::byte> out,
                        std::uint16_t locate,
                        std::uint64_t ts_ns,
                        std::uint64_t order_ref,
                        Side side,
                        Qty shares,
                        std::string_view stock,
                        Price price) const noexcept;
  std::size_t add_order_mpid(std::span<std::byte> out,
                             std::uint16_t locate,
                             std::uint64_t ts_ns,
                             std::uint64_t order_ref,
                             Side side,
                             Qty shares,
                             std::string_view stock,
                             Price price,
                             std::string_view attribution) const noexcept;
  std::size_t order_executed(std::span<std::byte> out,
                             std::uint16_t locate,
                             std::uint64_t ts_ns,
                             std::uint64_t order_ref,
                             Qty executed,
                             std::uint64_t match) const noexcept;
  std::size_t order_executed_with_price(std::span<std::byte> out,
                                        std::uint16_t locate,
                                        std::uint64_t ts_ns,
                                        std::uint64_t order_ref,
                                        Qty executed,
                                        std::uint64_t match,
                                        Price execution_price,
                                        bool printable) const noexcept;
  std::size_t order_cancel(std::span<std::byte> out,
                           std::uint16_t locate,
                           std::uint64_t ts_ns,
                           std::uint64_t order_ref,
                           Qty cancelled) const noexcept;
  std::size_t order_delete(std::span<std::byte> out,
                           std::uint16_t locate,
                           std::uint64_t ts_ns,
                           std::uint64_t order_ref) const noexcept;
  std::size_t order_replace(std::span<std::byte> out,
                            std::uint16_t locate,
                            std::uint64_t ts_ns,
                            std::uint64_t original_ref,
                            std::uint64_t new_ref,
                            Qty shares,
                            Price price) const noexcept;
  std::size_t trade(std::span<std::byte> out,
                    std::uint16_t locate,
                    std::uint64_t ts_ns,
                    Side side,
                    Qty shares,
                    std::string_view stock,
                    Price price,
                    std::uint64_t match) const noexcept;
  std::size_t cross_trade(std::span<std::byte> out,
                          std::uint16_t locate,
                          std::uint64_t ts_ns,
                          Qty shares,
                          std::string_view stock,
                          Price cross_price,
                          std::uint64_t match,
                          char cross_type) const noexcept;
  std::size_t broken_trade(std::span<std::byte> out,
                           std::uint16_t locate,
                           std::uint64_t ts_ns,
                           std::uint64_t match) const noexcept;

  // Any message already filled in its packed struct.
  template <class M>
  static std::size_t write(std::span<std::byte> out, const M& m) noexcept {
    if (out.size() < sizeof(M)) return 0;
    std::memcpy(out.data(), &m, sizeof(M));
    return sizeof(M);
  }
  // Fills the common header; false if the timestamp does not fit 48 bits.
  bool header(MessageHeader& h,
              char type,
              std::uint16_t locate,
              std::uint64_t ts_ns) const noexcept;

 private:
  std::uint16_t tracking_ = 0;
};

}  // namespace fastmm::codecs::itch
