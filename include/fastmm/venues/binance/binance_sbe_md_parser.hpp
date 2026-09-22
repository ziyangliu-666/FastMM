#pragma once
// Binance Spot SBE market-data decoder (md_format = "sbe").
//
// Endpoint (sbe-market-data-streams.md): wss://stream-sbe.binance.com[:9443]/ws|/stream,
// Demo Mode wss://demo-stream-sbe.binance.com, testnet wss://stream-sbe.testnet.binance.vision.
// The connection needs an Ed25519 API key in the X-MBX-APIKEY upgrade header (no signature).
// Every binary frame is one SBE message (8-byte header + body) of schema id 1 version 0
// (tools/sbe/binance_spot_stream_1_0.xml, generated flyweights in
// generated/binance_stream_sbe.hpp). Timestamps are microseconds; every price and quantity is
// an int64 mantissa with a per-message exponent field (mbx:exponent).
//
//   <sym>@trade       TradesStreamEvent 10000         one TradeMsg per group entry
//   <sym>@bestBidAsk  BestBidAskStreamEvent 10001     BookTickerMsg, venue_seq = bookUpdateId
//   <sym>@depth20     DepthSnapshotStreamEvent 10002  BookSnapshot, ids = bookUpdateId
//   <sym>@depth       DepthDiffStreamEvent 10003      BookDeltaMsg, first = U, last = u
//
// The output is the JSON parser's (binance_md_parser.hpp) message for message, so BinanceDepthSync
// and the engine cannot tell the formats apart. Header-only: the flyweights inline into the feed.
// No allocation, no exceptions.
#include "fastmm/codecs/sbe/sbe.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/generated/binance_stream_sbe.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::venues::binance {

class BinanceSbeMdParser {
 public:
  BinanceSbeMdParser(const SymbolTable& symbols, VenueId venue) noexcept
      : symbols_(symbols), venue_(venue) {}

  // Decodes one binary frame. For every normalised message `emit(EventHeader&, MdKind)` is called
  // with the message built in `out` (kDecoderScratchBytes; reused between calls, so the callback
  // must consume it before returning). A trade frame can carry several trades. Returns Ok when at
  // least one message was emitted, else the reason (Ignored, Malformed, UnknownSymbol, Overflow).
  template <class Emit>
  ParseStatus decode(std::span<const std::byte> frame,
                     Timestamp recv_ts,
                     Cycles t0,
                     std::span<std::byte> out,
                     Emit&& emit) noexcept {
    ++stats_.frames;
    if (out.size() < kDecoderScratchBytes) return count(ParseStatus::Overflow);
    if (frame.size() < codecs::sbe::MessageHeader::kSize) return count(ParseStatus::Malformed);
    const auto h = codecs::sbe::MessageHeader::load(frame.data());
    if (h.schema_id != sbe_stream::kSchemaId) return count(ParseStatus::Ignored);
    const auto body = frame.subspan(codecs::sbe::MessageHeader::kSize);
    switch (h.template_id) {
      case sbe_stream::DepthDiffStreamEvent::kTemplateId:
        return depth(sbe_stream::DepthDiffStreamEvent(body, h.block_length, h.version),
                     false,
                     recv_ts,
                     t0,
                     out,
                     emit);
      case sbe_stream::BestBidAskStreamEvent::kTemplateId:
        return bbo(sbe_stream::BestBidAskStreamEvent(body, h.block_length, h.version),
                   recv_ts,
                   t0,
                   out,
                   emit);
      case sbe_stream::TradesStreamEvent::kTemplateId:
        return trades(
            sbe_stream::TradesStreamEvent(body, h.block_length, h.version), recv_ts, t0, out, emit);
      case sbe_stream::DepthSnapshotStreamEvent::kTemplateId:
        return depth(sbe_stream::DepthSnapshotStreamEvent(body, h.block_length, h.version),
                     true,
                     recv_ts,
                     t0,
                     out,
                     emit);
      default:
        return count(ParseStatus::Ignored);
    }
  }

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

