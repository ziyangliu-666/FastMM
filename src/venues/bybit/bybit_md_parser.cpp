#include "fastmm/venues/bybit/bybit_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::bybit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct BybitMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BybitMdParser::BybitMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
BybitMdParser::~BybitMdParser() = default;

void BybitMdParser::reset_tickers() noexcept {
  for (Top& t : tops_) t = Top{};
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// Reads [["px","size"],...] into `levels` (at most `max`): count, -1 malformed, -2 too many.
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

// Trade ids are decimal strings on spot ("2100000000188691524"); UUIDs (other categories)
// are folded to 64 bits with FNV-1a so they still dedupe.
[[nodiscard]] std::uint64_t trade_id_of(std::string_view s) noexcept {
  if (!s.empty() && s.size() <= 19) {
    std::uint64_t v = 0;
    bool numeric = true;
    for (char c : s) {
      if (c < '0' || c > '9') {
        numeric = false;
        break;
      }
      v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (numeric) return v;
  }
  std::uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] ControlOp op_of(std::string_view op) noexcept {
  if (op == "subscribe") return ControlOp::Subscribe;
  if (op == "unsubscribe") return ControlOp::Unsubscribe;
  if (op == "ping" || op == "pong") return ControlOp::Pong;
  if (op == "auth") return ControlOp::Auth;
  return ControlOp::Other;
}

// decode() is split per frame kind: one function holding every simdjson lookup
// made gcc's UBSan instrumentation (null, alignment, object-size) take minutes
// to compile this file at -O1.
MdDecodeResult malformed(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  return r;
}

// Control responses: {"success":true,"ret_msg":"subscribe","conn_id":..,"req_id":..,"op":..}
[[gnu::noinline]] MdDecodeResult decode_control(MdParserStats& stats,
                                                od::object& root,
                                                MdDecodeResult r) noexcept {
  root.reset();
  std::string_view op;
  bool success = false;
  if (root["success"].get_bool().get(success) != sj::SUCCESS) success = false;
  std::string_view msg;
  if (root["ret_msg"].get_string().get(msg) == sj::SUCCESS) r.ret_msg = msg;
  std::string_view req;
  if (root["req_id"].get_string().get(req) == sj::SUCCESS) r.req_id = req;
  if (root["op"].get_string().get(op) != sj::SUCCESS) {
    ++stats.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  ++stats.control;
  r.control = op_of(op);
  r.control_success = success;
  r.status = success ? ParseStatus::Ignored : ParseStatus::Error;
  return r;
}

struct DecodeCtx {
  MdParserStats* stats;
  InstrumentId inst;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::int64_t ts;
  bool snapshot;
  std::span<std::byte> out;
};

[[gnu::noinline]] MdDecodeResult decode_depth(const DecodeCtx& c,
                                              od::object& root,
                                              MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::object data;
  if (root["data"].get_object().get(data) != sj::SUCCESS) return malformed(stats, r);
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  Level* levels = m->levels();
  od::value bids;
  if (data["b"].get(bids) != sj::SUCCESS) return malformed(stats, r);
  const int nb = read_levels(bids, levels, kMaxBookLevelsPerMsg);
  if (nb == -1) return malformed(stats, r);
  if (nb == -2) {
    ++stats.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::value asks;
  if (data["a"].get(asks) != sj::SUCCESS) return malformed(stats, r);
  const int na = read_levels(asks, levels + nb, kMaxBookLevelsPerMsg);
  if (na == -1) return malformed(stats, r);
  if (na == -2) {
    ++stats.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  std::uint64_t u = 0;
  std::uint64_t seq = 0;
  if (data["u"].get_uint64().get(u) != sj::SUCCESS) return malformed(stats, r);
  if (data["seq"].get_uint64().get(seq) != sj::SUCCESS) seq = 0;
  std::int64_t cts = 0;
  if (root["cts"].get_int64().get(cts) != sj::SUCCESS) cts = c.ts;
  const bool snapshot = c.snapshot;
  const auto bid_count = static_cast<std::uint32_t>(nb);
  const auto ask_count = static_cast<std::uint32_t>(na);
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, snapshot ? EventType::BookSnapshot : EventType::BookDelta, c.inst, c.venue, len);
  if (snapshot) m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = u;
  m->last_update_id = u;
  m->prev_update_id = 0;
  m->hdr.venue_seq = seq;
  m->hdr.exch_ts = ts_from_ms(cts);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  if (snapshot) {
    ++stats.book_snapshots;
  } else {
    ++stats.book_deltas;
  }
  r.status = ParseStatus::Ok;
  r.kind = snapshot ? MdKind::BookSnapshot : MdKind::BookDelta;
  r.len = len;
  r.count = 1;
  return r;
}

// The fields of an orderbook.1 frame; decode() folds them into the per-instrument top.
struct TopFrame {
  Level tmp[4];
  int nb = 0;
  int na = 0;
  std::uint64_t u = 0;
  std::int64_t cts = 0;
};

// False if the frame is malformed.
[[gnu::noinline]] bool read_top(od::object& root, std::int64_t ts, TopFrame& f) noexcept {
  od::object data;
  if (root["data"].get_object().get(data) != sj::SUCCESS) return false;
  od::value bids;
  if (data["b"].get(bids) != sj::SUCCESS) return false;
  f.nb = read_levels(bids, f.tmp, 2);
  od::value asks;
  if (f.nb < 0 || data["a"].get(asks) != sj::SUCCESS) return false;
  f.na = read_levels(asks, f.tmp + 2, 2);
  if (f.na < 0) return false;
  if (data["u"].get_uint64().get(f.u) != sj::SUCCESS) return false;
  if (root["cts"].get_int64().get(f.cts) != sj::SUCCESS) f.cts = ts;
  return true;
}

[[gnu::noinline]] MdDecodeResult decode_trades(const DecodeCtx& c,
                                               od::object& root,
                                               MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::array data;
  if (root["data"].get_array().get(data) != sj::SUCCESS) return malformed(stats, r);
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  for (auto item : data) {
    od::object t;
    if (item.get_object().get(t) != sj::SUCCESS) return malformed(stats, r);
    std::string_view id;
    std::int64_t trade_time = 0;
    std::string_view p;
    std::string_view v;
    std::string_view side;
    std::uint64_t seq = 0;
    // Recorded field order: i, T, p, v, S, seq, s, BT, RPI.
    if (t["i"].get_string().get(id) != sj::SUCCESS) return malformed(stats, r);
    if (t["T"].get_int64().get(trade_time) != sj::SUCCESS) return malformed(stats, r);
    if (t["p"].get_string().get(p) != sj::SUCCESS) return malformed(stats, r);
    if (t["v"].get_string().get(v) != sj::SUCCESS) return malformed(stats, r);
    if (t["S"].get_string().get(side) != sj::SUCCESS) return malformed(stats, r);
    if (t["seq"].get_uint64().get(seq) != sj::SUCCESS) seq = 0;
    const auto px = parse_price(p);
    const auto qty = parse_qty(v);
    if (!px || !qty) return malformed(stats, r);
    if (written + sizeof(TradeMsg) > c.out.size()) {
      ++stats.overflow;
      break;
    }
    auto* m = reinterpret_cast<TradeMsg*>(c.out.data() + written);
    init_header(*m, EventType::Trade, c.inst, c.venue);
    m->price = *px;
    m->qty = *qty;
    m->trade_id = trade_id_of(id);
    m->aggressor = side == "Sell" ? Side::Sell : Side::Buy;  // S = taker side
    m->hdr.venue_seq = seq;
    m->hdr.exch_ts = ts_from_ms(trade_time);
    m->hdr.recv_ts = c.recv_ts;
    m->hdr.t0_cycles = c.t0;
    written += sizeof(TradeMsg);
    ++count;
    ++stats.trades;
  }
  if (count == 0) {
    ++stats.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.kind = MdKind::Trade;
  r.len = written;
  r.count = count;
  return r;
}

}  // namespace

MdDecodeResult BybitMdParser::decode(std::string_view json,
                                     Timestamp recv_ts,
                                     Cycles t0,
                                     std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);

  std::string_view topic;
  if (root["topic"].get_string().get(topic) != sj::SUCCESS) return decode_control(stats_, root, r);

  constexpr std::string_view kBook = "orderbook.";
  constexpr std::string_view kTrade = "publicTrade.";
  enum class Kind { Depth, TopOfBook, Trades } kind;
  std::string_view symbol;
  if (topic.starts_with(kBook)) {
    const std::string_view rest = topic.substr(kBook.size());
    const std::size_t dot = rest.find('.');
    if (dot == std::string_view::npos) return malformed(stats_, r);
    kind = rest.substr(0, dot) == "1" ? Kind::TopOfBook : Kind::Depth;
    symbol = rest.substr(dot + 1);
  } else if (topic.starts_with(kTrade)) {
    kind = Kind::Trades;
    symbol = topic.substr(kTrade.size());
  } else {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  const InstrumentId inst = symbols_.find(venue_, symbol);
  if (!inst.valid()) {
    ++stats_.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }

  std::int64_t ts = 0;
  if (root["ts"].get_int64().get(ts) != sj::SUCCESS) return malformed(stats_, r);
  std::string_view type;
  if (root["type"].get_string().get(type) != sj::SUCCESS) return malformed(stats_, r);
  const bool snapshot = type == "snapshot";
  const DecodeCtx c{&stats_, inst, venue_, recv_ts, t0, ts, snapshot, out};

  switch (kind) {
    case Kind::Depth:
      return decode_depth(c, root, r);
    case Kind::TopOfBook: {
      TopFrame f;
      if (!read_top(root, ts, f)) return malformed(stats_, r);
      Top& top = tops_[inst.value];
      if (snapshot) top = Top{};
      if (f.nb > 0) {
        top.bid_px = f.tmp[0].qty.is_zero() ? Price{} : f.tmp[0].price;
        top.bid_qty = f.tmp[0].qty;
      }
      if (f.na > 0) {
        top.ask_px = f.tmp[2].qty.is_zero() ? Price{} : f.tmp[2].price;
        top.ask_qty = f.tmp[2].qty;
      }
      auto* m = reinterpret_cast<BookTickerMsg*>(out.data());
      init_header(*m, EventType::BookTicker, inst, venue_);
      m->bid_px = top.bid_px;
      m->bid_qty = top.bid_qty;
      m->ask_px = top.ask_px;
      m->ask_qty = top.ask_qty;
      m->hdr.venue_seq = f.u;
      m->hdr.exch_ts = ts_from_ms(f.cts);
      m->hdr.recv_ts = recv_ts;
      m->hdr.t0_cycles = t0;
      ++stats_.book_tickers;
      r.status = ParseStatus::Ok;
      r.kind = MdKind::BookTicker;
      r.len = sizeof(BookTickerMsg);
      r.count = 1;
      return r;
    }
    case Kind::Trades:
      return decode_trades(c, root, r);
  }
  return malformed(stats_, r);
}

}  // namespace fastmm::venues::bybit
