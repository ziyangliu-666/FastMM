#include "fastmm/venues/coinbase/coinbase_private_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct CoinbasePrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

CoinbasePrivateParser::CoinbasePrivateParser(const SymbolTable& symbols,
                                             const InstrumentTable& instruments,
                                             VenueId venue,
                                             std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
CoinbasePrivateParser::~CoinbasePrivateParser() = default;

bool CoinbasePrivateParser::learn(std::string_view order_id,
                                  ClientOrderId cl,
                                  InstrumentId instrument,
                                  Side side,
                                  Qty size,
                                  Qty filled) noexcept {
  const auto key = order_key(order_id);
  if (!key) return false;
  if (TrackedOrder* o = orders_.find(*key)) {
    o->cl = cl;
    o->instrument = instrument;
    o->side = side;
    if (size.is_positive()) o->size = size;
    if (filled > o->filled) o->filled = filled;
    return true;
  }
  if (orders_.insert(*key, TrackedOrder{cl, instrument, side, size, filled}).first == nullptr) {
    ++stats_.table_full;
    return false;
  }
  return true;
}

const TrackedOrder* CoinbasePrivateParser::find(std::string_view order_id) const noexcept {
  const auto key = order_key(order_id);
  return key ? orders_.find(*key) : nullptr;
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

// The fields of one user-channel message, read in one pass.
struct Fields {
  std::string_view type;
  std::string_view time;
  std::string_view product_id;
  std::string_view order_id;
  std::string_view client_oid;
  std::string_view size;
  std::string_view price;
  std::string_view side;
  std::string_view reason;
  std::string_view cancel_reason;
  std::string_view maker_order_id;
  std::string_view taker_order_id;
  std::string_view maker_fee_rate;
  std::string_view taker_fee_rate;
  std::string_view new_size;
  std::string_view message;
  std::uint64_t trade_id = 0;
  std::int64_t cancel_code = -1;
};

[[gnu::noinline]] bool read_fields(od::object& o, Fields& f) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    std::string_view* dst = nullptr;
    switch (key.size()) {
      case 4:
        if (key == "type") dst = &f.type;
        if (key == "time") dst = &f.time;
        if (key == "size") dst = &f.size;
        if (key == "side") dst = &f.side;
        break;
      case 5:
        if (key == "price") dst = &f.price;
        break;
      case 6:
        if (key == "reason") dst = &f.reason;
        break;
      case 7:
        if (key == "message") dst = &f.message;
        break;
      case 8:
        if (key == "order_id") dst = &f.order_id;
        if (key == "new_size") dst = &f.new_size;
        if (key == "trade_id") {
          if (field.value().get_uint64().get(f.trade_id) != sj::SUCCESS) return false;
          continue;
        }
        break;
      case 10:
        if (key == "product_id") dst = &f.product_id;
        if (key == "client_oid") dst = &f.client_oid;
        break;
      case 13:
        if (key == "cancel_reason") {
          // Documented as a code and its text (101: Time In Force); a number or a string.
          od::value v = field.value();
          std::int64_t n = 0;
          if (v.get_int64().get(n) == sj::SUCCESS) {
            f.cancel_code = n;
          } else if (v.get_string().get(f.cancel_reason) == sj::SUCCESS) {
            std::int64_t c = 0;
            std::size_t i = 0;
            for (; i < f.cancel_reason.size() && f.cancel_reason[i] >= '0' &&
                   f.cancel_reason[i] <= '9';
                 ++i)
              c = c * 10 + (f.cancel_reason[i] - '0');
            if (i > 0) f.cancel_code = c;
          }
          continue;
        }
        break;
      case 14:
        if (key == "maker_order_id") dst = &f.maker_order_id;
        if (key == "taker_order_id") dst = &f.taker_order_id;
        if (key == "maker_fee_rate") dst = &f.maker_fee_rate;
        if (key == "taker_fee_rate") dst = &f.taker_fee_rate;
        break;
      default:
        break;
    }
    if (dst == nullptr) continue;
    // `size`, `price` and the like are strings; a null (a market order's price) reads as empty.
    if (field.value().get_string().get(*dst) != sj::SUCCESS) *dst = {};
  }
  return true;
}

template <class M>
M* place(std::span<std::byte> out, std::uint32_t written) noexcept {
  std::byte* p = out.data() + written;
  std::memset(p, 0, sizeof(M));
  return reinterpret_cast<M*>(p);
}

// price x size x rate, truncated to the fixed-point unit.
[[nodiscard]] Notional fee_of(Price px, Qty qty, Notional rate) noexcept {
  return Notional::from_raw(mul_raw(mul(px, qty), rate));
}

}  // namespace