  // mantissa * 10^exponent as a raw 1e-8 fixed-point value (false if inexact or overflowing).
  [[nodiscard]] static bool to_raw(std::int64_t mantissa,
                                   int exponent,
                                   std::int64_t& raw) noexcept {
    return codecs::sbe::decimal_to_fixed_raw(mantissa, exponent, raw);
  }
  [[nodiscard]] static constexpr Timestamp ts_from_us(std::int64_t us) noexcept {
    return Timestamp{us * 1'000};
  }

 private:
  ParseStatus count(ParseStatus s) noexcept {
    switch (s) {
      case ParseStatus::Malformed:
        ++stats_.malformed;
        break;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        break;
      case ParseStatus::Overflow:
        ++stats_.overflow;
        break;
      case ParseStatus::Ignored:
        ++stats_.ignored;
        break;
      default:
        break;
    }
    return s;
  }

  template <class M>
  static void stamp(M& m, Timestamp recv_ts, Cycles t0) noexcept {
    m.hdr.recv_ts = recv_ts;
    m.hdr.t0_cycles = t0;
  }

  // Reads one side into `levels`; -1 malformed / inexact, -2 more than `max` levels.
  template <class Group>
  static int read_side(const Group& g, int pe, int qe, Level* levels, std::uint32_t max) noexcept {
    if (!g.valid()) return -1;
    if (g.count() > max) return -2;
    std::uint32_t n = 0;
    for (const auto e : g) {
      std::int64_t p = 0;
      std::int64_t q = 0;
      if (!to_raw(e.price(), pe, p) || !to_raw(e.qty(), qe, q)) return -1;
      levels[n++] = Level{Price::from_raw(p), Qty::from_raw(q)};
    }
    return static_cast<int>(n);
  }

  template <class Msg, class Emit>
  ParseStatus depth(const Msg& msg,
                    bool snapshot,
                    Timestamp recv_ts,
                    Cycles t0,
                    std::span<std::byte> out,
                    Emit& emit) noexcept {
    if (!msg.valid()) return count(ParseStatus::Malformed);
    const InstrumentId inst = symbols_.find(venue_, msg.symbol());
    if (!inst.valid()) return count(ParseStatus::UnknownSymbol);
    const int pe = msg.price_exponent();
    const int qe = msg.qty_exponent();
    auto* m = reinterpret_cast<BookDeltaMsg*>(out.data());
    Level* levels = m->levels();
    const int nb = read_side(msg.bids(), pe, qe, levels, kMaxBookLevelsPerMsg);
    if (nb == -2) return count(ParseStatus::Overflow);
    if (nb < 0) return count(ParseStatus::Malformed);
    const int na = read_side(msg.asks(), pe, qe, levels + nb, kMaxBookLevelsPerMsg);
    if (na == -2) return count(ParseStatus::Overflow);
    if (na < 0) return count(ParseStatus::Malformed);
    const auto bid_count = static_cast<std::uint32_t>(nb);
    const auto ask_count = static_cast<std::uint32_t>(na);
    const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
    if (len > kDecoderScratchBytes) return count(ParseStatus::Overflow);
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if constexpr (requires { msg.first_book_update_id(); }) {
      first = static_cast<std::uint64_t>(msg.first_book_update_id());
      last = static_cast<std::uint64_t>(msg.last_book_update_id());
    } else {
      first = last = static_cast<std::uint64_t>(msg.book_update_id());
    }
    init_header(*m, snapshot ? EventType::BookSnapshot : EventType::BookDelta, inst, venue_, len);
    if (snapshot) m->hdr.flags |= EventHeader::kSnapshot;
    m->bid_count = bid_count;
    m->ask_count = ask_count;
    m->first_update_id = first;
    m->last_update_id = last;
    m->prev_update_id = 0;
    m->hdr.venue_seq = last;
    m->hdr.exch_ts = ts_from_us(msg.event_time());
    stamp(*m, recv_ts, t0);
    if (!snapshot) ++stats_.book_deltas;
    emit(m->hdr, snapshot ? MdKind::BookSnapshot : MdKind::BookDelta);
    return ParseStatus::Ok;
  }

  template <class Emit>
  ParseStatus bbo(const sbe_stream::BestBidAskStreamEvent& msg,
                  Timestamp recv_ts,
                  Cycles t0,
                  std::span<std::byte> out,
                  Emit& emit) noexcept {
    if (!msg.valid()) return count(ParseStatus::Malformed);
    const InstrumentId inst = symbols_.find(venue_, msg.symbol());
    if (!inst.valid()) return count(ParseStatus::UnknownSymbol);
    const int pe = msg.price_exponent();
    const int qe = msg.qty_exponent();
    std::int64_t bp = 0;
    std::int64_t bq = 0;
    std::int64_t ap = 0;
    std::int64_t aq = 0;
    if (!to_raw(msg.bid_price(), pe, bp) || !to_raw(msg.bid_qty(), qe, bq) ||
        !to_raw(msg.ask_price(), pe, ap) || !to_raw(msg.ask_qty(), qe, aq))
      return count(ParseStatus::Malformed);
    auto* m = reinterpret_cast<BookTickerMsg*>(out.data());
    init_header(*m, EventType::BookTicker, inst, venue_);
    m->bid_px = Price::from_raw(bp);
    m->bid_qty = Qty::from_raw(bq);
    m->ask_px = Price::from_raw(ap);
    m->ask_qty = Qty::from_raw(aq);
    m->hdr.venue_seq = static_cast<std::uint64_t>(msg.book_update_id());
    m->hdr.exch_ts = ts_from_us(msg.event_time());
    stamp(*m, recv_ts, t0);
    ++stats_.book_tickers;
    emit(m->hdr, MdKind::BookTicker);
    return ParseStatus::Ok;
  }

  template <class Emit>
  ParseStatus trades(const sbe_stream::TradesStreamEvent& msg,
                     Timestamp recv_ts,
                     Cycles t0,
                     std::span<std::byte> out,
                     Emit& emit) noexcept {
    if (!msg.valid()) return count(ParseStatus::Malformed);
    const InstrumentId inst = symbols_.find(venue_, msg.symbol());
    if (!inst.valid()) return count(ParseStatus::UnknownSymbol);
    const int pe = msg.price_exponent();
    const int qe = msg.qty_exponent();
    const Timestamp exch =
        ts_from_us(msg.transact_time() != 0 ? msg.transact_time() : msg.event_time());
    auto* m = reinterpret_cast<TradeMsg*>(out.data());
    std::uint32_t emitted = 0;
    for (const auto t : msg.trades()) {
      std::int64_t p = 0;
      std::int64_t q = 0;
      if (!to_raw(t.price(), pe, p) || !to_raw(t.qty(), qe, q)) {
        ++stats_.malformed;
        continue;
      }
      init_header(*m, EventType::Trade, inst, venue_);
      m->price = Price::from_raw(p);
      m->qty = Qty::from_raw(q);
      m->trade_id = static_cast<std::uint64_t>(t.id());
      // Buyer is the maker -> the seller crossed the spread (same rule as the JSON `m`).
      m->aggressor = t.is_buyer_maker() == sbe_stream::boolEnum::True ? Side::Sell : Side::Buy;
      m->hdr.venue_seq = m->trade_id;
      m->hdr.exch_ts = exch;
      stamp(*m, recv_ts, t0);
      ++stats_.trades;
      ++emitted;
      emit(m->hdr, MdKind::Trade);
    }
    return emitted != 0 ? ParseStatus::Ok : count(ParseStatus::Ignored);
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
};

}  // namespace fastmm::venues::binance
