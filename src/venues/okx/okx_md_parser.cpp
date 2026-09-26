#include "fastmm/venues/okx/okx_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::okx {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct OkxMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

OkxMdParser::OkxMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
OkxMdParser::~OkxMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

MdDecodeResult malformed(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

// Reads [[px, sz, "0", orders], ...] into `levels` and, when `texts` is given, the texts next to
// them: count, -1 malformed, -2 too many.
int read_levels(od::value arr_val, Level* levels, OkxLevelText* texts, std::uint32_t max) noexcept {
  od::array arr;
  if (arr_val.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto lvl_res : arr) {
    if (n >= max) return -2;
    od::array entry;
    if (lvl_res.get_array().get(entry) != sj::SUCCESS) return -1;
    std::string_view px;
    std::string_view sz;
    int idx = 0;
    for (auto item : entry) {
      if (idx < 2) {
        std::string_view s;
        if (item.get_string().get(s) != sj::SUCCESS) return -1;
        (idx == 0 ? px : sz) = s;
      }
      ++idx;
    }
    if (idx < 2) return -1;
    const auto p = parse_price(px);
    const auto q = parse_qty(sz);
    if (!p || !q) return -1;
    levels[n] = Level{*p, *q};
    if (texts != nullptr) texts[n] = OkxLevelText{p->raw, px, sz, q->is_zero()};
    ++n;
  }
  return static_cast<int>(n);
}

[[nodiscard]] ControlOp event_of(std::string_view e) noexcept {
  if (e == "subscribe") return ControlOp::Subscribe;
  if (e == "unsubscribe") return ControlOp::Unsubscribe;
  if (e == "login") return ControlOp::Login;
  if (e == "error") return ControlOp::Error;
  if (e == "channel-conn-count" || e == "channel-conn-count-error")
    return ControlOp::ChannelConnCount;
  if (e == "notice") return ControlOp::Notice;
  return ControlOp::Other;
}

// {"event":..,"arg":{..},"code":"..","msg":"..","connId":".."}
[[gnu::noinline]] MdDecodeResult decode_event(MdParserStats& stats,
                                              od::object& root,
                                              std::string_view event,
                                              MdDecodeResult r) noexcept {
  ++stats.control;
  r.control = event_of(event);
  root.reset();
  od::object arg;
  if (root["arg"].get_object().get(arg) == sj::SUCCESS) {
    std::string_view ch;
    if (arg["channel"].get_string().get(ch) == sj::SUCCESS) r.channel = ch;
  }
  root.reset();
  std::string_view code;
  if (root["code"].get_string().get(code) == sj::SUCCESS) {
    const auto c = parse_int64(code);
    r.code = c ? static_cast<int>(*c) : -1;
  }
  root.reset();
  std::string_view msg;
  if (root["msg"].get_string().get(msg) == sj::SUCCESS) r.msg = msg;
  switch (r.control) {
    case ControlOp::Error:
      r.control_success = false;
      r.status = ParseStatus::Error;
      break;
    case ControlOp::Login:
      r.control_success = r.code == 0;
      r.status = r.control_success ? ParseStatus::Ignored : ParseStatus::Error;
      break;
    default:
      r.control_success = true;
      r.status = ParseStatus::Ignored;
      break;
  }
  return r;
}

struct DecodeCtx {
  MdParserStats* stats;
  InstrumentId inst;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
  std::span<OkxLevelText> texts;  // books: the level texts, bids first
  std::uint32_t* text_count;
  std::uint32_t* text_bids;
};

[[nodiscard]] std::int64_t ts_field(od::object& o) noexcept {
  std::string_view s;
  if (o["ts"].get_string().get(s) != sj::SUCCESS) return 0;
  const auto v = parse_int64(s);
  return v ? *v : 0;
}

// The first (only) element of `data`.
[[nodiscard]] bool first_data(od::value data_val, od::object& item) noexcept {
  od::array data;
  if (data_val.get_array().get(data) != sj::SUCCESS) return false;
  auto it = data.begin();
  if (it == data.end()) return false;
  return (*it).get_object().get(item) == sj::SUCCESS;
}

[[gnu::noinline]] MdDecodeResult decode_books(const DecodeCtx& c,
                                              std::string_view action,
                                              od::value data,
                                              MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  const bool snapshot = action == "snapshot";
  if (!snapshot && action != "update") return malformed(stats, r);
  od::object item;
  if (!first_data(data, item)) return malformed(stats, r);
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  Level* levels = m->levels();
  // Pushed asks first; the message and the texts keep bids first.
  const auto kHalf = static_cast<std::uint32_t>(c.texts.size() / 2);
  OkxLevelText* texts = c.texts.data();
  od::value asks;
  if (item["asks"].get(asks) != sj::SUCCESS) return malformed(stats, r);
  const int na = read_levels(asks, levels + kMaxBookLevelsPerMsg / 2, texts + kHalf, kHalf);
  if (na == -1) return malformed(stats, r);
  od::value bids;
  if (na == -2 || item["bids"].get(bids) != sj::SUCCESS) {
    if (na == -2) {
      ++stats.overflow;
      r.status = ParseStatus::Overflow;
      return r;
    }
    return malformed(stats, r);
  }
  const int nb = read_levels(bids, levels, texts, kHalf);
  if (nb == -1) return malformed(stats, r);
  if (nb == -2) {
    ++stats.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  const auto bid_count = static_cast<std::uint32_t>(nb);
  const auto ask_count = static_cast<std::uint32_t>(na);
  // Close the gap between the bids and the asks.
  for (std::uint32_t i = 0; i < ask_count; ++i) {
    levels[bid_count + i] = levels[kMaxBookLevelsPerMsg / 2 + i];
    texts[bid_count + i] = texts[kHalf + i];
  }
  *c.text_bids = bid_count;
  *c.text_count = bid_count + ask_count;
  const std::int64_t ts = ts_field(item);
  // Deprecated on 2026-06-23 and sent as 0 since: zero means "no checksum".
  std::int64_t checksum = 0;
  if (item["checksum"].get_int64().get(checksum) != sj::SUCCESS) checksum = 0;
  r.has_checksum = checksum != 0;
  r.checksum = static_cast<std::int32_t>(checksum);
  std::int64_t prev_seq = -1;
  std::int64_t seq = 0;
  if (item["prevSeqId"].get_int64().get(prev_seq) != sj::SUCCESS) return malformed(stats, r);
  if (item["seqId"].get_int64().get(seq) != sj::SUCCESS) return malformed(stats, r);
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, snapshot ? EventType::BookSnapshot : EventType::BookDelta, c.inst, c.venue, len);
  if (snapshot) m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = static_cast<std::uint64_t>(seq);
  m->last_update_id = static_cast<std::uint64_t>(seq);
  m->prev_update_id = prev_seq < 0 ? 0 : static_cast<std::uint64_t>(prev_seq);
  m->hdr.venue_seq = static_cast<std::uint64_t>(seq);
  m->hdr.exch_ts = ts_from_ms(ts);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  ++(snapshot ? stats.book_snapshots : stats.book_deltas);
  r.status = ParseStatus::Ok;
  r.kind = snapshot ? MdKind::BookSnapshot : MdKind::BookDelta;
  r.len = len;
  r.count = 1;
  return r;
}

[[gnu::noinline]] MdDecodeResult decode_bbo(const DecodeCtx& c,
                                            od::value data,
                                            MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::object item;
  if (!first_data(data, item)) return malformed(stats, r);
  Level tmp[4];
  od::value asks;
  if (item["asks"].get(asks) != sj::SUCCESS) return malformed(stats, r);
  const int na = read_levels(asks, tmp + 2, nullptr, 2);
  od::value bids;
  if (na < 0 || item["bids"].get(bids) != sj::SUCCESS) return malformed(stats, r);
  const int nb = read_levels(bids, tmp, nullptr, 2);
  if (nb < 0) return malformed(stats, r);
  const std::int64_t ts = ts_field(item);
  std::int64_t seq = 0;
  if (item["seqId"].get_int64().get(seq) != sj::SUCCESS) seq = 0;
  auto* m = reinterpret_cast<BookTickerMsg*>(c.out.data());
  init_header(*m, EventType::BookTicker, c.inst, c.venue);
  if (nb > 0 && !tmp[0].qty.is_zero()) {
    m->bid_px = tmp[0].price;
    m->bid_qty = tmp[0].qty;
  }
  if (na > 0 && !tmp[2].qty.is_zero()) {
    m->ask_px = tmp[2].price;
    m->ask_qty = tmp[2].qty;
  }
  m->hdr.venue_seq = seq > 0 ? static_cast<std::uint64_t>(seq) : 0;
  m->hdr.exch_ts = ts_from_ms(ts);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  ++stats.book_tickers;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookTicker;
  r.len = sizeof(BookTickerMsg);
  r.count = 1;
  return r;
}

[[gnu::noinline]] MdDecodeResult decode_trades(const DecodeCtx& c,
                                               od::value data_val,
                                               MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::array data;
  if (data_val.get_array().get(data) != sj::SUCCESS) return malformed(stats, r);
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  for (auto v : data) {
    od::object t;
    if (v.get_object().get(t) != sj::SUCCESS) return malformed(stats, r);
    std::string_view id;
    std::string_view px_s;
    std::string_view sz_s;
    std::string_view side;
    // Documented field order: instId, tradeId, px, sz, side, ts, count.
    if (t["tradeId"].get_string().get(id) != sj::SUCCESS) return malformed(stats, r);
    if (t["px"].get_string().get(px_s) != sj::SUCCESS) return malformed(stats, r);
    if (t["sz"].get_string().get(sz_s) != sj::SUCCESS) return malformed(stats, r);
    if (t["side"].get_string().get(side) != sj::SUCCESS) return malformed(stats, r);
    const std::int64_t ts = ts_field(t);
    const auto px = parse_price(px_s);
    const auto qty = parse_qty(sz_s);
    const auto tid = parse_int64(id);
    if (!px || !qty || !tid) return malformed(stats, r);
    if (written + sizeof(TradeMsg) > c.out.size()) {
      ++stats.overflow;
      break;
    }
    auto* m = reinterpret_cast<TradeMsg*>(c.out.data() + written);
    init_header(*m, EventType::Trade, c.inst, c.venue);
    m->price = *px;
    m->qty = *qty;
    m->trade_id = static_cast<std::uint64_t>(*tid);
    m->aggressor = side == "sell" ? Side::Sell : Side::Buy;  // "Trade side of taker"
    m->hdr.venue_seq = static_cast<std::uint64_t>(*tid);
    m->hdr.exch_ts = ts_from_ms(ts);
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

MdDecodeResult OkxMdParser::decode(std::string_view json,
                                   Timestamp recv_ts,
                                   Cycles t0,
                                   std::span<std::byte> out) noexcept {
  ++stats_.frames;
  text_count_ = 0;
  text_bids_ = 0;
  MdDecodeResult r;
  if (json == "pong") {
    ++stats_.control;
    r.control = ControlOp::Pong;
    r.control_success = true;
    r.status = ParseStatus::Ignored;
    return r;
  }
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

  // Pushes are {"arg":{..},["action":..,]"data":[..]} in that order; events have "event".
  // One pass: the fields before `data` say how to read it.
  std::string_view channel;
  std::string_view inst_id;
  std::string_view action;
  enum class Kind : std::uint8_t { None, Books, Bbo, Trades } kind = Kind::None;
  InstrumentId inst{};
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key == "event") {
      std::string_view event;
      if (field.value().get_string().get(event) != sj::SUCCESS) return malformed(stats_, r);
      root.reset();
      return decode_event(stats_, root, event, r);
    }
    if (key == "arg") {
      od::object arg;
      if (field.value().get_object().get(arg) != sj::SUCCESS) return malformed(stats_, r);
      if (arg["channel"].get_string().get(channel) != sj::SUCCESS) return malformed(stats_, r);
      if (arg["instId"].get_string().get(inst_id) != sj::SUCCESS) inst_id = {};
      continue;
    }
    if (key == "action") {
      if (field.value().get_string().get(action) != sj::SUCCESS) return malformed(stats_, r);
      continue;
    }
    if (key != "data") continue;
    if (channel == "books" || channel == "books-l2-tbt" || channel == "books50-l2-tbt") {
      kind = Kind::Books;
    } else if (channel == "bbo-tbt") {
      kind = Kind::Bbo;
    } else if (channel == "trades") {
      kind = Kind::Trades;
    } else {
      ++stats_.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    inst = symbols_.find(venue_, inst_id);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      r.status = ParseStatus::UnknownSymbol;
      return r;
    }
    const DecodeCtx c{&stats_, inst, venue_, recv_ts, t0, out, texts_, &text_count_, &text_bids_};
    switch (kind) {
      case Kind::Books:
        return decode_books(c, action, field.value(), r);
      case Kind::Bbo:
        return decode_bbo(c, field.value(), r);
      case Kind::Trades:
        return decode_trades(c, field.value(), r);
      case Kind::None:
        break;
    }
  }
  ++stats_.ignored;
  r.status = ParseStatus::Ignored;
  return r;
}

}  // namespace fastmm::venues::okx
