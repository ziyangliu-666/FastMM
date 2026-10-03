#include "fastmm/venues/gate/gate_private_parser.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/gate/gate_error_map.hpp"
#include "fastmm/venues/gate/gate_order_encoder.hpp"
#include "fastmm/venues/gate/gate_rest_decoder.hpp"

#include <simdjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fastmm::venues::gate {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct GatePrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

GatePrivateParser::GatePrivateParser(const SymbolTable& symbols,
                                     const InstrumentTable& instruments,
                                     VenueId venue,
                                     std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
GatePrivateParser::~GatePrivateParser() = default;

std::optional<ClientOrderId> cl_ord_id_of_text(std::string_view text) noexcept {
  if (!text.starts_with(kTextPrefix)) return std::nullopt;
  return decode_cl_ord_id(text.substr(kTextPrefix.size()));
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
  m.hdr.exch_ts = ts_from_ms(exch_ms);
}

// A number Gate sends as a JSON number (int or float) or as a string, as text in `buf`.
std::string_view number_text(od::value v, char* buf, std::size_t cap) noexcept {
  std::string_view s;
  if (v.get_string().get(s) == sj::SUCCESS) return s;
  std::int64_t i = 0;
  if (v.get_int64().get(i) == sj::SUCCESS) return std::string_view(buf, format_int64(i, buf));
  double d = 0.0;
  if (v.get_double().get(d) != sj::SUCCESS) return {};
  const int n = std::snprintf(buf, cap, "%.8f", d);
  return n > 0 ? std::string_view(buf, static_cast<std::size_t>(n)) : std::string_view{};
}

bool signed_size(od::value v, Qty& mag, bool& positive) noexcept {
  char buf[48];
  const std::string_view t = number_text(v, buf, sizeof buf);
  return parse_signed_size(t, mag, positive);
}

std::int64_t int_of(od::value v) noexcept {
  std::int64_t i = 0;
  if (v.get_int64().get(i) == sj::SUCCESS) return i;
  double d = 0.0;
  if (v.get_double().get(d) == sj::SUCCESS) return static_cast<std::int64_t>(d);
  std::string_view s;
  if (v.get_string().get(s) == sj::SUCCESS) {
    if (const auto p = parse_int64(s)) return *p;
  }
  return 0;
}

PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

[[gnu::noinline]] PrivateDecodeResult decode_control(PrivateParserStats& stats,
                                                     od::object& root,
                                                     std::string_view channel,
                                                     std::string_view event,
                                                     PrivateDecodeResult r) noexcept {
  ++stats.control;
  r.channel = channel;
  r.control = channel == "futures.pong"     ? ControlOp::Pong
              : channel == "futures.system" ? ControlOp::System
              : event == "subscribe"        ? ControlOp::Subscribe
              : event == "unsubscribe"      ? ControlOp::Unsubscribe
              : event == "api"              ? ControlOp::Api
                                            : ControlOp::Other;
  r.control_success = true;
  root.reset();
  od::value err;
  if (root["error"].get(err) == sj::SUCCESS) {
    od::object eo;
    if (err.get_object().get(eo) == sj::SUCCESS) {
      std::int64_t code = 0;
      if (eo["code"].get_int64().get(code) == sj::SUCCESS) r.error_code = code;
      std::string_view msg;
      if (eo["message"].get_string().get(msg) == sj::SUCCESS) r.error = msg;
      r.control_success = false;
    }
  }
  r.status = r.control_success ? ParseStatus::Ignored : ParseStatus::Error;
  return r;
}

struct ItemCtx {
  PrivateParserStats* stats;
  const SymbolTable* symbols;
  const InstrumentTable* instruments;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::int64_t time_ms;  // the frame's time_ms
  std::span<std::byte> out;
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  bool balance_changed = false;

  bool room(std::size_t n) noexcept {
    if (written + n <= out.size()) return true;
    ++stats->overflow;
    return false;
  }
  // The output buffer is reused for every frame: each message is zeroed before it is filled.
  template <class M>
  M* place() noexcept {
    std::byte* p = out.data() + written;
    std::memset(p, 0, sizeof(M));
    return reinterpret_cast<M*>(p);
  }
};

enum class Item : std::uint8_t { Next, Stop, Malformed };

// One futures.orders item.
[[gnu::noinline]] Item decode_order(ItemCtx& c, od::object& o) noexcept {
  std::string_view contract;
  std::string_view text;
  std::string_view status;
  std::string_view finish_as;
  char id_buf[24];
  std::string_view id;
  Qty size{};
  Qty left{};
  bool bid = true;
  bool left_pos = true;
  std::int64_t create_ms = 0;
  std::int64_t finish_ms = 0;
  std::int64_t update_ms = 0;
  bool have_size = false;
  bool have_left = false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return Item::Malformed;
    od::value v = field.value();
    if (key == "contract") {
      if (v.get_string().get(contract) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "id") {
      id = number_text(v, id_buf, sizeof id_buf);
    } else if (key == "id_string") {
      std::string_view s;
      if (v.get_string().get(s) == sj::SUCCESS) id = s;
    } else if (key == "text") {
      if (v.get_string().get(text) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "status") {
      if (v.get_string().get(status) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "finish_as") {
      if (v.get_string().get(finish_as) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "size") {
      if (!signed_size(v, size, bid)) return Item::Malformed;
      have_size = true;
    } else if (key == "left") {
      if (!signed_size(v, left, left_pos)) return Item::Malformed;
      have_left = true;
    } else if (key == "create_time_ms") {
      create_ms = int_of(v);
    } else if (key == "finish_time_ms") {
      finish_ms = int_of(v);
    } else if (key == "update_time") {
      update_ms = int_of(v);
    }
  }
  if (contract.empty() || id.empty() || status.empty() || !have_size || !have_left)
    return Item::Malformed;
  ++c.stats->orders;
  const InstrumentId inst = c.symbols->find(c.venue, contract);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  ClientOrderId cl{};
  if (const auto d = cl_ord_id_of_text(text)) {
    cl = *d;
  } else {
    ++c.stats->foreign_ids;
  }
  const Qty cum = size - left;
  const std::int64_t exch_ms =
      update_ms != 0 ? update_ms : (finish_ms != 0 ? finish_ms : c.time_ms);
  if (status == "open") {
    if (left != size || !finish_as.empty()) {
      ++c.stats->ignored;  // a part filled: the fill came on usertrades
      return Item::Next;
    }
    if (!c.room(sizeof(OrderAckMsg))) return Item::Stop;
    auto* m = c.place<OrderAckMsg>();
    init_header(*m, EventType::OrderAck, inst, c.venue);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(id);
    stamp(*m, c.recv_ts, c.t0, create_ms != 0 ? create_ms : exch_ms);
    c.written += sizeof(OrderAckMsg);
    ++c.count;
    return Item::Next;
  }
  if (status != "finished") {
    ++c.stats->ignored;
    return Item::Next;
  }
  if (finish_as == "filled" || left.is_zero()) {
    ++c.stats->ignored;  // the usertrades deliver the fills
    return Item::Next;
  }
  if (finish_as_expired(finish_as)) {
    if (!c.room(sizeof(OrderExpiredMsg))) return Item::Stop;
    auto* m = c.place<OrderExpiredMsg>();
    init_header(*m, EventType::OrderExpired, inst, c.venue);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(id);
    m->cum_qty = cum;
    stamp(*m, c.recv_ts, c.t0, exch_ms);
    c.written += sizeof(OrderExpiredMsg);
    ++c.count;
    return Item::Next;
  }
  // cancelled (and anything else the venue ends an order with)
  if (!c.room(sizeof(OrderCancelAckMsg))) return Item::Stop;
  auto* m = c.place<OrderCancelAckMsg>();
  init_header(*m, EventType::OrderCancelAck, inst, c.venue);
  m->cl_ord_id = cl;
  m->venue_order_id.assign(id);
  m->cum_qty = cum;
  stamp(*m, c.recv_ts, c.t0, exch_ms);
  c.written += sizeof(OrderCancelAckMsg);
  ++c.count;
  return Item::Next;
}

// One futures.usertrades item.
[[gnu::noinline]] Item decode_trade(ItemCtx& c, od::object& o) noexcept {
  std::string_view contract;
  std::string_view text;
  std::string_view role;
  std::string_view price_s;
  char id_buf[24];
  char oid_buf[24];
  char fee_buf[48];
  std::string_view id;
  std::string_view order_id;
  std::string_view fee_s;
  Qty size{};
  bool bid = true;
  std::int64_t time_ms = 0;
  bool have_size = false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return Item::Malformed;
    od::value v = field.value();
    if (key == "contract") {
      if (v.get_string().get(contract) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "id") {
      id = number_text(v, id_buf, sizeof id_buf);
    } else if (key == "order_id") {
      order_id = number_text(v, oid_buf, sizeof oid_buf);
    } else if (key == "text") {
      if (v.get_string().get(text) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "role") {
      if (v.get_string().get(role) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "price") {
      if (v.get_string().get(price_s) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "size") {
      if (!signed_size(v, size, bid)) return Item::Malformed;
      have_size = true;
    } else if (key == "fee") {
      fee_s = number_text(v, fee_buf, sizeof fee_buf);
    } else if (key == "create_time_ms") {
      time_ms = int_of(v);
    }
  }
  if (contract.empty() || id.empty() || order_id.empty() || price_s.empty() || !have_size)
    return Item::Malformed;
  ++c.stats->trades;
  const InstrumentId inst = c.symbols->find(c.venue, contract);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  const auto px = parse_price(price_s);
  if (!px) return Item::Malformed;
  ClientOrderId cl{};
  if (const auto d = cl_ord_id_of_text(text)) {
    cl = *d;
  } else {
    ++c.stats->foreign_ids;
  }
  if (!c.room(sizeof(OrderFillMsg))) return Item::Stop;
  auto* m = c.place<OrderFillMsg>();
  init_header(*m, EventType::OrderFill, inst, c.venue);
  m->cl_ord_id = cl;
  m->venue_order_id.assign(order_id);
  m->exec_id.assign(id);
  m->price = *px;
  m->qty = size;
  // cum_qty / leaves_qty: the connector fills them from its shadow (the trade does not say).
  if (!fee_s.empty()) {
    if (const auto f = parse_fee(fee_s)) m->fee = *f;  // positive paid; a rebate is negative
  }
  m->fee_asset = FeeAsset::Quote;  // the settle currency
  m->side = bid ? Side::Buy : Side::Sell;
  m->liquidity = role == "maker" ? Liquidity::Maker : Liquidity::Taker;
  stamp(*m, c.recv_ts, c.t0, time_ms != 0 ? time_ms : c.time_ms);
  c.written += sizeof(OrderFillMsg);
  ++c.count;
  return Item::Next;
}

// One futures.positions item (single mode only).
[[gnu::noinline]] Item decode_position(ItemCtx& c, od::object& o) noexcept {
  std::string_view contract;
  std::string_view mode;
  Qty size{};
  bool positive = true;
  Price entry{};
  std::int64_t time_ms = 0;
  bool have_size = false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return Item::Malformed;
    od::value v = field.value();
    if (key == "contract") {
      if (v.get_string().get(contract) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "mode") {
      if (v.get_string().get(mode) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "size") {
      if (!signed_size(v, size, positive)) return Item::Malformed;
      have_size = true;
    } else if (key == "entry_price") {
      char buf[48];
      const std::string_view t = number_text(v, buf, sizeof buf);
      if (!t.empty()) {
        if (const auto p = parse_avg_price(t)) entry = *p;
      }
    } else if (key == "time_ms") {
      time_ms = int_of(v);
    }
  }
  if (contract.empty() || !have_size) return Item::Malformed;
  ++c.stats->positions;
  if (!mode.empty() && mode != "single") {
    ++c.stats->dual_positions;
    return Item::Next;
  }
  const InstrumentId inst = c.symbols->find(c.venue, contract);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  if (!c.room(sizeof(PositionUpdateMsg))) return Item::Stop;
  auto* m = c.place<PositionUpdateMsg>();
  init_header(*m, EventType::PositionUpdate, inst, c.venue);
  m->qty = positive ? size : -size;
  m->avg_px = entry;
  stamp(*m, c.recv_ts, c.t0, time_ms != 0 ? time_ms : c.time_ms);
  c.written += sizeof(PositionUpdateMsg);
  ++c.count;
  return Item::Next;
}

// One futures.balances item: {balance, change, text, time_ms, type, currency}.
[[gnu::noinline]] Item decode_balance(ItemCtx& c, od::object& o) noexcept {
  std::string_view type;
  std::string_view text;
  std::string_view currency;
  char change_buf[48];
  std::string_view change;
  std::int64_t time_ms = 0;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return Item::Malformed;
    od::value v = field.value();
    if (key == "type") {
      if (v.get_string().get(type) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "text") {
      if (v.get_string().get(text) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "currency") {
      if (v.get_string().get(currency) != sj::SUCCESS) return Item::Malformed;
    } else if (key == "change") {
      change = number_text(v, change_buf, sizeof change_buf);
    } else if (key == "time_ms") {
      time_ms = int_of(v);
    }
  }
  ++c.stats->balances;
  c.balance_changed = true;
  if (type != "fund" || change.empty()) return Item::Next;
  // A funding payment: `text` names the contract ("BTC_USDT:..." in the account book's shape).
  std::string_view contract = text;
  if (const std::size_t colon = contract.find(':'); colon != std::string_view::npos)
    contract = contract.substr(0, colon);
  const InstrumentId inst = c.symbols->find(c.venue, contract);
  if (!inst.valid()) return Item::Next;
  const auto amount = parse_notional(change);
  if (!amount) return Item::Next;
  if (!c.room(sizeof(FundingMsg))) return Item::Stop;
  auto* m = c.place<FundingMsg>();
  init_header(*m, EventType::Funding, inst, c.venue);
  m->amount = *amount;  // positive received
  // No id from the venue: the settlement time and the contract identify the payment.
  char idb[64];
  const std::size_t n = format_int64(time_ms != 0 ? time_ms : c.time_ms, idb);
  std::memcpy(idb + n, ":", 1);
  const std::size_t cn =
      contract.size() < sizeof idb - n - 1 ? contract.size() : sizeof idb - n - 1;
  std::memcpy(idb + n + 1, contract.data(), cn);
  m->funding_id.assign(std::string_view(idb, n + 1 + cn));
  m->asset.assign(currency.empty() ? c.instruments->get(inst).settlement_ccy() : currency);
  stamp(*m, c.recv_ts, c.t0, time_ms != 0 ? time_ms : c.time_ms);
  c.written += sizeof(FundingMsg);
  ++c.count;
  ++c.stats->funding;
  return Item::Next;
}

}  // namespace

PrivateDecodeResult GatePrivateParser::decode(std::string_view json,
                                              Timestamp recv_ts,
                                              Cycles t0,
                                              std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);

  std::int64_t time_ms = 0;
  std::string_view channel;
  std::string_view event;
  if (root["time_ms"].get_int64().get(time_ms) != sj::SUCCESS) time_ms = 0;
  root.reset();
  if (root["channel"].get_string().get(channel) != sj::SUCCESS) {
    root.reset();
    od::value req;
    if (root["request_id"].get(req) == sj::SUCCESS) {  // a WebSocket API reply: the decoder's
      ++stats_.control;
      r.control = ControlOp::Api;
      r.status = ParseStatus::Ignored;
      return r;
    }
    return malformed(stats_, r);
  }
  root.reset();
  if (root["event"].get_string().get(event) != sj::SUCCESS) event = {};
  if (event != "update" && event != "all") return decode_control(stats_, root, channel, event, r);

  const bool is_order = channel == "futures.orders";
  const bool is_trade = channel == "futures.usertrades";
  const bool is_position = channel == "futures.positions";
  const bool is_balance = channel == "futures.balances";
  if (!is_order && !is_trade && !is_position && !is_balance) {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  root.reset();
  od::array data;
  if (root["result"].get_array().get(data) != sj::SUCCESS) return malformed(stats_, r);

  ItemCtx c{&stats_, &symbols_, &instruments_, venue_, recv_ts, t0, time_ms, out};
  for (auto item : data) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return malformed(stats_, r);
    const Item res = is_order      ? decode_order(c, o)
                     : is_trade    ? decode_trade(c, o)
                     : is_position ? decode_position(c, o)
                                   : decode_balance(c, o);
    if (res == Item::Malformed) return malformed(stats_, r);
    if (res == Item::Stop) break;
  }
  r.balance_changed = c.balance_changed;
  if (c.count == 0) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.order_kind = is_position ? OrderEventKind::Position
                 : is_trade  ? OrderEventKind::Fill
                             : OrderEventKind::Ack;
  r.len = c.written;
  r.count = c.count;
  return r;
}

}  // namespace fastmm::venues::gate
