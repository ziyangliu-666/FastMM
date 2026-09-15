#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::binance_usdm {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct BinanceUsdmMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BinanceUsdmMdParser::BinanceUsdmMdParser(const SymbolTable& symbols,
                                         VenueId venue,
                                         std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
BinanceUsdmMdParser::~BinanceUsdmMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// [["px","qty"],...] into `levels`: the count, -1 on a malformed level, -2 above `max` levels.
int read_levels(od::value arr_val, Level* levels, std::uint32_t max) noexcept {
  od::array arr;
  if (arr_val.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto lvl_res : arr) {
    if (n >= max) return -2;
    od::array pair;
    if (lvl_res.get_array().get(pair) != sj::SUCCESS) return -1;
    std::string_view px;
    std::string_view qty;
    int idx = 0;
    for (auto item : pair) {
      std::string_view s;
      if (item.get_string().get(s) != sj::SUCCESS) return -1;
      if (idx == 0) {
        px = s;
      } else if (idx == 1) {
        qty = s;
      }
      ++idx;
    }
    if (idx < 2) return -1;
    const auto p = parse_price(px);
    const auto q = parse_qty(qty);
    if (!p || !q) return -1;
    levels[n++] = Level{*p, *q};
  }
  return static_cast<int>(n);
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
}

struct DecodeCtx {
  MdParserStats* stats;
  const SymbolTable* symbols;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
};

DecodeResult fail(MdParserStats& stats, DecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  return r;
}
DecodeResult unknown(MdParserStats& stats, DecodeResult r) noexcept {
  ++stats.unknown_symbol;
  r.status = ParseStatus::UnknownSymbol;
  return r;
}
DecodeResult overflow(MdParserStats& stats, DecodeResult r) noexcept {
  ++stats.overflow;
  r.status = ParseStatus::Overflow;
  return r;
}

// Bids then asks of a depth payload into the message's level array.
// Returns Ok, Malformed or Overflow in `r.status` (Ok leaves r otherwise untouched).
[[gnu::noinline]] bool read_book_sides(MdParserStats& stats,
                                       od::value bids,
                                       od::object& obj,
                                       const char* ask_key,
                                       BookDeltaMsg& m,
                                       DecodeResult& r) noexcept {
  Level* levels = m.levels();
  const int nb = read_levels(bids, levels, kMaxBookLevelsPerMsg);
  if (nb == -1) {
    r = fail(stats, r);
    return false;
  }
  if (nb == -2) {
    r = overflow(stats, r);
    return false;
  }
  od::value asks;
  if (obj[ask_key].get(asks) != sj::SUCCESS) {
    r = fail(stats, r);
    return false;
  }
  const int na = read_levels(asks, levels + nb, kMaxBookLevelsPerMsg);
  if (na == -1) {
    r = fail(stats, r);
    return false;
  }
  if (na == -2) {
    r = overflow(stats, r);
    return false;
  }
  m.bid_count = static_cast<std::uint32_t>(nb);
  m.ask_count = static_cast<std::uint32_t>(na);
  return true;
}

[[gnu::noinline]] DecodeResult decode_depth(const DecodeCtx& c,
                                            od::object& data,
                                            DecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  std::int64_t tx_time = 0;
  std::string_view sym;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::uint64_t prev = 0;
  // Wire order: e, E, T, s, ps, U, u, pu, b, a, st. T is the matching-engine time.
  if (data["T"].get_int64().get(tx_time) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["U"].get_uint64().get(first) != sj::SUCCESS) return fail(stats, r);
  if (data["u"].get_uint64().get(last) != sj::SUCCESS) return fail(stats, r);
  if (data["pu"].get_uint64().get(prev) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) return unknown(stats, r);
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  od::value bids;
  if (data["b"].get(bids) != sj::SUCCESS) return fail(stats, r);
  if (!read_book_sides(stats, bids, data, "a", *m, r)) return r;
  const std::uint32_t len = BookDeltaMsg::size_for(m->bid_count, m->ask_count);
  const std::uint32_t bid_count = m->bid_count;
  const std::uint32_t ask_count = m->ask_count;
  init_header(*m, EventType::BookDelta, inst, c.venue, len);
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = first;
  m->last_update_id = last;
  m->prev_update_id = prev;
  m->hdr.venue_seq = last;
  m->hdr.exch_ts = ts_from_ms(tx_time);
  stamp(*m, c.recv_ts, c.t0);
  ++stats.book_deltas;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookDelta;
  r.len = len;
  return r;
}

