#include "fastmm/backtest/order_events.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fastmm::bt {

namespace {

// A fixed-point raw value as a decimal with 8 places.
void put_fixed(std::string& s, std::int64_t raw) {
  char buf[40];
  const bool neg = raw < 0;
  const std::uint64_t u =
      neg ? 0 - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
  std::snprintf(
      buf, sizeof buf, "%s%" PRIu64 ".%08" PRIu64, neg ? "-" : "", u / 100000000U, u % 100000000U);
  s += buf;
}

void put_int(std::string& s, std::int64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%" PRId64, v);
  s += buf;
}

void put_uint(std::string& s, std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%" PRIu64, v);
  s += buf;
}

// Text for a CSV field: commas and quotes become spaces, so no field needs quoting.
void put_text(std::string& s, std::string_view t) {
  for (const char c : t) s += (c == ',' || c == '"' || c == '\n' || c == '\r') ? ' ' : c;
}

const char* side_name(Side side) {
  return side == Side::Buy ? "B" : "S";
}

}  // namespace

std::uint64_t write_order_events(const std::vector<std::string>& journals,
                                 std::FILE* out,
                                 const OrderEventsOptions& opt) {
  // type: the event; time: recv_ts (the engine's send time for an outbound message); venue_time:
  // the venue's time on the record; venue: the venue id (a pool member's own); id: the client
  // order id; a..e: the type's fields (see the switch below).
  if (opt.header)
    std::fputs(
        "type,time,venue_time,venue,symbol,id,side,price,qty,a,b,c,d,e,flags,trigger,engine\n",
        out);
  std::uint64_t rows = 0;
  std::string line;
  line.reserve(512);
  for (const std::string& path : journals) {
    JournalReader reader;
    if (auto r = reader.open(path); !r)
      throw std::runtime_error("cannot open " + path + ": " + reader.describe(r.error()));
    const std::span<const Instrument> table = reader.instruments();
    const std::span<const JournalParam> params = reader.params();
    // The TSC of a record's T0 (an outbound message: the receipt of the event that triggered it)
    // in wall-clock ns, from the header's calibration; a journal without one (a backtest's) keeps
    // the simulated time it stored there.
    const JournalFileHeader& hdr = reader.header();
    TscCalibration tsc{};
    tsc.tsc0 = hdr.tsc0;
    tsc.ns0 = hdr.tsc_ns0;
    tsc.ns_per_cycle_q32 = hdr.tsc_ns_per_cycle_q32;
    auto trigger_ns = [&](const EventHeader* h) -> std::int64_t {
      const std::uint64_t c = h->t0_cycles.v;
      if (!tsc.has_rate() || c == 0) return static_cast<std::int64_t>(c);
      return tsc_to_ns(tsc, c);
    };
    const bool all_symbols =
        std::find(opt.symbols.begin(), opt.symbols.end(), "*") != opt.symbols.end();
    auto symbol = [&](InstrumentId id) -> std::string_view {
      return id.valid() && id.value < table.size() ? table[id.value].symbol.view()
                                                   : std::string_view{};
    };
    // The engine clock, followed through the records (EngineTime values, kEngineTime deltas): the
    // time the engine consumed an inbound event, 0 for a record it did not consume. A later part of
    // a session starts from the clock its header carries.
    std::int64_t engine_ns = journal_engine_clock(hdr).ns;
    reader.for_each([&](const EventHeader* h) {
      if (h->type == EventType::EngineTime) {
        engine_ns = msg_cast<EngineTimeMsg>(h).engine_ts.ns;
        return;
      }
      std::int64_t consumed = 0;
      if ((h->flags & EventHeader::kEngineTime) != 0) {
        engine_ns += static_cast<std::int32_t>(h->reserved0);
        consumed = engine_ns;
      }
      const char* type = nullptr;
      const bool md_type =
          h->type == EventType::Trade ||
          (opt.md_books && (h->type == EventType::BookTicker || h->type == EventType::BookDelta ||
                            h->type == EventType::BookSnapshot));
      if (md_type && !opt.symbols.empty() &&
          (all_symbols ||
           std::find(opt.symbols.begin(), opt.symbols.end(), symbol(h->instrument)) !=
               opt.symbols.end())) {
        const std::int64_t tm = h->recv_ts.ns;
        if ((opt.from_ns != 0 && tm < opt.from_ns) || (opt.to_ns != 0 && tm >= opt.to_ns)) return;
        auto head = [&](const char* t) {
          line.clear();
          line += t;
          line += ',';
          put_int(line, tm);
          line += ',';
          put_int(line, h->exch_ts.ns);
          line += ',';
          put_uint(line, h->venue.value);
          line += ',';
          put_text(line, symbol(h->instrument));
          line += ',';
        };
        auto tail = [&]() {
          line += ',';
          put_int(line, h->flags);
          line += ',';
          put_int(line, trigger_ns(h));
          line += ',';
          put_int(line, consumed);
          line += '\n';
          std::fputs(line.c_str(), out);
          ++rows;
        };
        if (h->type == EventType::Trade) {
          const auto& m = msg_cast<TradeMsg>(h);
          head("trade");
          line += ',';
          line += side_name(m.aggressor);  // the aggressor
          line += ',';
          put_fixed(line, m.price.raw);
          line += ',';
          put_fixed(line, m.qty.raw);
          line += ',';
          put_uint(line, m.trade_id);  // a
          line += ",,,,";
          tail();
        } else if (h->type == EventType::BookTicker) {
          const auto& m = msg_cast<BookTickerMsg>(h);
          head("ticker");
          line += ",,";
          put_fixed(line, m.bid_px.raw);  // price, qty: the bid
          line += ',';
          put_fixed(line, m.bid_qty.raw);
          line += ',';
          put_fixed(line, m.ask_px.raw);  // a, b: the ask
          line += ',';
          put_fixed(line, m.ask_qty.raw);
          line += ",,,";
          tail();
        } else if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot) {
          const auto& m = msg_cast<BookDeltaMsg>(h);
          const char* t = m.is_snapshot() ? "snapshot" : "depth";
          for (std::uint32_t i = 0; i < m.bid_count + m.ask_count; ++i) {
            const Level& l = m.levels()[i];
            head(t);
            line += ',';
            line += i < m.bid_count ? "B" : "S";
            line += ',';
            put_fixed(line, l.price.raw);
            line += ',';
            put_fixed(line, l.qty.raw);
            line += ',';
            put_uint(line, m.last_update_id);  // a
            line += ",,,,";
            tail();
          }
        }
        return;
      }
      switch (h->type) {
        case EventType::OutNewOrder:
          type = "new";
          break;
        case EventType::OutCancel:
          type = "cancel";
          break;
        case EventType::OutReplace:
          type = "replace";
          break;
        case EventType::OrderAck:
          type = "ack";
          break;
        case EventType::OrderReject:
          type = "reject";
          break;
        case EventType::OrderCancelAck:
          type = "cancel_ack";
          break;
        case EventType::OrderCancelReject:
          type = "cancel_reject";
          break;
        case EventType::OrderFill:
          type = "fill";
          break;
        case EventType::OrderExpired:
          type = "expired";
          break;
        case EventType::Balance:
          type = "balance";
          break;
        case EventType::AccountFlow:
          type = "flow";
          break;
        case EventType::Reconcile:
          type = "reconcile";
          break;
        case EventType::ParamUpdate:
          type = "param";
          break;
        default:
          return;
      }
      const std::int64_t t = h->recv_ts.ns;
      if (opt.from_ns != 0 && t < opt.from_ns) return;
      if (opt.to_ns != 0 && t >= opt.to_ns) return;
      line.clear();
      line += type;
      line += ',';
      put_int(line, t);
      line += ',';
      put_int(line, h->exch_ts.ns);
      line += ',';
      put_uint(line, h->venue.value);
      line += ',';
      put_text(line, symbol(h->instrument));
      line += ',';
      // id,side,price,qty,a,b,c,d,e
      switch (h->type) {
        case EventType::OutNewOrder: {
          const auto& m = msg_cast<OutNewOrderMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ',';
          line += side_name(m.side);
          line += ',';
          put_fixed(line, m.price.raw);
          line += ',';
          put_fixed(line, m.qty.raw);
          line += ',';
          put_int(line, static_cast<int>(m.type));  // a: order type
          line += ',';
          put_int(line, static_cast<int>(m.tif));  // b: time in force
          line += ',';
          put_int(line, m.reduce_only);  // c
          line += ",,";
          break;
        }
        case EventType::OutCancel:
          put_uint(line, msg_cast<OutCancelMsg>(h).cl_ord_id.value);
          line += ",,,,,,,,";
          break;
        case EventType::OutReplace: {
          const auto& m = msg_cast<OutReplaceMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ",,";
          put_fixed(line, m.price.raw);
          line += ',';
          put_fixed(line, m.qty.raw);
          line += ',';
          put_uint(line, m.orig_cl_ord_id.value);  // a: the order replaced
          line += ",,,,";
          break;
        }
        case EventType::OrderAck:
          put_uint(line, msg_cast<OrderAckMsg>(h).cl_ord_id.value);
          line += ",,,,";
          put_int(line, msg_cast<OrderAckMsg>(h).flags);  // a
          line += ",,,,";
          break;
        case EventType::OrderReject: {
          const auto& m = msg_cast<OrderRejectMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ",,,,";
          put_text(line, to_string(m.reason));  // a: reason
          line += ',';
          put_int(line, m.venue_code);  // b: venue code
          line += ',';
          put_text(line, m.text.view());  // c: venue text
          line += ",,";
          break;
        }
        case EventType::OrderCancelAck: {
          const auto& m = msg_cast<OrderCancelAckMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ",,,";
          put_fixed(line, m.cum_qty.raw);  // qty: filled before the cancel
          line += ",,,,,";
          break;
        }
        case EventType::OrderCancelReject: {
          const auto& m = msg_cast<OrderCancelRejectMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ",,,,";
          put_text(line, to_string(m.reason));
          line += ',';
          put_int(line, m.venue_code);
          line += ',';
          put_text(line, m.text.view());
          line += ",,";
          break;
        }
        case EventType::OrderFill: {
          const auto& m = msg_cast<OrderFillMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ',';
          line += side_name(m.side);
          line += ',';
          put_fixed(line, m.price.raw);
          line += ',';
          put_fixed(line, m.qty.raw);
          line += ',';
          put_int(line, static_cast<int>(m.liquidity));  // a: 1 maker, 2 taker
          line += ',';
          put_fixed(line, m.fee.raw);  // b: fee
          line += ',';
          put_int(line, static_cast<int>(m.fee_asset));  // c
          line += ',';
          put_text(line, m.exec_id.view());  // d: exec id
          line += ',';
          put_fixed(line, m.leaves_qty.raw);  // e: leaves
          line += ',';
          put_int(line, h->flags | (m.flags << 8));
          line += ',';
          put_int(line, trigger_ns(h));
          line += ',';
          put_int(line, consumed);
          line += '\n';
          std::fputs(line.c_str(), out);
          ++rows;
          return;
        }
        case EventType::OrderExpired: {
          const auto& m = msg_cast<OrderExpiredMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ",,,";
          put_fixed(line, m.cum_qty.raw);
          line += ",,,,,";
          break;
        }
        case EventType::AccountFlow: {
          const auto& m = msg_cast<AccountFlowMsg>(h);
          line += ",,,,";
          put_text(line, m.asset.view());  // a: asset
          line += ',';
          put_fixed(line, m.amount.raw);  // b: signed, positive in
          line += ',';
          put_text(line, m.flow_id.view());  // c: the venue's id
          line += ",,,";
          put_int(line, h->flags);
          line += ',';
          put_int(line, trigger_ns(h));
          line += ',';
          put_int(line, consumed);
          line += '\n';
          std::fputs(line.c_str(), out);
          ++rows;
          return;
        }
        case EventType::Balance: {
          const auto& m = msg_cast<BalanceMsg>(h);
          line += ",,,,";
          put_text(line, m.asset.view());  // a: asset
          line += ',';
          put_fixed(line, m.free.raw);  // b
          line += ',';
          put_fixed(line, m.locked.raw);  // c
          line += ',';
          put_fixed(line, m.total.raw);  // d
          line += ',';
          put_fixed(line, m.equity.raw);  // e
          line += ',';
          put_int(line, h->flags | (m.flags << 8));
          line += ',';
          put_int(line, trigger_ns(h));
          line += ',';
          put_int(line, consumed);
          line += '\n';
          std::fputs(line.c_str(), out);
          ++rows;
          return;
        }
        case EventType::Reconcile: {
          const auto& m = msg_cast<ReconcileMsg>(h);
          put_uint(line, m.cl_ord_id.value);
          line += ',';
          line += m.kind == ReconcileMsg::Kind::OpenOrder ? side_name(m.side) : "";
          line += ',';
          put_fixed(line, m.kind == ReconcileMsg::Kind::Position ? m.avg_px.raw : m.price.raw);
          line += ',';
          put_fixed(line,
                    m.kind == ReconcileMsg::Kind::Position ? m.position_qty.raw : m.orig_qty.raw);
          line += ',';
          put_int(line, static_cast<int>(m.kind));  // a: 0 begin, 1 open order, 2 position, 3 end
          line += ',';
          put_fixed(line, m.cum_qty.raw);  // b
          line += ",,,";
          break;
        }
        case EventType::ParamUpdate: {
          const auto& m = msg_cast<ParamUpdateMsg>(h);
          line += ",,,,";
          // a: "name=raw;name=raw", b: origin, c: source
          for (std::uint32_t i = 0; i < m.count && i < ParamUpdateMsg::kMaxFields; ++i) {
            if (i > 0) line += ';';
            const std::uint16_t f = m.field[i];
            if (f < params.size())
              put_text(line, params[f].name);
            else
              put_uint(line, f);
            line += '=';
            const std::uint8_t pt = f < params.size() ? params[f].type : 0;
            if (pt == 1) {  // double: IEEE bits
              double d = 0;
              std::memcpy(&d, &m.value[i], sizeof d);
              char buf[40];
              std::snprintf(buf, sizeof buf, "%.10g", d);
              line += buf;
            } else {
              put_int(line, m.value[i]);
            }
          }
          line += ',';
          put_int(line, static_cast<int>(m.origin));
          line += ',';
          put_text(line, m.source_view());
          line += ",,";
          break;
        }
        default:
          return;
      }
      line += ',';
      put_int(line, h->flags);
      line += ',';
      put_int(line, trigger_ns(h));
      line += ',';
      put_int(line, consumed);
      line += '\n';
      std::fputs(line.c_str(), out);
      ++rows;
    });
  }
  return rows;
}

}  // namespace fastmm::bt
