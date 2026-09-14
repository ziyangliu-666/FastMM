// TotalView-ITCH 5.0 encoder (see itch_encoder.hpp).
#include "fastmm/codecs/itch/itch_encoder.hpp"

namespace fastmm::codecs::itch {

namespace {
[[nodiscard]] constexpr char side_indicator(Side s) noexcept {
  return s == Side::Buy ? 'B' : 'S';
}
}  // namespace

bool ItchEncoder::header(MessageHeader& h,
                         char type,
                         std::uint16_t locate,
                         std::uint64_t ts_ns) const noexcept {
  if (ts_ns > nasdaq::kMaxBe48) return false;
  h.message_type = type;
  h.stock_locate.set(locate);
  h.tracking_number.set(tracking_);
  h.timestamp.set(ts_ns);
  return true;
}

std::size_t ItchEncoder::system_event(std::span<std::byte> out,
                                      std::uint64_t ts_ns,
                                      char event_code) const noexcept {
  SystemEvent m{};
  if (!header(m.hdr, 'S', 0, ts_ns)) return 0;
  m.event_code = event_code;
  return write(out, m);
}

std::size_t ItchEncoder::stock_directory(std::span<std::byte> out,
                                         std::uint16_t locate,
                                         std::uint64_t ts_ns,
                                         std::string_view stock,
                                         std::uint32_t round_lot_size,
                                         char market_category) const noexcept {
  StockDirectory m{};
  if (!header(m.hdr, 'R', locate, ts_ns)) return 0;
  nasdaq::put_alpha(m.stock, sizeof m.stock, stock);
  m.market_category = market_category;
  m.financial_status_indicator = 'N';
  m.round_lot_size.set(round_lot_size);
  m.round_lots_only = 'N';
  m.issue_classification = 'C';
  nasdaq::put_alpha(m.issue_sub_type, sizeof m.issue_sub_type, "Z");
  m.authenticity = 'P';
  m.short_sale_threshold_indicator = 'N';
  m.ipo_flag = 'N';
  m.luld_reference_price_tier = '1';
  m.etp_flag = 'N';
  m.etp_leverage_factor.set(0);
  m.inverse_indicator = 'N';
  return write(out, m);
}

std::size_t ItchEncoder::add_order(std::span<std::byte> out,
                                   std::uint16_t locate,
                                   std::uint64_t ts_ns,
                                   std::uint64_t order_ref,
                                   Side side,
                                   Qty shares,
                                   std::string_view stock,
                                   Price price) const noexcept {
  AddOrder m{};
  std::uint32_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'A', locate, ts_ns) || !nasdaq::qty_to_shares(shares, n) ||
      !nasdaq::price_to_price4(price, p4))
    return 0;
  m.order_reference_number.set(order_ref);
  m.buy_sell_indicator = side_indicator(side);
  m.shares.set(n);
  nasdaq::put_alpha(m.stock, sizeof m.stock, stock);
  m.price.set(p4);
  return write(out, m);
}

std::size_t ItchEncoder::add_order_mpid(std::span<std::byte> out,
                                        std::uint16_t locate,
                                        std::uint64_t ts_ns,
                                        std::uint64_t order_ref,
                                        Side side,
                                        Qty shares,
                                        std::string_view stock,
                                        Price price,
                                        std::string_view attribution) const noexcept {
  AddOrderMpid m{};
  std::uint32_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'F', locate, ts_ns) || !nasdaq::qty_to_shares(shares, n) ||
      !nasdaq::price_to_price4(price, p4))
    return 0;
  m.order_reference_number.set(order_ref);
  m.buy_sell_indicator = side_indicator(side);
  m.shares.set(n);
  nasdaq::put_alpha(m.stock, sizeof m.stock, stock);
  m.price.set(p4);
  nasdaq::put_alpha(m.attribution, sizeof m.attribution, attribution);
  return write(out, m);
}

std::size_t ItchEncoder::order_executed(std::span<std::byte> out,
                                        std::uint16_t locate,
                                        std::uint64_t ts_ns,
                                        std::uint64_t order_ref,
                                        Qty executed,
                                        std::uint64_t match) const noexcept {
  OrderExecuted m{};
  std::uint32_t n = 0;
  if (!header(m.hdr, 'E', locate, ts_ns) || !nasdaq::qty_to_shares(executed, n)) return 0;
  m.order_reference_number.set(order_ref);
  m.executed_shares.set(n);
  m.match_number.set(match);
  return write(out, m);
}