[[gnu::noinline]] DecodeResult decode_ticker(const DecodeCtx& c,
                                             od::object& data,
                                             DecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  // Wire order: e, u, s, ps, b, B, a, A, T, E, st.
  std::uint64_t upd = 0;
  std::string_view sym;
  std::string_view b;
  std::string_view bq;
  std::string_view a;
  std::string_view aq;
  std::int64_t tx_time = 0;
  if (data["u"].get_uint64().get(upd) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["b"].get_string().get(b) != sj::SUCCESS) return fail(stats, r);
  if (data["B"].get_string().get(bq) != sj::SUCCESS) return fail(stats, r);
  if (data["a"].get_string().get(a) != sj::SUCCESS) return fail(stats, r);
  if (data["A"].get_string().get(aq) != sj::SUCCESS) return fail(stats, r);
  if (data["T"].get_int64().get(tx_time) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) return unknown(stats, r);
  const auto bp = parse_price(b);
  const auto bqty = parse_qty(bq);
  const auto ap = parse_price(a);
  const auto aqty = parse_qty(aq);
  if (!bp || !bqty || !ap || !aqty) return fail(stats, r);
  auto* m = reinterpret_cast<BookTickerMsg*>(c.out.data());
  init_header(*m, EventType::BookTicker, inst, c.venue);
  m->bid_px = *bp;
  m->bid_qty = *bqty;
  m->ask_px = *ap;
  m->ask_qty = *aqty;
  m->hdr.venue_seq = upd;
  m->hdr.exch_ts = ts_from_ms(tx_time);
  stamp(*m, c.recv_ts, c.t0);
  ++stats.book_tickers;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookTicker;
  r.len = sizeof(BookTickerMsg);
  return r;
}

[[gnu::noinline]] DecodeResult decode_agg_trade(const DecodeCtx& c,
                                                od::object& data,
                                                DecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  // Wire order: e, E, a, s, p, q, nq, f, l, T, m, st.
  std::uint64_t agg_id = 0;
  std::string_view sym;
  std::string_view p;
  std::string_view q;
  std::int64_t trade_time = 0;
  bool buyer_is_maker = false;
  if (data["a"].get_uint64().get(agg_id) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["p"].get_string().get(p) != sj::SUCCESS) return fail(stats, r);
  if (data["q"].get_string().get(q) != sj::SUCCESS) return fail(stats, r);
  if (data["T"].get_int64().get(trade_time) != sj::SUCCESS) return fail(stats, r);
  if (data["m"].get_bool().get(buyer_is_maker) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) return unknown(stats, r);
  const auto px = parse_price(p);
  const auto qty = parse_qty(q);
  if (!px || !qty) return fail(stats, r);
  auto* m = reinterpret_cast<TradeMsg*>(c.out.data());
  init_header(*m, EventType::Trade, inst, c.venue);
  m->price = *px;
  m->qty = *qty;
  m->trade_id = agg_id;
  // "Is the buyer the market maker?": true means the seller crossed the spread.
  m->aggressor = buyer_is_maker ? Side::Sell : Side::Buy;
  m->hdr.venue_seq = agg_id;
  m->hdr.exch_ts = ts_from_ms(trade_time);
  stamp(*m, c.recv_ts, c.t0);
  ++stats.trades;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::Trade;
  r.len = sizeof(TradeMsg);
  return r;
}

enum class StreamKind : std::uint8_t { Depth, Ticker, AggTrade, Other };