PrivateDecodeResult CoinbasePrivateParser::decode(std::string_view json,
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
  Fields f;
  if (!read_fields(root, f)) return malformed(stats_, r);

  if (f.type == "subscriptions" || f.type == "heartbeat" || f.type == "error") {
    ++stats_.control;
    r.control = f.type == "error"       ? PrivateControl::Error
                : f.type == "heartbeat" ? PrivateControl::Heartbeat
                                        : PrivateControl::Subscriptions;
    r.msg = f.message;
    r.reason = f.reason;
    r.status = f.type == "error" ? ParseStatus::Error : ParseStatus::Ignored;
    return r;
  }
  const std::int64_t exch_ns = f.time.empty() ? 0 : parse_time_ns(f.time);
  const Timestamp exch_ts{exch_ns > 0 ? exch_ns : 0};
  std::uint32_t written = 0;
  auto stamp = [&](EventHeader& h) {
    h.recv_ts = recv_ts;
    h.t0_cycles = t0;
    h.exch_ts = exch_ts;
  };

  if (f.type == "received") {
    ++stats_.received;
    const auto cl = decode_client_oid(f.client_oid);
    if (!cl) {
      ++stats_.foreign;
      r.status = ParseStatus::Ignored;
      return r;
    }
    const InstrumentId inst = symbols_.find(venue_, f.product_id);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      r.status = ParseStatus::UnknownSymbol;
      return r;
    }
    const auto size = parse_qty(f.size);
    if (f.order_id.empty() || (f.side != "buy" && f.side != "sell")) return malformed(stats_, r);
    static_cast<void>(learn(
        f.order_id, *cl, inst, f.side == "sell" ? Side::Sell : Side::Buy, size ? *size : Qty{}));
    auto* m = place<OrderAckMsg>(out, written);
    init_header(*m, EventType::OrderAck, inst, venue_);
    m->cl_ord_id = *cl;
    m->venue_order_id.assign(f.order_id);
    stamp(m->hdr);
    written += sizeof(OrderAckMsg);
    r.count = 1;
    r.order_kind = OrderEventKind::Ack;
  } else if (f.type == "match") {
    const auto px = parse_price(f.price);
    const auto qty = parse_qty(f.size);
    if (!px || !qty || f.trade_id == 0 || (f.side != "buy" && f.side != "sell"))
      return malformed(stats_, r);
    const Side maker_side = f.side == "sell" ? Side::Sell : Side::Buy;
    // Both legs can be this profile's (a self-trade the venue let through).
    for (int leg = 0; leg < 2; ++leg) {
      const bool maker = leg == 0;
      const auto key = order_key(maker ? f.maker_order_id : f.taker_order_id);
      TrackedOrder* o = key ? orders_.find(*key) : nullptr;
      if (o == nullptr) continue;
      o->filled = o->filled + *qty;
      Notional rate{};
      // A rate may carry more than 8 decimals: rounded.
      if (const auto rt = parse_avg_price(maker ? f.maker_fee_rate : f.taker_fee_rate))
        rate = Notional::from_raw(rt->raw);
      auto* m = place<OrderFillMsg>(out, written);
      init_header(*m, EventType::OrderFill, o->instrument, venue_);
      m->cl_ord_id = o->cl;
      m->venue_order_id.assign(maker ? f.maker_order_id : f.taker_order_id);
      char tid[24];
      m->exec_id.assign(
          std::string_view(tid, format_int64(static_cast<std::int64_t>(f.trade_id), tid)));
      m->price = *px;
      m->qty = *qty;
      m->cum_qty = o->filled;
      m->leaves_qty = o->size > o->filled ? o->size - o->filled : Qty{};
      m->fee = fee_of(*px, *qty, rate);
      m->fee_asset = FeeAsset::Quote;  // spot fees are charged in the quote currency
      m->side = maker ? maker_side : (maker_side == Side::Buy ? Side::Sell : Side::Buy);
      m->liquidity = maker ? Liquidity::Maker : Liquidity::Taker;
      stamp(m->hdr);
      written += sizeof(OrderFillMsg);
      ++r.count;
      ++stats_.fills;
    }
    if (r.count == 0) {
      ++stats_.foreign;
      r.status = ParseStatus::Ignored;
      return r;
    }
    r.order_kind = OrderEventKind::Fill;
  } else if (f.type == "done") {
    ++stats_.done;
    const auto key = order_key(f.order_id);
    TrackedOrder* o = key ? orders_.find(*key) : nullptr;
    if (o == nullptr) {
      ++stats_.foreign;
      r.status = ParseStatus::Ignored;
      return r;
    }
    const TrackedOrder t = *o;
    orders_.erase(*key);
    if (f.reason != "canceled") {  // filled: the matches reported it
      r.status = ParseStatus::Ignored;
      return r;
    }
    if (f.cancel_code == 101) {
      auto* m = place<OrderExpiredMsg>(out, written);
      init_header(*m, EventType::OrderExpired, t.instrument, venue_);
      m->cl_ord_id = t.cl;
      m->venue_order_id.assign(f.order_id);
      m->cum_qty = t.filled;
      stamp(m->hdr);
      written += sizeof(OrderExpiredMsg);
      r.order_kind = OrderEventKind::Expired;
    } else {
      auto* m = place<OrderCancelAckMsg>(out, written);
      init_header(*m, EventType::OrderCancelAck, t.instrument, venue_);
      m->cl_ord_id = t.cl;
      m->venue_order_id.assign(f.order_id);
      m->cum_qty = t.filled;
      stamp(m->hdr);
      written += sizeof(OrderCancelAckMsg);
      r.order_kind = OrderEventKind::CancelAck;
    }
    r.count = 1;
  } else if (f.type == "change") {
    ++stats_.changes;
    const auto key = order_key(f.order_id);
    TrackedOrder* o = key ? orders_.find(*key) : nullptr;
    if (o != nullptr) {
      if (const auto ns = parse_qty(f.new_size)) o->size = o->filled + *ns;
    } else {
      ++stats_.foreign;
    }
    r.status = ParseStatus::Ignored;
    return r;
  } else {
    // open, activate and anything newer.
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.len = written;
  return r;
}

}  // namespace fastmm::venues::coinbase
