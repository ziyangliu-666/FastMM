#include "fastmm/sim/server/venue_state.hpp"

namespace fastmm::sim::server {

std::string OrderIndex::client_key(AccountId account, std::uint32_t symbol, std::string_view id) {
  std::string k = std::to_string(account);
  k.push_back('|');
  k += std::to_string(symbol);
  k.push_back('|');
  k += id;
  return k;
}

OrderRecord& OrderIndex::insert(OrderRecord r) {
  const std::int64_t id = r.order_id;
  by_internal_[r.internal.value] = id;
  by_client_[client_key(r.account, r.symbol, r.client_order_id)] = id;
  auto [it, inserted] = orders_.insert_or_assign(id, std::move(r));
  static_cast<void>(inserted);
  return it->second;
}

OrderRecord* OrderIndex::by_order_id(std::int64_t id) noexcept {
  const auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : &it->second;
}

OrderRecord* OrderIndex::by_internal(ClientOrderId id) noexcept {
  const auto it = by_internal_.find(id.value);
  return it == by_internal_.end() ? nullptr : by_order_id(it->second);
}

OrderRecord* OrderIndex::by_client_id(AccountId account,
                                      std::uint32_t symbol,
                                      std::string_view client_order_id) noexcept {
  const auto it = by_client_.find(client_key(account, symbol, client_order_id));
  return it == by_client_.end() ? nullptr : by_order_id(it->second);
}

void OrderIndex::rekey_internal(OrderRecord& r, ClientOrderId new_internal) {
  by_internal_.erase(r.internal.value);
  r.internal = new_internal;
  by_internal_[new_internal.value] = r.order_id;
}

void OrderIndex::rekey_client(OrderRecord& r, std::string new_client_order_id) {
  by_client_.erase(client_key(r.account, r.symbol, r.client_order_id));
  r.client_order_id = std::move(new_client_order_id);
  by_client_[client_key(r.account, r.symbol, r.client_order_id)] = r.order_id;
}

std::size_t OrderIndex::erase_terminal() {
  std::size_t n = 0;
  for (auto it = orders_.begin(); it != orders_.end();) {
    const OrderRecord& r = it->second;
    if (!r.terminal()) {
      ++it;
      continue;
    }
    if (const auto bi = by_internal_.find(r.internal.value);
        bi != by_internal_.end() && bi->second == r.order_id)
      by_internal_.erase(bi);
    if (const auto bc = by_client_.find(client_key(r.account, r.symbol, r.client_order_id));
        bc != by_client_.end() && bc->second == r.order_id)
      by_client_.erase(bc);
    it = orders_.erase(it);
    ++n;
  }
  return n;
}

std::vector<OrderRecord*> OrderIndex::open_orders(AccountId account, std::int64_t symbol) {
  std::vector<OrderRecord*> out;
  out.reserve(orders_.size());
  for (auto& [id, r] : orders_) {
    if (r.account != account || r.terminal()) continue;
    if (symbol >= 0 && static_cast<std::int64_t>(r.symbol) != symbol) continue;
    out.push_back(&r);
  }
  return out;
}

std::size_t OrderIndex::open_count(AccountId account) const noexcept {
  std::size_t n = 0;
  for (const auto& [id, r] : orders_) {
    if (r.account == account && !r.terminal()) ++n;
  }
  return n;
}

}  // namespace fastmm::sim::server