StreamKind kind_of_stream(std::string_view stream) noexcept {
  const std::size_t at = stream.find('@');
  if (at == std::string_view::npos) return StreamKind::Other;
  const std::string_view suffix = stream.substr(at);
  // "@depth", "@depth@100ms", "@depth@500ms"; partial depth ("@depth5") is not subscribed.
  if (suffix == "@depth" || suffix.starts_with("@depth@")) return StreamKind::Depth;
  if (suffix == "@bookTicker") return StreamKind::Ticker;
  if (suffix == "@aggTrade") return StreamKind::AggTrade;
  return StreamKind::Other;
}

StreamKind kind_of_event(std::string_view e) noexcept {
  if (e == "depthUpdate") return StreamKind::Depth;
  if (e == "bookTicker") return StreamKind::Ticker;
  if (e == "aggTrade") return StreamKind::AggTrade;
  return StreamKind::Other;
}

}  // namespace

DecodeResult BinanceUsdmMdParser::decode(std::string_view json,
                                         Timestamp recv_ts,
                                         Cycles t0,
                                         std::span<std::byte> out) noexcept {
  ++stats_.frames;
  DecodeResult r;
  if (out.size() < kDecoderScratchBytes) return overflow(stats_, r);
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return fail(stats_, r);
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return fail(stats_, r);
  od::object data;
  StreamKind kind = StreamKind::Other;
  {
    std::string_view stream;
    od::value v;
    if (root["stream"].get(v) == sj::SUCCESS && v.get_string().get(stream) == sj::SUCCESS) {
      if (root["data"].get_object().get(data) != sj::SUCCESS) return fail(stats_, r);
      kind = kind_of_stream(stream);
    } else {
      root.reset();
      data = root;
      std::string_view e;
      od::value ev;
      if (data["e"].get(ev) == sj::SUCCESS && ev.get_string().get(e) == sj::SUCCESS)
        kind = kind_of_event(e);
      data.reset();
    }
  }
  const DecodeCtx c{&stats_, &symbols_, venue_, recv_ts, t0, out};
  switch (kind) {
    case StreamKind::Depth:
      return decode_depth(c, data, r);
    case StreamKind::Ticker:
      return decode_ticker(c, data, r);
    case StreamKind::AggTrade:
      return decode_agg_trade(c, data, r);
    case StreamKind::Other:
      break;
  }
  ++stats_.ignored;
  r.status = ParseStatus::Ignored;
  return r;
}

DecodeResult BinanceUsdmMdParser::decode_depth_snapshot(std::string_view json,
                                                        InstrumentId instrument,
                                                        Timestamp recv_ts,
                                                        Cycles t0,
                                                        std::span<std::byte> out) noexcept {
  DecodeResult r;
  if (out.size() < kDecoderScratchBytes) return overflow(stats_, r);
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return fail(stats_, r);
  std::uint64_t last = 0;
  if (root["lastUpdateId"].get_uint64().get(last) != sj::SUCCESS) return fail(stats_, r);
  std::int64_t tx_time = 0;
  if (root["T"].get_int64().get(tx_time) != sj::SUCCESS) tx_time = 0;
  root.reset();
  auto* m = reinterpret_cast<BookDeltaMsg*>(out.data());
  od::value bids;
  if (root["bids"].get(bids) != sj::SUCCESS) return fail(stats_, r);
  if (!read_book_sides(stats_, bids, root, "asks", *m, r)) return r;
  const std::uint32_t bid_count = m->bid_count;
  const std::uint32_t ask_count = m->ask_count;
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, EventType::BookSnapshot, instrument, venue_, len);
  m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = last;
  m->last_update_id = last;
  m->prev_update_id = 0;
  m->hdr.venue_seq = last;
  if (tx_time != 0) m->hdr.exch_ts = ts_from_ms(tx_time);
  stamp(*m, recv_ts, t0);
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookSnapshot;
  r.len = len;
  return r;
}

}  // namespace fastmm::venues::binance_usdm
