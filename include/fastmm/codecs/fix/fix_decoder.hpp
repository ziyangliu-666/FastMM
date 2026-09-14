#pragma once
// FixDecoder: FIX 4.4 application messages -> normalised engine events (satisfies
// codecs::Decoder). Messages are built in place in the EventSink; nothing allocates.
//
//   ExecutionReport(8), by ExecType(150):
//     0 New, 5 Replaced          -> OrderAck (ClOrdID 11 = the live id, OrderID 37)
//     8 Rejected                 -> OrderReject (OrdRejReason 103, Text 58)
//     4 Canceled                 -> OrderCancelAck (OrigClOrdID 41, else ClOrdID; CumQty 14)
//     C Expired, 3 Done for day  -> OrderExpired (CumQty 14)
//     F Trade (and legacy 1/2)   -> OrderFill (LastQty 32, LastPx 31, CumQty 14, LeavesQty 151,
//                                   ExecID 17 deduplicated, Side 54, LastLiquidityInd 851)
//     6, A, E pending states, D Restated, I Order Status, G/H trade corrections -> Ignored
//   OrderCancelReject(9), CxlRejReason 102: 1 -> VenueUnknownOrder, else VenueReject
//     CxlRejResponseTo 434 = 1   -> OrderCancelReject for OrigClOrdID(41)
//     CxlRejResponseTo 434 = 2   -> OrderReject for the replacement ClOrdID(11): the OMS resolves a
//                                   pending replace through the new id, as for Binance
//                                   cancelReplace
//   MarketDataSnapshotFullRefresh(W) -> one BookSnapshot (kSnapshot) per message
//   MarketDataIncrementalRefresh(X)  -> one BookDelta per run of book entries on one instrument,
//                                       one Trade per MDEntryType 2 entry
//
// ClOrdIDs are the engine's own ("fm" + 12 hex, see strong_id.hpp); order events for ids that do
// not decode are Ignored (foreign_ids). Order events are delivered even when Symbol(55) is
// unknown (instrument left invalid): losing an order event is worse than a missing instrument.
// Market data needs a known symbol (UnknownSymbol otherwise).
//
// Field/enum sources: OnixS FIX 4.4 dictionary, ExecutionReport (msgType_8_8.html),
// OrderCancelReject (msgType_9_9.html), MarketDataSnapshotFullRefresh (msgType_W_87.html),
// MarketDataIncrementalRefresh (msgType_X_88.html) and the tag pages for 150, 39, 103, 102, 269,
// 279, 851. See docs/codecs-fix.md.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/fix/fix_symbols.hpp"
#include "fastmm/codecs/fix/fix_tags.hpp"
#include "fastmm/codecs/fix/fix_view.hpp"
#include "fastmm/core/fixed_string.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::codecs::fix {

struct FixDecoderStats {
  std::uint64_t messages = 0;
  std::uint64_t acks = 0;
  std::uint64_t rejects = 0;
  std::uint64_t cancel_acks = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t fills = 0;
  std::uint64_t duplicate_fills = 0;
  std::uint64_t expired = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t trades = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t overflow = 0;
};

class FixDecoder {
 public:
  static constexpr std::size_t kExecIdHistory = 256;
  static constexpr std::size_t kMaxMdEntries = std::size_t{2} * kMaxBookLevelsPerMsg;

  FixDecoder(const FixSymbolTable& symbols,
             VenueId venue,
             std::string_view begin_string = kBeginString44) noexcept
      : symbols_(&symbols), venue_(venue), begin_string_(begin_string) {}

  // Decoder: parses (and validates) the frame, then decodes it.
  venues::ParseStatus decode(const FrameView& frame,
                             std::int64_t rx_ts,
                             venues::EventSink& sink) noexcept;
  // Decodes an already parsed message, e.g. FixSession::view() after on_frame() returned false.
  venues::ParseStatus decode_view(const FixView& v,
                                  std::int64_t rx_ts,
                                  venues::EventSink& sink) noexcept;

  void set_verify_checksum(bool on) noexcept { verify_checksum_ = on; }
  [[nodiscard]] const FixDecoderStats& stats() const noexcept { return stats_; }

 private:
  venues::ParseStatus execution_report(const FixView& v,
                                       std::int64_t rx_ts,
                                       venues::EventSink& sink) noexcept;
  venues::ParseStatus cancel_reject(const FixView& v,
                                    std::int64_t rx_ts,
                                    venues::EventSink& sink) noexcept;
  venues::ParseStatus snapshot(const FixView& v,
                               std::int64_t rx_ts,
                               venues::EventSink& sink) noexcept;
  venues::ParseStatus incremental(const FixView& v,
                                  std::int64_t rx_ts,
                                  venues::EventSink& sink) noexcept;
  venues::ParseStatus status(venues::ParseStatus s) noexcept;
  [[nodiscard]] bool exec_id_seen(std::uint64_t h) const noexcept;
  void remember_exec_id(std::uint64_t h) noexcept;

  const FixSymbolTable* symbols_;
  VenueId venue_;
  FixedString<16> begin_string_;
  bool verify_checksum_ = true;
  FixDecoderStats stats_{};
  std::size_t exec_pos_ = 0;
  std::uint64_t exec_ids_[kExecIdHistory]{};
  // Scratch for market data entries: field ranges, instrument and type per entry.
  std::uint32_t entry_begin_[kMaxMdEntries]{};
  std::uint32_t entry_end_[kMaxMdEntries]{};
  std::uint32_t entry_inst_[kMaxMdEntries]{};
  char entry_type_[kMaxMdEntries]{};
  FixView view_;
};

}  // namespace fastmm::codecs::fix
