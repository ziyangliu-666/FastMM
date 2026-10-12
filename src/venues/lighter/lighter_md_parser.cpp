#include "fastmm/venues/lighter/lighter_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::lighter {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct LighterMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

LighterMdParser::LighterMdParser(const MarketMap& markets, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), markets_(markets), venue_(venue) {}
LighterMdParser::~LighterMdParser() = default;

std::int64_t market_of_channel(std::string_view channel) noexcept {
  const std::size_t c = channel.rfind(':');
  if (c == std::string_view::npos) return -1;
  const auto v = parse_int64(channel.substr(c + 1));
  return v ? *v : -1;
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// {"price":"226.346","size":"11.941"} -> a level; false if unreadable.
[[gnu::noinline]] bool read_level(od::value v, Level& out) noexcept {
  od::object o;
  if (v.get_object().get(o) != sj::SUCCESS) return false;
  std::string_view px;
  std::string_view sz;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "price") {
      if (field.value().get_string().get(px) != sj::SUCCESS) return false;
    } else if (key == "size") {
      if (field.value().get_string().get(sz) != sj::SUCCESS) return false;
    }
  }
  if (px.empty() || sz.empty()) return false;
  const auto p = parse_price(px);
  const auto q = parse_rounded<Qty>(sz);
  if (!p || !q) return false;
  out = Level{*p, *q};
  return true;
}

// [{price,size},...] into `levels` (at most `max`): count, -1 malformed, -2 too many.
[[gnu::noinline]] int read_side(od::value arr_val, Level* levels, std::uint32_t max) noexcept {
  od::array arr;
  if (arr_val.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto item : arr) {
    if (n >= max) return -2;
    od::value v;
    if (item.get(v) != sj::SUCCESS || !read_level(v, levels[n])) return -1;
    ++n;
  }
  return static_cast<int>(n);
}

struct Book {
  int bids = -1;
  int asks = -1;
  std::uint64_t nonce = 0;
  std::uint64_t begin_nonce = 0;
  std::int64_t updated_us = 0;
  bool overflow = false;
};

// The order_book object: bids at the front of `levels`, asks (which come first) parked at
// levels + kMaxBookLevelsPerMsg and moved behind the bids afterwards.
[[gnu::noinline]] bool read_book(od::object& o, Level* levels, Book& b) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "asks" || key == "bids") {
      od::value v;
      if (field.value().get(v) != sj::SUCCESS) return false;
      const bool asks = key == "asks";
      const int n =
          read_side(v, asks ? levels + kMaxBookLevelsPerMsg : levels, kMaxBookLevelsPerMsg);
      if (n == -1) return false;
      if (n == -2) {
        b.overflow = true;
        return true;
      }
      (asks ? b.asks : b.bids) = n;
    } else if (key == "nonce") {
      if (field.value().get_uint64().get(b.nonce) != sj::SUCCESS) return false;
    } else if (key == "begin_nonce") {
      if (field.value().get_uint64().get(b.begin_nonce) != sj::SUCCESS) return false;
    } else if (key == "last_updated_at") {
      if (field.value().get_int64().get(b.updated_us) != sj::SUCCESS) return false;
    }
  }
  return true;
}

struct Ticker {
  Level bid{};
  Level ask{};
  bool have_bid = false;
  bool have_ask = false;
};

[[gnu::noinline]] bool read_ticker(od::object& o, Ticker& t) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "a" || key == "b") {
      od::value v;
      if (field.value().get(v) != sj::SUCCESS) return false;
      bool is_null = false;
      if (v.is_null().get(is_null) != sj::SUCCESS) return false;
      if (is_null) continue;
      if (!read_level(v, key == "a" ? t.ask : t.bid)) return false;
      (key == "a" ? t.have_ask : t.have_bid) = true;
    }
  }
  return true;
}

struct TradeCtx {
  MdParserStats* stats;
  const MarketMap* markets;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
  std::uint32_t written = 0;
  std::uint32_t count = 0;
};

