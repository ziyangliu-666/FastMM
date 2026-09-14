#include "fastmm/venues/deribit/deribit_order_encoder.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <cstdint>

namespace fastmm::venues::deribit {

namespace {

JsonWriter& begin_request(JsonWriter& w, std::int64_t id, std::string_view method) noexcept {
  w.begin_object().key("jsonrpc").string("2.0").key("id").integer(id);
  w.key("method").string(method).key("params").begin_object();
  return w;
}
JsonWriter& begin_request(JsonWriter& w, std::string_view id, std::string_view method) noexcept {
  w.begin_object().key("jsonrpc").string("2.0").key("id").string(id);
  w.key("method").string(method).key("params").begin_object();
  return w;
}
std::size_t end_request(JsonWriter& w, std::string_view access_token) noexcept {
  if (!access_token.empty()) w.key("access_token").string(access_token);
  w.end_object().end_object();
  return w.ok() ? w.size() : 0;
}
JsonWriter& number(JsonWriter& w, std::string_view key, Price v) noexcept {
  const DecimalText t(v);
  return w.key(key).raw_value(t.view());
}
JsonWriter& number(JsonWriter& w, std::string_view key, Qty v) noexcept {
  const DecimalText t(v);
  return w.key(key).raw_value(t.view());
}

}  // namespace

Price DeribitOrderEncoder::venue_price(InstrumentId id, Price px, Side side) const noexcept {
  if (id.value < ticks_.size() && ticks_[id.value].base.is_positive())
    return ticks_[id.value].round(px, side);
  if (instruments_.contains(id)) return instruments_.get(id).round_price(px, side);
  return px;
}

std::size_t DeribitOrderEncoder::encode(const OrderCommand& cmd,
                                        const OrderShadow* shadow,
                                        std::string_view access_token,
                                        std::span<char> out) const noexcept {
  JsonWriter w(out);
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
      if (symbol.empty() || !cmd.qty.is_positive()) return 0;
      const RequestId rid = make_request_id(RequestKind::New, cmd.cl_ord_id);
      begin_request(w, rid.view(), cmd.side == Side::Buy ? "private/buy" : "private/sell");
      w.key("instrument_name").string(symbol);
      number(w, "contracts", cmd.qty);
      if (cmd.type == OrderType::Market) {
        w.key("type").string("market");
        w.key("post_only").boolean(false);  // documented default is true
      } else {
        const bool post_only = cmd.type == OrderType::PostOnly;
        w.key("type").string("limit");
        number(w, "price", venue_price(cmd.instrument, cmd.price, cmd.side));
        // post_only is "only valid in combination with time_in_force=good_til_cancelled".
        w.key("time_in_force").string(tif_text(post_only ? TimeInForce::Gtc : cmd.tif));
        w.key("post_only").boolean(post_only);
        if (post_only && reject_post_only_) w.key("reject_post_only").boolean(true);
      }
      w.key("label").string(encode_cl_ord_id(cmd.cl_ord_id).view());
      if (cmd.reduce_only) w.key("reduce_only").boolean(true);
      return end_request(w, access_token);
    }
    case OrderCommandKind::Cancel: {
      const RequestId rid = make_request_id(RequestKind::Cancel, cmd.cl_ord_id);
      std::string_view order_id;
      if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty()) {
        order_id = cmd.venue_order_id->view();
      } else if (shadow != nullptr && !shadow->venue_order_id.empty()) {
        order_id = shadow->venue_order_id.view();
      }
      if (!order_id.empty()) {
        begin_request(w, rid.view(), "private/cancel");
        w.key("order_id").string(order_id);
        return end_request(w, access_token);
      }
      // Not acked yet: the label is the only handle the venue has.
      const ClientOrderId label =
          shadow != nullptr && shadow->label.valid() ? shadow->label : cmd.cl_ord_id;
      begin_request(w, rid.view(), "private/cancel_by_label");
      w.key("label").string(encode_cl_ord_id(label).view());
      const InstrumentId inst = shadow != nullptr ? shadow->instrument : cmd.instrument;
      if (instruments_.contains(inst) && !instruments_.get(inst).base.empty())
        w.key("currency").string(instruments_.get(inst).base.view());
      return end_request(w, access_token);
    }
    case OrderCommandKind::Replace: {
      if (shadow == nullptr) return 0;
      std::string_view order_id;
      if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty()) {
        order_id = cmd.venue_order_id->view();
      } else {
        order_id = shadow->venue_order_id.view();
      }
      if (order_id.empty() || !cmd.qty.is_positive()) return 0;
      const RequestId rid = make_request_id(RequestKind::Replace, cmd.cl_ord_id);
      begin_request(w, rid.view(), "private/edit");
      w.key("order_id").string(order_id);
      number(w, "contracts", cmd.qty);
      number(w, "price", venue_price(shadow->instrument, cmd.price, shadow->side));
      const bool post_only = shadow->type == OrderType::PostOnly;
      w.key("post_only").boolean(post_only);
      if (post_only && reject_post_only_) w.key("reject_post_only").boolean(true);
      return end_request(w, access_token);
    }
  }
  return 0;
}

