#include "fastmm/venues/binance/binance_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstring>

namespace fastmm::venues::binance {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct BinanceMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    // Reserve once so no allocation happens per frame.
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BinanceMdParser::BinanceMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
BinanceMdParser::~BinanceMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// Reads [["px","qty"],...] into `levels` (at most `max`), returning the count or -1 on a
// malformed level, -2 if more than `max` levels are present (the rest is not read).
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

// decode() is split per stream kind: one function holding every simdjson lookup
// made gcc's UBSan instrumentation (null, alignment, object-size) take minutes
// to compile this file at -O1.
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

[[gnu::noinline]] DecodeResult decode_depth(const DecodeCtx& c,
                                            od::object& data,
                                            DecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  std::int64_t ev_time = 0;
  std::string_view sym;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  // Fields in wire order: e, E, s, U, u, b, a.
  if (data["E"].get_int64().get(ev_time) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["U"].get_uint64().get(first) != sj::SUCCESS) return fail(stats, r);
  if (data["u"].get_uint64().get(last) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) {
    ++stats.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  Level* levels = m->levels();
  od::value bids;
  od::value asks;
  if (data["b"].get(bids) != sj::SUCCESS) return fail(stats, r);
  const int nb = read_levels(bids, levels, kMaxBookLevelsPerMsg);
  if (nb == -1) return fail(stats, r);
  if (nb == -2) {
    ++stats.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  if (data["a"].get(asks) != sj::SUCCESS) return fail(stats, r);
  const int na = read_levels(asks, levels + nb, kMaxBookLevelsPerMsg);
  if (na == -1) return fail(stats, r);
  if (na == -2) {
    ++stats.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  const auto bid_count = static_cast<std::uint32_t>(nb);
  const auto ask_count = static_cast<std::uint32_t>(na);
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, EventType::BookDelta, inst, c.venue, len);
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = first;
  m->last_update_id = last;
  m->prev_update_id = 0;
  m->hdr.venue_seq = last;
  m->hdr.exch_ts = ts_from_ms(ev_time);
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
  // Fields: u, s, b, B, a, A (no event time on the spot bookTicker stream).
  std::uint64_t upd = 0;
  std::string_view sym;
  std::string_view b;
  std::string_view bq;
  std::string_view a;
  std::string_view aq;
  if (data["u"].get_uint64().get(upd) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["b"].get_string().get(b) != sj::SUCCESS) return fail(stats, r);
  if (data["B"].get_string().get(bq) != sj::SUCCESS) return fail(stats, r);
  if (data["a"].get_string().get(a) != sj::SUCCESS) return fail(stats, r);
  if (data["A"].get_string().get(aq) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) {
    ++stats.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
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
  stamp(*m, c.recv_ts, c.t0);
  ++stats.book_tickers;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookTicker;
  r.len = sizeof(BookTickerMsg);
  return r;
}

[[gnu::noinline]] DecodeResult decode_trade(const DecodeCtx& c,
                                            od::object& data,
                                            DecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  // Fields: e, E, s, t, p, q, T, m, M.
  std::int64_t ev_time = 0;
  std::string_view sym;
  std::uint64_t trade_id = 0;
  std::string_view p;
  std::string_view q;
  std::int64_t trade_time = 0;
  bool buyer_is_maker = false;
  if (data["E"].get_int64().get(ev_time) != sj::SUCCESS) return fail(stats, r);
  if (data["s"].get_string().get(sym) != sj::SUCCESS) return fail(stats, r);
  if (data["t"].get_uint64().get(trade_id) != sj::SUCCESS) return fail(stats, r);
  if (data["p"].get_string().get(p) != sj::SUCCESS) return fail(stats, r);
  if (data["q"].get_string().get(q) != sj::SUCCESS) return fail(stats, r);
  if (data["T"].get_int64().get(trade_time) != sj::SUCCESS) return fail(stats, r);
  if (data["m"].get_bool().get(buyer_is_maker) != sj::SUCCESS) return fail(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, sym);
  if (!inst.valid()) {
    ++stats.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  const auto px = parse_price(p);
  const auto qty = parse_qty(q);
  if (!px || !qty) return fail(stats, r);
  auto* m = reinterpret_cast<TradeMsg*>(c.out.data());
  init_header(*m, EventType::Trade, inst, c.venue);
  m->price = *px;
  m->qty = *qty;
  m->trade_id = trade_id;
  // Buyer is the maker -> the seller crossed the spread.
  m->aggressor = buyer_is_maker ? Side::Sell : Side::Buy;
  m->hdr.venue_seq = trade_id;
  m->hdr.exch_ts = ts_from_ms(trade_time != 0 ? trade_time : ev_time);
  stamp(*m, c.recv_ts, c.t0);
  ++stats.trades;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::Trade;
  r.len = sizeof(TradeMsg);
  return r;
}

}  // namespace

DecodeResult BinanceMdParser::decode(std::string_view json,
                                     Timestamp recv_ts,
                                     Cycles t0,
                                     std::span<std::byte> out) noexcept {
  ++stats_.frames;
  DecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return fail(stats_, r);
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return fail(stats_, r);
  // Combined stream wrapper: {"stream": "<name>", "data": {...}}. Raw streams carry the
  // payload at the top level; then the event type "e" (or the absence of it for bookTicker)
  // decides.
  std::string_view stream;
  od::object data;
  bool wrapped = false;
  {
    od::value v;
    if (root["stream"].get(v) == sj::SUCCESS && v.get_string().get(stream) == sj::SUCCESS) {
      wrapped = true;
      if (root["data"].get_object().get(data) != sj::SUCCESS) return fail(stats_, r);
    } else {
      root.reset();
      data = root;
    }
  }

  enum class Kind { Depth, Ticker, Trade, Other } kind = Kind::Other;
  if (wrapped) {
    const std::size_t at = stream.find('@');
    const std::string_view suffix =
        at == std::string_view::npos ? std::string_view{} : stream.substr(at);
    if (suffix.starts_with("@depth")) {
      kind = Kind::Depth;
    } else if (suffix == "@bookTicker") {
      kind = Kind::Ticker;
    } else if (suffix == "@trade") {
      kind = Kind::Trade;
    }
  } else {
    std::string_view e;
    od::value ev;
    if (data["e"].get(ev) == sj::SUCCESS && ev.get_string().get(e) == sj::SUCCESS) {
      if (e == "depthUpdate") {
        kind = Kind::Depth;
      } else if (e == "trade") {
        kind = Kind::Trade;
      }
    } else {
      // bookTicker has no "e"; it is the only raw payload with "u" and "B"
      data.reset();
      od::value bv;
      if (data["B"].get(bv) == sj::SUCCESS) kind = Kind::Ticker;
    }
    data.reset();
  }

  const DecodeCtx c{&stats_, &symbols_, venue_, recv_ts, t0, out};
  switch (kind) {
    case Kind::Depth:
      return decode_depth(c, data, r);
    case Kind::Ticker:
      return decode_ticker(c, data, r);
    case Kind::Trade:
      return decode_trade(c, data, r);
    case Kind::Other:
      break;
  }
  ++stats_.ignored;
  r.status = ParseStatus::Ignored;
  return r;
}

DecodeResult BinanceMdParser::decode_depth_snapshot(std::string_view json,
                                                    InstrumentId instrument,
                                                    Timestamp recv_ts,
                                                    Cycles t0,
                                                    std::span<std::byte> out) noexcept {
  DecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS) {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    return r;
  }
  std::uint64_t last = 0;
  if (root["lastUpdateId"].get_uint64().get(last) != sj::SUCCESS) {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    return r;
  }
  auto* m = reinterpret_cast<BookDeltaMsg*>(out.data());
  Level* levels = m->levels();
  od::value bids;
  od::value asks;
  if (root["bids"].get(bids) != sj::SUCCESS) {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    return r;
  }
  const int nb = read_levels(bids, levels, kMaxBookLevelsPerMsg);
  if (nb < 0 || root["asks"].get(asks) != sj::SUCCESS) {
    if (nb == -2) {
      ++stats_.overflow;
      r.status = ParseStatus::Overflow;
    } else {
      ++stats_.malformed;
      r.status = ParseStatus::Malformed;
    }
    return r;
  }
  const int na = read_levels(asks, levels + nb, kMaxBookLevelsPerMsg);
  if (na < 0) {
    if (na == -2) {
      ++stats_.overflow;
      r.status = ParseStatus::Overflow;
    } else {
      ++stats_.malformed;
      r.status = ParseStatus::Malformed;
    }
    return r;
  }
  const auto bid_count = static_cast<std::uint32_t>(nb);
  const auto ask_count = static_cast<std::uint32_t>(na);
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, EventType::BookSnapshot, instrument, venue_, len);
  m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = last;
  m->last_update_id = last;
  m->prev_update_id = 0;
  m->hdr.venue_seq = last;
  stamp(*m, recv_ts, t0);
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookSnapshot;
  r.len = len;
  return r;
}

}  // namespace fastmm::venues::binance
