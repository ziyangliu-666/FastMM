#include "fastmm/venues/coinbase/coinbase_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

namespace fastmm::venues::coinbase {

namespace {

// Appends `s` to buf at n; false when it does not fit.
bool append(char* buf, std::size_t cap, std::size_t& n, std::string_view s) noexcept {
  if (s.size() > cap - n) return false;
  for (const char c : s) buf[n++] = c;
  return true;
}

}  // namespace

bool CoinbaseOrderEncoder::encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept {
  if (cmd.kind != OrderCommandKind::New) return false;
  const std::string_view product = symbols_.venue_symbol(cmd.instrument);
  if (product.empty()) return false;
  out.method = "POST";
  out.path_n = 0;
  if (!append(out.path.data(), out.path.size(), out.path_n, "/orders")) return false;
  JsonWriter w(std::span<char>(out.body.data(), out.body.size()));
  const ClientOid oid = encode_client_oid(cmd.cl_ord_id);
  w.begin_object().key("client_oid").string(oid.view());
  w.key("product_id").string(product);
  w.key("side").string(cmd.side == Side::Buy ? "buy" : "sell");
  const DecimalText size(cmd.qty);
  if (cmd.type == OrderType::Market) {
    w.key("type").string("market").key("size").string(size.view());
  } else {
    const DecimalText price(cmd.price);
    w.key("type").string("limit").key("price").string(price.view());
    w.key("size").string(size.view());
    std::string_view tif = "GTC";
    if (cmd.type != OrderType::PostOnly) {
      if (cmd.tif == TimeInForce::Ioc) tif = "IOC";
      if (cmd.tif == TimeInForce::Fok) tif = "FOK";
    }
    w.key("time_in_force").string(tif);
    if (cmd.type == OrderType::PostOnly) w.key("post_only").boolean(true);
  }
  w.key("stp").string(to_string(stp_));
  w.end_object();
  if (!w.ok()) return false;
  out.body_n = w.size();
  return true;
}

bool CoinbaseOrderEncoder::encode_cancel(const OrderCommand& cmd,
                                         OrderRequest& out) const noexcept {
  if (cmd.kind != OrderCommandKind::Cancel) return false;
  const std::string_view product = symbols_.venue_symbol(cmd.instrument);
  if (product.empty()) return false;
  out.method = "DELETE";
  out.path_n = 0;
  out.body_n = 0;
  const ClientOid oid = encode_client_oid(cmd.cl_ord_id);
  char* p = out.path.data();
  const std::size_t cap = out.path.size();
  // The product in the query avoids a race the venue documents for cancels without it.
  return append(p, cap, out.path_n, "/orders/client:") && append(p, cap, out.path_n, oid.view()) &&
         append(p, cap, out.path_n, "?product_id=") && append(p, cap, out.path_n, product);
}

std::string CoinbaseOrderEncoder::open_orders_path(std::string_view after) {
  std::string p = "/orders?status=open&status=pending&status=active&limit=1000";
  if (!after.empty()) p.append("&after=").append(after);
  return p;
}

std::string CoinbaseOrderEncoder::fills_path(std::string_view product,
                                             std::int64_t start_ms,
                                             std::int64_t end_ms,
                                             std::string_view after,
                                             int limit) {
  std::string p = "/fills?product_id=";
  p.append(product);
  if (start_ms > 0) p.append("&start_date=").append(format_time_ms(start_ms).view());
  if (end_ms > 0) p.append("&end_date=").append(format_time_ms(end_ms).view());
  if (!after.empty()) p.append("&after=").append(after);
  p.append("&limit=").append(std::to_string(limit));
  return p;
}

std::string CoinbaseOrderEncoder::order_path(std::string_view order_id) {
  return "/orders/" + std::string(order_id);
}

std::string CoinbaseOrderEncoder::cancel_all_path(std::string_view product) {
  return "/orders?product_id=" + std::string(product);
}

}  // namespace fastmm::venues::coinbase
