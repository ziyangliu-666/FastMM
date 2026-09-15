#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::binance_usdm {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct BinanceUsdmUserParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BinanceUsdmUserParser::BinanceUsdmUserParser(const SymbolTable& symbols,
                                             const InstrumentTable& instruments,
                                             VenueId venue,
                                             std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
BinanceUsdmUserParser::~BinanceUsdmUserParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// The ORDER_TRADE_UPDATE "o" fields FastMM reads.
struct OrderUpdate {
  std::string_view symbol;     // s
  std::string_view client_id;  // c
  std::string_view side;       // S
  std::string_view order_qty;  // q
  std::string_view exec_type;  // x
  std::int64_t order_id = 0;   // i
  std::string_view last_qty;   // l
  std::string_view cum_qty;    // z
  std::string_view last_px;    // L
  std::string_view fee_asset;  // N (absent without commission)
  std::string_view fee;        // n
  std::int64_t tx_time = 0;    // T
  std::int64_t trade_id = 0;   // t
  bool maker = false;          // m
};

[[gnu::noinline]] bool read_order_update(od::object& o, OrderUpdate& u) noexcept {
  if (o["s"].get_string().get(u.symbol) != sj::SUCCESS) return false;
  if (o["c"].get_string().get(u.client_id) != sj::SUCCESS) return false;
  if (o["S"].get_string().get(u.side) != sj::SUCCESS) return false;
  if (o["q"].get_string().get(u.order_qty) != sj::SUCCESS) return false;
  if (o["x"].get_string().get(u.exec_type) != sj::SUCCESS) return false;
  if (o["i"].get_int64().get(u.order_id) != sj::SUCCESS) return false;
  if (o["l"].get_string().get(u.last_qty) != sj::SUCCESS) return false;
  if (o["z"].get_string().get(u.cum_qty) != sj::SUCCESS) return false;
  if (o["L"].get_string().get(u.last_px) != sj::SUCCESS) return false;
  if (o["N"].get_string().get(u.fee_asset) != sj::SUCCESS) u.fee_asset = {};
  if (o["n"].get_string().get(u.fee) != sj::SUCCESS) u.fee = {};
  if (o["T"].get_int64().get(u.tx_time) != sj::SUCCESS) return false;
  if (o["t"].get_int64().get(u.trade_id) != sj::SUCCESS) u.trade_id = 0;
  if (o["m"].get_bool().get(u.maker) != sj::SUCCESS) u.maker = false;
  return true;
}

void set_venue_order_id(VenueOrderId& out, std::int64_t order_id) noexcept {
  char buf[24];
  out.assign(std::string_view(buf, format_int64(order_id, buf)));
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
  m.hdr.exch_ts = ts_from_ms(exch_ms);
}

enum class ExecKind : std::uint8_t { New, Canceled, Expired, Trade, Other };
ExecKind exec_kind(std::string_view x) noexcept {
  if (x == "NEW") return ExecKind::New;
  if (x == "CANCELED") return ExecKind::Canceled;
  if (x == "EXPIRED") return ExecKind::Expired;
  if (x == "TRADE" || x == "CALCULATED") return ExecKind::Trade;  // CALCULATED: liquidation
  return ExecKind::Other;                                         // AMENDMENT
}

}  // namespace