std::size_t ItchEncoder::order_executed_with_price(std::span<std::byte> out,
                                                   std::uint16_t locate,
                                                   std::uint64_t ts_ns,
                                                   std::uint64_t order_ref,
                                                   Qty executed,
                                                   std::uint64_t match,
                                                   Price execution_price,
                                                   bool printable) const noexcept {
  OrderExecutedWithPrice m{};
  std::uint32_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'C', locate, ts_ns) || !nasdaq::qty_to_shares(executed, n) ||
      !nasdaq::price_to_price4(execution_price, p4))
    return 0;
  m.order_reference_number.set(order_ref);
  m.executed_shares.set(n);
  m.match_number.set(match);
  m.printable = printable ? 'Y' : 'N';
  m.execution_price.set(p4);
  return write(out, m);
}

std::size_t ItchEncoder::order_cancel(std::span<std::byte> out,
                                      std::uint16_t locate,
                                      std::uint64_t ts_ns,
                                      std::uint64_t order_ref,
                                      Qty cancelled) const noexcept {
  OrderCancel m{};
  std::uint32_t n = 0;
  if (!header(m.hdr, 'X', locate, ts_ns) || !nasdaq::qty_to_shares(cancelled, n)) return 0;
  m.order_reference_number.set(order_ref);
  m.cancelled_shares.set(n);
  return write(out, m);
}

std::size_t ItchEncoder::order_delete(std::span<std::byte> out,
                                      std::uint16_t locate,
                                      std::uint64_t ts_ns,
                                      std::uint64_t order_ref) const noexcept {
  OrderDelete m{};
  if (!header(m.hdr, 'D', locate, ts_ns)) return 0;
  m.order_reference_number.set(order_ref);
  return write(out, m);
}

std::size_t ItchEncoder::order_replace(std::span<std::byte> out,
                                       std::uint16_t locate,
                                       std::uint64_t ts_ns,
                                       std::uint64_t original_ref,
                                       std::uint64_t new_ref,
                                       Qty shares,
                                       Price price) const noexcept {
  OrderReplace m{};
  std::uint32_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'U', locate, ts_ns) || !nasdaq::qty_to_shares(shares, n) ||
      !nasdaq::price_to_price4(price, p4))
    return 0;
  m.original_order_reference_number.set(original_ref);
  m.new_order_reference_number.set(new_ref);
  m.shares.set(n);
  m.price.set(p4);
  return write(out, m);
}

std::size_t ItchEncoder::trade(std::span<std::byte> out,
                               std::uint16_t locate,
                               std::uint64_t ts_ns,
                               Side side,
                               Qty shares,
                               std::string_view stock,
                               Price price,
                               std::uint64_t match) const noexcept {
  Trade m{};
  std::uint32_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'P', locate, ts_ns) || !nasdaq::qty_to_shares(shares, n) ||
      !nasdaq::price_to_price4(price, p4))
    return 0;
  m.order_reference_number.set(0);
  m.buy_sell_indicator = side_indicator(side);
  m.shares.set(n);
  nasdaq::put_alpha(m.stock, sizeof m.stock, stock);
  m.price.set(p4);
  m.match_number.set(match);
  return write(out, m);
}

std::size_t ItchEncoder::cross_trade(std::span<std::byte> out,
                                     std::uint16_t locate,
                                     std::uint64_t ts_ns,
                                     Qty shares,
                                     std::string_view stock,
                                     Price cross_price,
                                     std::uint64_t match,
                                     char cross_type) const noexcept {
  CrossTrade m{};
  std::uint64_t n = 0;
  std::uint32_t p4 = 0;
  if (!header(m.hdr, 'Q', locate, ts_ns) || !nasdaq::qty_to_shares(shares, n) ||
      !nasdaq::price_to_price4(cross_price, p4))
    return 0;
  m.shares.set(n);
  nasdaq::put_alpha(m.stock, sizeof m.stock, stock);
  m.cross_price.set(p4);
  m.match_number.set(match);
  m.cross_type = cross_type;
  return write(out, m);
}

std::size_t ItchEncoder::broken_trade(std::span<std::byte> out,
                                      std::uint16_t locate,
                                      std::uint64_t ts_ns,
                                      std::uint64_t match) const noexcept {
  BrokenTrade m{};
  if (!header(m.hdr, 'B', locate, ts_ns)) return 0;
  m.match_number.set(match);
  return write(out, m);
}

}  // namespace fastmm::codecs::itch
