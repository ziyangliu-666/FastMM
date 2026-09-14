#include "fastmm/venues/deribit/deribit_private_parser.hpp"

#include "fastmm/venues/deribit/deribit_json.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <utility>

namespace fastmm::venues::deribit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct DeribitPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

DeribitPrivateParser::DeribitPrivateParser(const SymbolTable& symbols,
                                           const InstrumentTable& instruments,
                                           VenueId venue,
                                           std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
DeribitPrivateParser::~DeribitPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

template <class F, class V>
bool fixed_of(V&& v, F& out) noexcept {
  std::string_view tok;
  if (std::forward<V>(v).raw_json_token().get(tok) != sj::SUCCESS) return false;
  const auto f = json_fixed<F>(tok);
  if (!f) return false;
  out = *f;
  return true;
}

void stamp(EventHeader& h, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  h.recv_ts = recv_ts;
  h.t0_cycles = t0;
  h.exch_ts = Timestamp{exch_ms * 1'000'000};
}

void read_order_result(od::object& o, OrderResult& out) noexcept {
  out.present = true;
  std::string_view s;
  if (o["order_id"].get_string().get(s) == sj::SUCCESS) out.order_id = s;
  if (o["order_state"].get_string().get(s) == sj::SUCCESS) out.order_state = s;
  if (o["label"].get_string().get(s) == sj::SUCCESS) out.label = s;
  if (o["instrument_name"].get_string().get(s) == sj::SUCCESS) out.instrument_name = s;
  static_cast<void>(fixed_of(o["amount"], out.amount));
  static_cast<void>(fixed_of(o["filled_amount"], out.filled_amount));
}

}  // namespace

