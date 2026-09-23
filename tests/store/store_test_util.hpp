#pragma once
// Shared fixtures for the store tests: an instrument table and records shaped like the ones the
// engine emits.
#include "test_support.hpp"

#include "fastmm/core/instrument.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/store/backend.hpp"

#include <string>

namespace fastmm::test {

inline InstrumentTable store_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.base = "BTC";
  i.quote = "USDT";
  i.asset_class = AssetClass::Spot;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.flags = Instrument::kEnabled;
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  i.base = "ETH";
  REQUIRE(t.add(i));
  return t;
}

inline store::SessionOpen store_session(std::uint64_t id, std::int64_t started_ns) {
  store::SessionOpen s;
  s.session_id = id;
  s.session_epoch = 7;
  s.started_ns = started_ns;
  s.engine_name = "test";
  s.strategy = "basic_mm";
  s.version = "0.1.0";
  s.build_info = "test build";
  s.config_hash = 0xDEADBEEF;
  s.config_toml = "[engine]\nname = \"test\"\n";
  s.journal_path = "runs/test.fmj";
  s.host = "localhost";
  s.pid = 1234;
  s.pnl_carry_raw = -5000;
  return s;
}

// A fill of `qty` at `price` on instrument 0, with the position it leaves behind.
inline FillRecord store_fill(std::uint64_t session,
                             std::uint64_t seq,
                             std::int64_t ts_ns,
                             Side side,
                             std::int64_t price_raw,
                             std::int64_t qty_raw,
                             std::int64_t realized_raw,
                             std::int64_t fees_raw,
                             const std::string& exec_id) {
  FillRecord r{};
  r.hdr.len = sizeof(FillRecord);
  r.hdr.type = RecordType::Fill;
  r.hdr.version = kRecordVersion;
  r.hdr.instrument = InstrumentId{0};
  r.hdr.seq = seq;
  r.hdr.session_id = session;
  r.hdr.engine_ts = Timestamp{ts_ns};
  r.cl_ord_id = make_cl_ord_id(7, static_cast<std::uint32_t>(seq));
  r.price = Price::from_raw(price_raw);
  r.qty = Qty::from_raw(qty_raw);
  r.booked_qty = r.qty;
  r.cum_qty = r.qty;
  r.fee = Notional::from_raw(fees_raw);
  r.fee_amount = r.fee;
  r.side = side;
  r.liquidity = Liquidity::Maker;
  r.fee_asset = FeeAsset::Quote;
  r.position_realized = Notional::from_raw(realized_raw);
  r.position_fees = Notional::from_raw(fees_raw);
  r.venue_order_id = "V1";
  r.exec_id.assign(exec_id);
  return r;
}

// A position snapshot whose cumulative counters the day roll-up differences.
inline PositionRecord store_position(std::uint64_t session,
                                     std::uint64_t seq,
                                     std::int64_t ts_ns,
                                     std::int64_t qty_raw,
                                     std::int64_t realized_raw,
                                     std::int64_t fees_raw,
                                     std::uint32_t fills) {
  PositionRecord r{};
  r.hdr.len = sizeof(PositionRecord);
  r.hdr.type = RecordType::Position;
  r.hdr.version = kRecordVersion;
  r.hdr.instrument = InstrumentId{0};
  r.hdr.seq = seq;
  r.hdr.session_id = session;
  r.hdr.engine_ts = Timestamp{ts_ns};
  r.pos.qty = Qty::from_raw(qty_raw);
  r.pos.avg_px = Price::from_decimal("100").value();
  r.pos.realized = Notional::from_raw(realized_raw);
  r.pos.fees = Notional::from_raw(fees_raw);
  r.pos.gross_traded = Qty::from_raw(qty_raw < 0 ? -qty_raw : qty_raw);
  r.pos.fills = fills;
  r.total_realized = r.pos.realized;
  r.total_fees = r.pos.fees;
  return r;
}

inline OrderRecord store_order(std::uint64_t session,
                               std::uint64_t seq,
                               std::int64_t ts_ns,
                               std::uint32_t id,
                               OrderState state) {
  OrderRecord r{};
  r.hdr.len = sizeof(OrderRecord);
  r.hdr.type = RecordType::Order;
  r.hdr.version = kRecordVersion;
  r.hdr.instrument = InstrumentId{0};
  r.hdr.seq = seq;
  r.hdr.session_id = session;
  r.hdr.engine_ts = Timestamp{ts_ns};
  if (is_terminal(state)) r.hdr.flags |= RecordHeader::kTerminal;
  r.order.cl_ord_id = make_cl_ord_id(7, id);
  r.order.instrument = InstrumentId{0};
  r.order.side = Side::Buy;
  r.order.type = OrderType::Limit;
  r.order.tif = TimeInForce::Gtc;
  r.order.state = state;
  r.order.price = Price::from_decimal("100").value();
  r.order.qty = Qty::from_decimal("1").value();
  r.order.created = Timestamp{ts_ns};
  return r;
}

// The record that closes the client order id a cancel-replace superseded.
inline OrderRecord store_replaced(std::uint64_t session,
                                  std::uint64_t seq,
                                  std::int64_t ts_ns,
                                  std::uint32_t old_id) {
  OrderRecord r = store_order(session, seq, ts_ns, old_id, OrderState::Live);
  r.hdr.flags |= RecordHeader::kTerminal;
  r.hdr.aux[2] = 1;
  return r;
}

inline KillRecord store_kill(std::uint64_t session,
                             std::uint64_t seq,
                             std::int64_t ts_ns,
                             KillReason reason) {
  KillRecord r{};
  r.hdr.len = sizeof(KillRecord);
  r.hdr.type = RecordType::Kill;
  r.hdr.version = kRecordVersion;
  r.hdr.seq = seq;
  r.hdr.session_id = session;
  r.hdr.engine_ts = Timestamp{ts_ns};
  r.reason = reason;
  r.kill_flags = 1;
  r.kills = 1;
  return r;
}

// 2024-03-04T00:00:00Z and 2024-03-05T00:00:00Z.
inline constexpr std::int64_t kDay1Ns = 1'709'510'400'000'000'000LL;
inline constexpr std::int64_t kDay2Ns = kDay1Ns + 86'400'000'000'000LL;

}  // namespace fastmm::test
