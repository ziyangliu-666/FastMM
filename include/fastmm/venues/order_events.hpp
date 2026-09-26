#pragma once
// Helpers that build the normalised order/connection events a connector emits from its
// control logic (WebSocket API responses, REST replies, local rejects). Each constructs the
// message on the stack and copies it into the order EventSink (fixed-size messages only, so
// hdr.len == sizeof(M)).
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <x86intrin.h>

#include <cstdint>
#include <string_view>

namespace fastmm::venues {

inline void emit_order_reject(EventSink& sink,
                              VenueId venue,
                              InstrumentId inst,
                              ClientOrderId id,
                              RejectReason reason,
                              int code,
                              std::string_view text) noexcept {
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject, inst, venue);
  m.cl_ord_id = id;
  m.reason = reason;
  m.venue_code = code;
  m.text.assign(text);  // truncates to capacity
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

inline void emit_cancel_reject(EventSink& sink,
                               VenueId venue,
                               InstrumentId inst,
                               ClientOrderId id,
                               RejectReason reason,
                               int code,
                               std::string_view text) noexcept {
  OrderCancelRejectMsg m{};
  init_header(m, EventType::OrderCancelReject, inst, venue);
  m.cl_ord_id = id;
  m.reason = reason;
  m.venue_code = code;
  m.text.assign(text);  // truncates to capacity
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

// `flags`: OrderAckMsg::kAmendedInPlace when the venue amended the resting order rather than
// creating a new one.
inline void emit_order_ack(EventSink& sink,
                           VenueId venue,
                           InstrumentId inst,
                           ClientOrderId id,
                           std::string_view venue_order_id,
                           std::uint8_t flags = 0) noexcept {
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck, inst, venue);
  m.cl_ord_id = id;
  m.venue_order_id.assign(venue_order_id);
  m.flags = flags;
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

inline void emit_cancel_ack(EventSink& sink,
                            VenueId venue,
                            InstrumentId inst,
                            ClientOrderId id,
                            std::string_view venue_order_id,
                            Qty cum_qty) noexcept {
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck, inst, venue);
  m.cl_ord_id = id;
  m.venue_order_id.assign(venue_order_id);
  m.cum_qty = cum_qty;
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

// An execution from the venue's trade history (Venue::request_executions), not from the private
// stream. It carries the venue's execution id, which is what lets the OMS drop the ones it already
// booked, and no cumulative quantity: the venue's trade history does not report one, so the OMS
// works out how much of the execution is new. `cl_ord_id` may be empty when the connector cannot
// map the venue's order id back to one of its own (a restarted session): the fill still reaches the
// position, as an unknown fill. `trade_time_ms` is the venue's time of the trade (Unix ms), stamped
// as hdr.exch_ts: fastmm-gateway tells from it whether an execution naming no live order is older
// than what a strategy's store holds.
inline void emit_replayed_fill(EventSink& sink,
                               VenueId venue,
                               InstrumentId inst,
                               ClientOrderId id,
                               std::string_view venue_order_id,
                               std::string_view exec_id,
                               Side side,
                               Price price,
                               Qty qty,
                               Notional fee,
                               FeeAsset fee_asset,
                               Liquidity liquidity,
                               std::int64_t trade_time_ms) noexcept {
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, inst, venue);
  m.hdr.exch_ts = Timestamp{trade_time_ms * 1'000'000};  // the venue's time of the trade
  m.cl_ord_id = id;
  m.venue_order_id.assign(venue_order_id);
  m.exec_id.assign(exec_id);
  m.price = price;
  m.qty = qty;
  m.fee = fee;
  m.side = side;
  m.liquidity = liquidity;
  m.fee_asset = fee_asset;
  m.flags = OrderFillMsg::kReplayed;
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

// A funding payment (FundingMsg): `amount` signed in `asset`, negative paid. `replayed` when it
// comes from the venue's history rather than its private stream.
inline void emit_funding(EventSink& sink,
                         VenueId venue,
                         InstrumentId inst,
                         std::string_view funding_id,
                         Notional amount,
                         std::string_view asset,
                         std::int64_t time_ms,
                         bool replayed) noexcept {
  FundingMsg m{};
  init_header(m, EventType::Funding, inst, venue);
  m.hdr.exch_ts = Timestamp{time_ms * 1'000'000};
  m.amount = amount;
  m.funding_id.assign(funding_id);
  m.asset.assign(asset);
  m.flags = replayed ? FundingMsg::kReplayed : std::uint8_t{0};
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

// The connector cannot trade on this venue any more (error map HardStop / Fatal, failed
// authentication): asks the engine to trip this venue's kill switch only
// (ControlCommand::TripVenueKill, the reason in `arg`). Travels with the order events, so it is
// journaled and replayed like them. Connectors send it once, on the first such error.
inline void emit_venue_kill(EventSink& sink, VenueId venue, KillReason reason) noexcept {
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId::invalid(), venue);
  m.command = ControlCommand::TripVenueKill;
  m.arg = static_cast<std::uint64_t>(reason);
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  static_cast<void>(sink.push(m.hdr));
}

// channel: 0 = market data, 1 = order/user stream (ConnectionStateMsg contract).
inline void emit_connection_state(EventSink& sink,
                                  VenueId venue,
                                  std::uint8_t channel,
                                  ConnState state,
                                  std::int32_t reason = 0) noexcept {
  ConnectionStateMsg m{};
  init_header(m, EventType::ConnectionState, InstrumentId::invalid(), venue);
  m.state = state;
  m.channel = channel;
  m.reason_code = reason;
  m.hdr.recv_ts = wall_now();
  static_cast<void>(sink.push(m.hdr));
}

}  // namespace fastmm::venues
