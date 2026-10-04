#pragma once
// PoolPlan: several exchange accounts behind one venue ([venues.<member>] pool_of = "<primary>").
//
// A venue's instruments, books, tickers and positions belong to its primary; the members are
// further accounts on the same exchange that take orders for those instruments, each with its own
// keys, balances and order-count windows. The engine routes a new order to one member
// (NewOrderRequest::account, else by balance and remaining window), the order keeps the member it
// went to, and every event of it carries that member's venue id. A member has no instruments of
// its own and no market data.
//
// Built by Config::pool_plan() from the configuration and carried in EngineConfig::pools and
// sim::SimTransportConfig::pools; a configuration without pools leaves it inactive.
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm {

// The accounts of one pool, the primary first (StrategyContext::pool).
struct PoolMembers {
  static constexpr std::size_t kMax = 8;
  std::array<VenueId, kMax> venue{};
  std::uint8_t count = 0;

  [[nodiscard]] std::size_t size() const noexcept { return count; }
  [[nodiscard]] bool empty() const noexcept { return count == 0; }
  [[nodiscard]] const VenueId* begin() const noexcept { return venue.data(); }
  [[nodiscard]] const VenueId* end() const noexcept { return venue.data() + count; }
  [[nodiscard]] VenueId operator[](std::size_t i) const noexcept { return venue[i]; }
  [[nodiscard]] bool contains(VenueId v) const noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (venue[i] == v) return true;
    }
    return false;
  }
};

struct PoolPlan {
  static constexpr std::size_t kVenues = 8;  // == kMaxVenues (core/transport.hpp)
  static constexpr std::size_t kMaxMembers = PoolMembers::kMax;  // primary included

  // primary_of[v]: the primary of member v; invalid where v is not a member.
  std::array<VenueId, kVenues> primary_of{};
  bool active = false;

  // Makes `member` an account of `primary`'s pool. False when either id is out of range, the two
  // are equal, `member` already belongs to a pool, `primary` is itself a member, or the pool is
  // full.
  bool add(VenueId member, VenueId primary) noexcept {
    if (!member.valid() || !primary.valid() || member == primary) return false;
    if (member.value >= kVenues || primary.value >= kVenues) return false;
    if (primary_of[member.value].valid() || primary_of[primary.value].valid()) return false;
    if (members(primary).size() >= kMaxMembers) return false;
    primary_of[member.value] = primary;
    active = true;
    return true;
  }
  // The venue whose instruments `v` takes orders for: its primary, or `v` itself.
  [[nodiscard]] VenueId primary(VenueId v) const noexcept {
    return v.value < kVenues && primary_of[v.value].valid() ? primary_of[v.value] : v;
  }
  [[nodiscard]] bool is_member(VenueId v) const noexcept {
    return v.value < kVenues && primary_of[v.value].valid();
  }
  // `primary` has members (or is one).
  [[nodiscard]] bool pooled(VenueId primary) const noexcept {
    if (!active || primary.value >= kVenues) return false;
    if (primary_of[primary.value].valid()) return true;
    for (const VenueId p : primary_of) {
      if (p == primary) return true;
    }
    return false;
  }
  // The accounts of `primary`'s pool, `primary` first; just `primary` where it has none.
  [[nodiscard]] PoolMembers members(VenueId primary) const noexcept {
    PoolMembers m;
    if (!primary.valid()) return m;
    m.venue[m.count++] = primary;
    for (std::size_t v = 0; v < kVenues && m.count < PoolMembers::kMax; ++v) {
      if (primary_of[v] == primary) m.venue[m.count++] = VenueId{static_cast<std::uint8_t>(v)};
    }
    return m;
  }
  // `account` may take orders for `primary`'s instruments: it is the primary or one of its members.
  [[nodiscard]] bool admits(VenueId primary, VenueId account) const noexcept {
    return account == primary || (account.value < kVenues && primary_of[account.value] == primary);
  }
};

}  // namespace fastmm