// trades[] entries -> TradeMsg; false if malformed.
[[gnu::noinline]] bool read_trades(TradeCtx& c, od::array& arr) noexcept {
  for (auto item : arr) {
    od::object t;
    if (item.get_object().get(t) != sj::SUCCESS) return false;
    std::string_view px;
    std::string_view sz;
    std::int64_t market = -1;
    std::int64_t time = 0;
    std::uint64_t tid = 0;
    bool maker_ask = false;
    for (auto field : t) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
      if (key == "price") {
        if (field.value().get_string().get(px) != sj::SUCCESS) return false;
      } else if (key == "size") {
        if (field.value().get_string().get(sz) != sj::SUCCESS) return false;
      } else if (key == "market_id") {
        if (field.value().get_int64().get(market) != sj::SUCCESS) return false;
      } else if (key == "timestamp") {
        if (field.value().get_int64().get(time) != sj::SUCCESS) return false;
      } else if (key == "trade_id") {
        if (field.value().get_uint64().get(tid) != sj::SUCCESS) return false;
      } else if (key == "is_maker_ask") {
        if (field.value().get_bool().get(maker_ask) != sj::SUCCESS) return false;
      }
    }
    if (px.empty() || sz.empty() || market < 0) return false;
    const InstrumentId inst = c.markets->find(market);
    if (!inst.valid()) {
      ++c.stats->unknown_symbol;
      continue;
    }
    const auto p = parse_price(px);
    const auto q = parse_rounded<Qty>(sz);
    if (!p || !q) return false;
    if (c.written + sizeof(TradeMsg) > c.out.size()) {
      ++c.stats->overflow;
      continue;  // read on: the frame's type, at its end, may say it is the history
    }
    auto* m = reinterpret_cast<TradeMsg*>(c.out.data() + c.written);
    init_header(*m, EventType::Trade, inst, c.venue);
    m->price = *p;
    m->qty = *q;
    m->trade_id = tid;
    m->aggressor = maker_ask ? Side::Buy : Side::Sell;
    m->hdr.venue_seq = tid;
    m->hdr.exch_ts = ts_from_ms(time);
    m->hdr.recv_ts = c.recv_ts;
    m->hdr.t0_cycles = c.t0;
    c.written += sizeof(TradeMsg);
    ++c.count;
  }
  return true;
}

MdDecodeResult malformed(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  return r;
}

}  // namespace