UserDecodeResult BinanceUsdmUserParser::decode(std::string_view json,
                                               Timestamp recv_ts,
                                               Cycles t0,
                                               std::span<std::byte> out) noexcept {
  ++stats_.frames;
  UserDecodeResult r;
  auto ignored = [&]() {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    r.count = 0;
    r.len = 0;
    return r;
  };
  auto malformed = [&]() {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    r.len = 0;
    return r;
  };
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed();
  od::object ev;
  {
    od::value v;
    if (root["data"].get(v) == sj::SUCCESS) {  // combined-stream wrapper
      if (v.get_object().get(ev) != sj::SUCCESS) return malformed();
    } else {
      root.reset();
      ev = root;
    }
  }
  std::string_view type;
  {
    od::value e;
    if (ev["e"].get(e) != sj::SUCCESS || e.get_string().get(type) != sj::SUCCESS) return ignored();
  }

  if (type == "ORDER_TRADE_UPDATE") {
    od::object o;
    if (ev["o"].get_object().get(o) != sj::SUCCESS) return malformed();
    OrderUpdate x;
    if (!read_order_update(o, x)) return malformed();
    ++stats_.order_updates;
    const ExecKind kind = exec_kind(x.exec_type);
    if (kind == ExecKind::Other) return ignored();
    const InstrumentId inst = symbols_.find(venue_, x.symbol);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      r.status = ParseStatus::UnknownSymbol;
      return r;
    }
    ClientOrderId cl_ord_id{};
    if (const auto decoded = decode_cl_ord_id(x.client_id)) {
      cl_ord_id = *decoded;
    } else {
      ++stats_.foreign_ids;
      if (kind != ExecKind::Trade) return ignored();
    }
    const auto cum = parse_qty(x.cum_qty);
    if (!cum) return malformed();
    r.status = ParseStatus::Ok;
    r.count = 1;
    switch (kind) {
      case ExecKind::New: {
        auto* m = reinterpret_cast<OrderAckMsg*>(out.data());
        init_header(*m, EventType::OrderAck, inst, venue_);
        m->cl_ord_id = cl_ord_id;
        set_venue_order_id(m->venue_order_id, x.order_id);
        stamp(*m, recv_ts, t0, x.tx_time);
        r.order_kind = OrderEventKind::Ack;
        r.len = sizeof(OrderAckMsg);
        return r;
      }
      case ExecKind::Canceled: {
        auto* m = reinterpret_cast<OrderCancelAckMsg*>(out.data());
        init_header(*m, EventType::OrderCancelAck, inst, venue_);
        m->cl_ord_id = cl_ord_id;
        set_venue_order_id(m->venue_order_id, x.order_id);
        m->cum_qty = *cum;
        stamp(*m, recv_ts, t0, x.tx_time);
        r.order_kind = OrderEventKind::CancelAck;
        r.len = sizeof(OrderCancelAckMsg);
        return r;
      }
      case ExecKind::Expired: {
        auto* m = reinterpret_cast<OrderExpiredMsg*>(out.data());
        init_header(*m, EventType::OrderExpired, inst, venue_);
        m->cl_ord_id = cl_ord_id;
        set_venue_order_id(m->venue_order_id, x.order_id);
        m->cum_qty = *cum;
        stamp(*m, recv_ts, t0, x.tx_time);
        r.order_kind = OrderEventKind::Expired;
        r.len = sizeof(OrderExpiredMsg);
        return r;
      }
      case ExecKind::Trade: {
        const auto last_qty = parse_qty(x.last_qty);
        const auto last_px = parse_price(x.last_px);
        const auto order_qty = parse_qty(x.order_qty);
        if (!last_qty || !last_px || !order_qty) return malformed();
        auto* m = reinterpret_cast<OrderFillMsg*>(out.data());
        init_header(*m, EventType::OrderFill, inst, venue_);
        m->cl_ord_id = cl_ord_id;
        set_venue_order_id(m->venue_order_id, x.order_id);
        {
          char buf[24];
          m->exec_id.assign(std::string_view(buf, format_int64(x.trade_id, buf)));
        }
        m->price = *last_px;
        m->qty = *last_qty;
        m->cum_qty = *cum;
        m->leaves_qty = *order_qty - *cum;
        m->fee = Notional{};
        if (const auto fee = parse_notional(x.fee)) m->fee = *fee;
        // USDⓈ-M commission is charged in the margin asset (USDT), or in BNB with the BNB fee
        // discount on; the engine cannot value BNB.
        const Instrument& in = instruments_.get(inst);
        if (x.fee_asset.empty() || m->fee.is_zero() ||
            iequals_symbol(in.quote.view(), x.fee_asset)) {
          m->fee_asset = FeeAsset::Quote;
        } else if (iequals_symbol(in.base.view(), x.fee_asset)) {
          m->fee_asset = FeeAsset::Base;
        } else {
          m->fee_asset = FeeAsset::Other;
        }
        m->side = x.side == "SELL" ? Side::Sell : Side::Buy;
        m->liquidity = x.maker ? Liquidity::Maker : Liquidity::Taker;
        stamp(*m, recv_ts, t0, x.tx_time);
        r.order_kind = OrderEventKind::Fill;
        r.len = sizeof(OrderFillMsg);
        return r;
      }
      case ExecKind::Other:
        break;
    }
    return ignored();
  }

  if (type == "ACCOUNT_UPDATE") {
    ++stats_.account_updates;
    std::int64_t tx_time = 0;
    if (ev["T"].get_int64().get(tx_time) != sj::SUCCESS) return malformed();
    od::object a;
    if (ev["a"].get_object().get(a) != sj::SUCCESS) return malformed();
    od::array positions;
    if (a["P"].get_array().get(positions) != sj::SUCCESS) return ignored();  // balance only
    std::uint32_t written = 0;
    std::uint32_t count = 0;
    for (auto pos_res : positions) {
      od::object p;
      if (pos_res.get_object().get(p) != sj::SUCCESS) return malformed();
      std::string_view sym;
      std::string_view amount;
      std::string_view entry;
      std::string_view side;
      if (p["s"].get_string().get(sym) != sj::SUCCESS) return malformed();
      if (p["pa"].get_string().get(amount) != sj::SUCCESS) return malformed();
      if (p["ep"].get_string().get(entry) != sj::SUCCESS) return malformed();
      if (p["ps"].get_string().get(side) != sj::SUCCESS) return malformed();
      const auto qty = parse_qty(amount);
      const auto avg = parse_price(entry);
      if (!qty || !avg) return malformed();
      if (side != "BOTH") {
        ++stats_.hedge_positions;
        continue;
      }
      const InstrumentId inst = symbols_.find(venue_, sym);
      if (!inst.valid()) continue;  // a position on another symbol of the account
      if (written + sizeof(PositionUpdateMsg) > out.size()) break;
      auto* m = reinterpret_cast<PositionUpdateMsg*>(out.data() + written);
      init_header(*m, EventType::PositionUpdate, inst, venue_);
      m->qty = *qty;
      m->avg_px = *avg;
      stamp(*m, recv_ts, t0, tx_time);
      written += sizeof(PositionUpdateMsg);
      ++count;
    }
    if (count == 0) return ignored();
    stats_.positions += count;
    r.status = ParseStatus::Ok;
    r.order_kind = OrderEventKind::Position;
    r.len = written;
    r.count = count;
    return r;
  }

  if (type == "listenKeyExpired") {
    ++stats_.listen_key_expired;
    UserDecodeResult out_r = ignored();
    out_r.listen_key_expired = true;
    return out_r;
  }
  // MARGIN_CALL, ACCOUNT_CONFIG_UPDATE, TRADE_LITE, STRATEGY_UPDATE, GRID_UPDATE, ALGO_UPDATE,
  // CONDITIONAL_ORDER_TRIGGER_REJECT
  return ignored();
}

}  // namespace fastmm::venues::binance_usdm