std::size_t DeribitOrderEncoder::encode_auth(std::int64_t id,
                                             std::string_view client_id,
                                             std::string_view client_secret,
                                             std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "public/auth");
  w.key("grant_type").string("client_credentials");
  w.key("client_id").string(client_id).key("client_secret").string(client_secret);
  return end_request(w, {});
}

std::size_t DeribitOrderEncoder::encode_auth_refresh(std::int64_t id,
                                                     std::string_view refresh_token,
                                                     std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "public/auth");
  w.key("grant_type").string("refresh_token").key("refresh_token").string(refresh_token);
  return end_request(w, {});
}

std::size_t DeribitOrderEncoder::encode_set_heartbeat(std::int64_t id,
                                                      std::int64_t interval_s,
                                                      std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "public/set_heartbeat");
  w.key("interval").integer(interval_s < 10 ? 10 : interval_s);
  return end_request(w, {});
}

std::size_t DeribitOrderEncoder::encode_test(std::int64_t id, std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "public/test");
  return end_request(w, {});
}

std::size_t DeribitOrderEncoder::encode_subscribe(std::int64_t id,
                                                  bool is_private,
                                                  std::span<const std::string> channels,
                                                  std::string_view access_token,
                                                  std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, is_private ? "private/subscribe" : "public/subscribe");
  w.key("channels").begin_array();
  for (const std::string& c : channels) w.string(c);
  w.end_array();
  return end_request(w, is_private ? access_token : std::string_view{});
}

std::size_t DeribitOrderEncoder::encode_enable_cancel_on_disconnect(std::int64_t id,
                                                                    std::string_view access_token,
                                                                    std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "private/enable_cancel_on_disconnect");
  w.key("scope").string("connection");
  return end_request(w, access_token);
}

std::size_t DeribitOrderEncoder::encode_open_orders(std::int64_t id,
                                                    std::string_view currency,
                                                    std::string_view access_token,
                                                    std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "private/get_open_orders_by_currency");
  w.key("currency").string(currency);
  return end_request(w, access_token);
}

std::size_t DeribitOrderEncoder::encode_cancel_all_by_instrument(std::int64_t id,
                                                                 std::string_view instrument,
                                                                 std::string_view access_token,
                                                                 std::span<char> out) noexcept {
  JsonWriter w(out);
  begin_request(w, id, "private/cancel_all_by_instrument");
  w.key("instrument_name").string(instrument);
  return end_request(w, access_token);
}

std::string DeribitOrderEncoder::rest_cancel_all_target(std::string_view instrument) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string t = "/private/cancel_all_by_instrument?instrument_name=";
  for (const char c : instrument) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
      t += c;
    } else {
      const auto u = static_cast<unsigned char>(c);
      t += '%';
      t += kHex[u >> 4];
      t += kHex[u & 0xF];
    }
  }
  return t;
}

std::string DeribitOrderEncoder::basic_auth_header(const Credentials& c) {
  std::string raw = c.client_id;
  raw += ':';
  raw += c.client_secret.value;
  std::string b64(net::base64_encoded_size(raw.size()), '\0');
  const std::size_t n = net::base64_encode(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()),
      std::span<char>(b64.data(), b64.size()));
  b64.resize(n);
  for (char& ch : raw) ch = '\0';
  return "Authorization: Basic " + b64 + "\r\n";
}

}  // namespace fastmm::venues::deribit