MdDecodeResult LighterMdParser::decode(std::string_view json,
                                       Timestamp recv_ts,
                                       Cycles t0,
                                       std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return malformed(stats_, r);
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return malformed(stats_, r);
  std::string_view channel;
  std::string_view type;
  std::int64_t updated_us = 0;
  std::int64_t timestamp_ms = 0;
  enum class Kind : std::uint8_t { None, Book, Ticker, Trades } kind = Kind::None;
  auto* book_msg = reinterpret_cast<BookDeltaMsg*>(out.data());
  Book book;
  Ticker ticker;
  TradeCtx tc{&stats_, &markets_, venue_, recv_ts, t0, out};
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key == "channel") {
      if (field.value().get_string().get(channel) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "type") {
      if (field.value().get_string().get(type) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "order_book") {
      od::object o;
      if (field.value().get_object().get(o) != sj::SUCCESS ||
          !read_book(o, book_msg->levels(), book))
        return malformed(stats_, r);
      kind = Kind::Book;
    } else if (key == "ticker") {
      od::object o;
      if (field.value().get_object().get(o) != sj::SUCCESS || !read_ticker(o, ticker))
        return malformed(stats_, r);
      kind = Kind::Ticker;
    } else if (key == "trades" || key == "liquidation_trades") {
      od::array a;
      if (field.value().get_array().get(a) != sj::SUCCESS || !read_trades(tc, a))
        return malformed(stats_, r);
      kind = Kind::Trades;
    } else if (key == "last_updated_at") {
      if (field.value().get_int64().get(updated_us) != sj::SUCCESS) updated_us = 0;
    } else if (key == "timestamp") {
      if (field.value().get_int64().get(timestamp_ms) != sj::SUCCESS) timestamp_ms = 0;
    } else if (key == "error") {
      od::object e;
      if (field.value().get_object().get(e) == sj::SUCCESS) {
        for (auto ef : e) {
          std::string_view k;
          if (ef.unescaped_key().get(k) != sj::SUCCESS) break;
          if (k == "code") {
            if (ef.value().get_int64().get(r.error_code) != sj::SUCCESS) r.error_code = 0;
          } else if (k == "message") {
            if (ef.value().get_string().get(r.error) != sj::SUCCESS) r.error = {};
          }
        }
      }
      r.control = ControlOp::Error;
    }
  }
  r.channel = channel;
  if (r.control == ControlOp::Error) {
    ++stats_.control;
    r.status = ParseStatus::Error;
    return r;
  }
  const bool snapshot = type.starts_with("subscribed/");
  const Timestamp exch = updated_us > 0     ? Timestamp{updated_us * 1000}
                         : timestamp_ms > 0 ? ts_from_ms(timestamp_ms)
                                            : Timestamp{};
  switch (kind) {
    case Kind::Book: {
      if (book.overflow) {
        ++stats_.overflow;
        r.status = ParseStatus::Overflow;
        return r;
      }
      if (book.bids < 0 || book.asks < 0) return malformed(stats_, r);
      const InstrumentId inst = markets_.find(market_of_channel(channel));
      if (!inst.valid()) {
        ++stats_.unknown_symbol;
        r.status = ParseStatus::UnknownSymbol;
        return r;
      }
      const auto bids = static_cast<std::uint32_t>(book.bids);
      const auto asks = static_cast<std::uint32_t>(book.asks);
      Level* levels = book_msg->levels();
      if (asks > 0)
        std::memmove(levels + bids, levels + kMaxBookLevelsPerMsg, asks * sizeof(Level));
      const std::uint32_t len = BookDeltaMsg::size_for(bids, asks);
      init_header(
          *book_msg, snapshot ? EventType::BookSnapshot : EventType::BookDelta, inst, venue_, len);
      if (snapshot) book_msg->hdr.flags |= EventHeader::kSnapshot;
      book_msg->bid_count = bids;
      book_msg->ask_count = asks;
      book_msg->first_update_id = snapshot ? book.nonce : book.begin_nonce + 1;
      book_msg->last_update_id = book.nonce;
      book_msg->prev_update_id = snapshot ? 0 : book.begin_nonce;
      book_msg->hdr.venue_seq = book.nonce;
      book_msg->hdr.exch_ts = book.updated_us > 0 ? Timestamp{book.updated_us * 1000} : exch;
      book_msg->hdr.recv_ts = recv_ts;
      book_msg->hdr.t0_cycles = t0;
      ++(snapshot ? stats_.book_snapshots : stats_.book_deltas);
      r.status = ParseStatus::Ok;
      r.kind = snapshot ? MdKind::BookSnapshot : MdKind::BookDelta;
      r.len = len;
      r.count = 1;
      return r;
    }
    case Kind::Ticker: {
      const InstrumentId inst = markets_.find(market_of_channel(channel));
      if (!inst.valid()) {
        ++stats_.unknown_symbol;
        r.status = ParseStatus::UnknownSymbol;
        return r;
      }
      auto* m = reinterpret_cast<BookTickerMsg*>(out.data());
      init_header(*m, EventType::BookTicker, inst, venue_);
      m->bid_px = ticker.have_bid ? ticker.bid.price : Price{};
      m->bid_qty = ticker.have_bid ? ticker.bid.qty : Qty{};
      m->ask_px = ticker.have_ask ? ticker.ask.price : Price{};
      m->ask_qty = ticker.have_ask ? ticker.ask.qty : Qty{};
      m->hdr.exch_ts = exch;
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
      if (snapshot || tc.count == 0) {
        ++stats_.ignored;
        r.status = ParseStatus::Ignored;
        return r;
      }
      stats_.trades += tc.count;
      r.status = ParseStatus::Ok;
      r.kind = MdKind::Trade;
      r.len = tc.written;
      r.count = tc.count;
      return r;
    case Kind::None:
      break;
  }
  ++stats_.control;
  r.status = ParseStatus::Ignored;
  if (type == "pong") {
    r.control = ControlOp::Pong;
  } else if (type == "connected") {
    r.control = ControlOp::Connected;
  } else if (type.starts_with("subscribed/")) {
    r.control = ControlOp::Subscribed;
  } else {
    r.control = ControlOp::Other;
    ++stats_.ignored;
  }
  return r;
}

}  // namespace fastmm::venues::lighter