PrivateDecodeResult DeribitPrivateParser::decode(std::string_view json,
                                                 Timestamp recv_ts,
                                                 Cycles t0,
                                                 std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  auto malformed = [&]() {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    r.len = 0;
    return r;
  };
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed();

  std::string_view method;
  if (root["method"].get_string().get(method) == sj::SUCCESS) {
    if (method == "heartbeat") {
      root.reset();
      od::object params;
      std::string_view type;
      const bool test = root["params"].get_object().get(params) == sj::SUCCESS &&
                        params["type"].get_string().get(type) == sj::SUCCESS &&
                        type == "test_request";
      ++stats_.heartbeats;
      r.frame = test ? FrameKind::TestRequest : FrameKind::Heartbeat;
      r.status = ParseStatus::Ignored;
      return r;
    }
    if (method != "subscription") {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    r.frame = FrameKind::Notification;
    root.reset();
    od::object params;
    if (root["params"].get_object().get(params) != sj::SUCCESS) return malformed();
    std::string_view channel;
    if (params["channel"].get_string().get(channel) != sj::SUCCESS) return malformed();
    const bool is_orders = channel.starts_with("user.orders.");
    const bool is_trades = channel.starts_with("user.trades.");
    if (!is_orders && !is_trades) {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    od::value data;
    if (params["data"].get(data) != sj::SUCCESS) return malformed();

    std::uint32_t written = 0;
    std::uint32_t count = 0;
    bool bad = false;
    auto room = [&](std::size_t n) {
      if (written + n <= out.size()) return true;
      ++stats_.overflow;
      return false;
    };
    // One order or trade object -> at most one event.
    auto one = [&](od::object& o) {
      std::string_view instrument_name;
      std::string_view order_id;
      std::string_view label;
      if (o["instrument_name"].get_string().get(instrument_name) != sj::SUCCESS ||
          o["order_id"].get_string().get(order_id) != sj::SUCCESS) {
        bad = true;
        return;
      }
      if (o["label"].get_string().get(label) != sj::SUCCESS) label = {};
      const InstrumentId inst = symbols_.find(venue_, instrument_name);
      if (!inst.valid() || !instruments_.contains(inst)) {
        ++stats_.unknown_symbol;
        return;
      }
      const Qty csize = instruments_.get(inst).contract_multiplier;
      ClientOrderId cl{};
      if (const auto d = decode_cl_ord_id(label)) {
        cl = *d;
      } else {
        ++stats_.foreign_ids;
      }

      if (is_trades) {
        std::string_view trade_id;
        std::string_view direction;
        std::string_view liquidity;
        std::int64_t ts = 0;
        Price px{};
        Qty amount{};
        Notional fee{};
        if (o["trade_id"].get_string().get(trade_id) != sj::SUCCESS ||
            o["direction"].get_string().get(direction) != sj::SUCCESS ||
            !fixed_of(o["price"], px) || !fixed_of(o["amount"], amount)) {
          bad = true;
          return;
        }
        if (o["timestamp"].get_int64().get(ts) != sj::SUCCESS) ts = 0;
        if (o["liquidity"].get_string().get(liquidity) != sj::SUCCESS) liquidity = {};
        static_cast<void>(fixed_of(o["fee"], fee));
        if (!room(sizeof(OrderFillMsg))) return;
        auto* m = reinterpret_cast<OrderFillMsg*>(out.data() + written);
        init_header(*m, EventType::OrderFill, inst, venue_);
        m->cl_ord_id = cl;
        m->venue_order_id.assign(order_id);
        m->exec_id.assign(trade_id);
        m->price = px;
        m->qty = amount_to_contracts(amount, csize);
        m->cum_qty = Qty{};
        m->leaves_qty = Qty{};
        m->fee = fee;
        m->side = direction == "sell" ? Side::Sell : Side::Buy;
        m->liquidity = Liquidity::Unknown;
        if (liquidity == "M") m->liquidity = Liquidity::Maker;
        if (liquidity == "T") m->liquidity = Liquidity::Taker;
        stamp(m->hdr, recv_ts, t0, ts);
        written += sizeof(OrderFillMsg);
        ++count;
        ++stats_.trades;
        return;
      }

      std::string_view state;
      if (o["order_state"].get_string().get(state) != sj::SUCCESS) {
        bad = true;
        return;
      }
      Qty filled{};
      static_cast<void>(fixed_of(o["filled_amount"], filled));
      std::int64_t ts = 0;
      if (o["last_update_timestamp"].get_int64().get(ts) != sj::SUCCESS) ts = 0;
      ++stats_.orders;
      if (state == "open") {
        if (!room(sizeof(OrderAckMsg))) return;
        auto* m = reinterpret_cast<OrderAckMsg*>(out.data() + written);
        init_header(*m, EventType::OrderAck, inst, venue_);
        m->cl_ord_id = cl;
        m->venue_order_id.assign(order_id);
        stamp(m->hdr, recv_ts, t0, ts);
        written += sizeof(OrderAckMsg);
        ++count;
      } else if (state == "cancelled") {
        if (!room(sizeof(OrderCancelAckMsg))) return;
        auto* m = reinterpret_cast<OrderCancelAckMsg*>(out.data() + written);
        init_header(*m, EventType::OrderCancelAck, inst, venue_);
        m->cl_ord_id = cl;
        m->venue_order_id.assign(order_id);
        m->cum_qty = amount_to_contracts(filled, csize);
        stamp(m->hdr, recv_ts, t0, ts);
        written += sizeof(OrderCancelAckMsg);
        ++count;
      } else if (state == "rejected") {
        if (!room(sizeof(OrderRejectMsg))) return;
        auto* m = reinterpret_cast<OrderRejectMsg*>(out.data() + written);
        init_header(*m, EventType::OrderReject, inst, venue_);
        m->cl_ord_id = cl;
        m->reason = RejectReason::VenueReject;
        m->venue_code = 0;
        m->text.assign("rejected");
        stamp(m->hdr, recv_ts, t0, ts);
        written += sizeof(OrderRejectMsg);
        ++count;
      } else {
        ++stats_.ignored;  // filled (fills via user.trades), untriggered, triggered
      }
    };

    od::json_type type{};
    if (data.type().get(type) != sj::SUCCESS) return malformed();
    if (type == od::json_type::object) {
      od::object o;
      if (data.get_object().get(o) != sj::SUCCESS) return malformed();
      one(o);
    } else if (type == od::json_type::array) {
      od::array arr;
      if (data.get_array().get(arr) != sj::SUCCESS) return malformed();
      for (auto item : arr) {
        od::object o;
        if (item.get_object().get(o) != sj::SUCCESS) return malformed();
        one(o);
        if (bad) break;
      }
    } else {
      return malformed();
    }
    if (bad) return malformed();
    if (count == 0) {
      r.status = ParseStatus::Ignored;
      return r;
    }
    r.status = ParseStatus::Ok;
    r.order_kind = is_trades ? OrderEventKind::Fill : OrderEventKind::Ack;
    r.len = written;
    r.count = count;
    return r;
  }

  // JSON-RPC response.
  root.reset();
  {
    od::value idv;
    if (root["id"].get(idv) != sj::SUCCESS) {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    std::int64_t num = 0;
    std::string_view text;
    if (idv.get_int64().get(num) == sj::SUCCESS) {
      r.rpc.id = num;
    } else if (idv.get_string().get(text) == sj::SUCCESS) {
      r.rpc.id_text = text;
    }
  }
  r.frame = FrameKind::Response;
  ++stats_.responses;
  root.reset();
  {
    od::object err;
    if (root["error"].get_object().get(err) == sj::SUCCESS) {
      r.rpc.is_error = true;
      std::int64_t code = 0;
      if (err["code"].get_int64().get(code) == sj::SUCCESS) r.rpc.error_code = code;
      std::string_view msg;
      if (err["message"].get_string().get(msg) == sj::SUCCESS) r.rpc.error_message = msg;
      od::object data;
      std::string_view reason;
      if (err["data"].get_object().get(data) == sj::SUCCESS &&
          data["reason"].get_string().get(reason) == sj::SUCCESS)
        r.rpc.error_reason = reason;
      r.status = ParseStatus::Error;
      return r;
    }
  }
  root.reset();
  od::value result;
  if (root["result"].get(result) != sj::SUCCESS) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  od::json_type type{};
  if (result.type().get(type) != sj::SUCCESS) return malformed();
  switch (type) {
    case od::json_type::object: {
      od::object ro;
      if (result.get_object().get(ro) != sj::SUCCESS) return malformed();
      od::object order;
      if (ro["order"].get_object().get(order) == sj::SUCCESS) {
        read_order_result(order, r.order);  // private/buy, private/sell, private/edit
        break;
      }
      ro.reset();
      std::string_view s;
      if (ro["order_id"].get_string().get(s) == sj::SUCCESS) {
        ro.reset();
        read_order_result(ro, r.order);  // private/cancel returns the order itself
        break;
      }
      ro.reset();
      if (ro["access_token"].get_string().get(s) == sj::SUCCESS) {
        r.auth.present = true;
        r.auth.access_token = s;
        ro.reset();
        if (ro["refresh_token"].get_string().get(s) == sj::SUCCESS) r.auth.refresh_token = s;
        ro.reset();
        std::int64_t e = 0;
        if (ro["expires_in"].get_int64().get(e) == sj::SUCCESS) r.auth.expires_in = e;
      }
      break;
    }
    case od::json_type::array: {
      od::array arr;
      if (result.get_array().get(arr) != sj::SUCCESS) return malformed();
      std::uint32_t n = 0;
      for (auto item : arr) {
        static_cast<void>(item);
        ++n;
      }
      r.result_items = n;
      break;
    }
    case od::json_type::number: {
      std::int64_t v = 0;
      if (result.get_int64().get(v) == sj::SUCCESS) r.result_int = v;
      break;
    }
    default:
      break;
  }
  r.status = ParseStatus::Ignored;
  return r;
}

ParseStatus DeribitPrivateParser::decode_open_orders(
    std::string_view json, const std::function<void(const OpenOrderRecord&)>& fn) noexcept {
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  {
    od::value err;
    if (root["error"].get(err) == sj::SUCCESS) return ParseStatus::Error;
  }
  root.reset();
  od::array list;
  if (root["result"].get_array().get(list) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto item : list) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    OpenOrderRecord rec;
    if (o["instrument_name"].get_string().get(rec.instrument_name) != sj::SUCCESS ||
        o["order_id"].get_string().get(rec.order_id) != sj::SUCCESS ||
        o["direction"].get_string().get(rec.direction) != sj::SUCCESS)
      return ParseStatus::Malformed;
    if (o["label"].get_string().get(rec.label) != sj::SUCCESS) rec.label = {};
    if (o["order_state"].get_string().get(rec.order_state) != sj::SUCCESS) rec.order_state = {};
    if (!fixed_of(o["price"], rec.price)) rec.price = Price{};  // "market_price" for triggers
    if (!fixed_of(o["amount"], rec.amount)) return ParseStatus::Malformed;
    if (!fixed_of(o["filled_amount"], rec.filled_amount)) rec.filled_amount = Qty{};
    fn(rec);
  }
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::deribit
