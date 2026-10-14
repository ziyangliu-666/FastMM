#include "fastmm/venues/hyperliquid/hl_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::hyperliquid {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct HlMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

HlMdParser::HlMdParser(const SymbolTable& symbols,
                       VenueId venue,
                       std::string dex,
                       std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      coins_(symbols, venue, std::move(dex)),
      venue_(venue) {}
HlMdParser::~HlMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

MdDecodeResult malformed(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  return r;
}

MdDecodeResult unknown(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.unknown_symbol;
  r.status = ParseStatus::UnknownSymbol;
  return r;
}

// {"px":"230.32","sz":"10.169","n":2} -> a level; false if unreadable.
[[gnu::noinline]] bool read_level(od::value v, Level& out) noexcept {
  od::object o;
  if (v.get_object().get(o) != sj::SUCCESS) return false;
  std::string_view px;
  std::string_view sz;
  bool have_px = false;
  bool have_sz = false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "px") {
      if (field.value().get_string().get(px) != sj::SUCCESS) return false;
      have_px = true;
    } else if (key == "sz") {
      if (field.value().get_string().get(sz) != sj::SUCCESS) return false;
      have_sz = true;
    }
  }
  if (!have_px || !have_sz) return false;
  const auto p = parse_price(px);
  const auto q = parse_rounded<Qty>(sz);
  if (!p || !q) return false;
  out = Level{*p, *q};
  return true;
}

// [{px,sz,n},...] into `levels` (at most `max`): count, -1 malformed, -2 too many.
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

struct Ctx {
  MdParserStats* stats;
  const CoinLookup* coins;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
};

// data {"coin","time","levels":[[bids],[asks]]}
[[gnu::noinline]] MdDecodeResult decode_l2book(const Ctx& c,
                                               od::object& data,
                                               MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  Level* levels = m->levels();
  std::string_view coin;
  std::int64_t time = 0;
  int nb = -1;
  int na = -1;
  for (auto field : data) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
    if (key == "coin") {
      if (field.value().get_string().get(coin) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "time") {
      if (field.value().get_int64().get(time) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "levels") {
      od::array sides;
      if (field.value().get_array().get(sides) != sj::SUCCESS) return malformed(stats, r);
      int idx = 0;
      for (auto side : sides) {
        od::value v;
        if (side.get(v) != sj::SUCCESS) return malformed(stats, r);
        if (idx == 0) {
          nb = read_side(v, levels, kMaxBookLevelsPerMsg);
          if (nb == -1) return malformed(stats, r);
        } else if (idx == 1) {
          na = read_side(v, levels + (nb > 0 ? nb : 0), kMaxBookLevelsPerMsg);
          if (na == -1) return malformed(stats, r);
        }
        if (nb == -2 || na == -2) {
          ++stats.overflow;
          r.status = ParseStatus::Overflow;
          return r;
        }
        ++idx;
      }
    }
  }
  if (coin.empty() || nb < 0 || na < 0 || time <= 0) return malformed(stats, r);
  const InstrumentId inst = c.coins->find(coin);
  if (!inst.valid()) return unknown(stats, r);
  const auto bids = static_cast<std::uint32_t>(nb);
  const auto asks = static_cast<std::uint32_t>(na);
  const std::uint32_t len = BookDeltaMsg::size_for(bids, asks);
  init_header(*m, EventType::BookSnapshot, inst, c.venue, len);
  m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bids;
  m->ask_count = asks;
  m->first_update_id = static_cast<std::uint64_t>(time);
  m->last_update_id = static_cast<std::uint64_t>(time);
  m->prev_update_id = 0;
  m->hdr.venue_seq = static_cast<std::uint64_t>(time);
  m->hdr.exch_ts = ts_from_ms(time);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  ++stats.book_snapshots;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookSnapshot;
  r.len = len;
  r.count = 1;
  return r;
}

// data {"coin","time","bbo":[{px,sz,n}|null,{px,sz,n}|null]}
[[gnu::noinline]] MdDecodeResult decode_bbo(const Ctx& c,
                                            od::object& data,
                                            MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  std::string_view coin;
  std::int64_t time = 0;
  Level side[2] = {};
  bool have_bbo = false;
  for (auto field : data) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
    if (key == "coin") {
      if (field.value().get_string().get(coin) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "time") {
      if (field.value().get_int64().get(time) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "bbo") {
      od::array arr;
      if (field.value().get_array().get(arr) != sj::SUCCESS) return malformed(stats, r);
      int idx = 0;
      for (auto item : arr) {
        od::value v;
        if (item.get(v) != sj::SUCCESS || idx > 1) return malformed(stats, r);
        bool is_null = false;
        if (v.is_null().get(is_null) != sj::SUCCESS) return malformed(stats, r);
        if (!is_null && !read_level(v, side[idx])) return malformed(stats, r);
        ++idx;
      }
      have_bbo = idx == 2;
    }
  }
  if (coin.empty() || !have_bbo) return malformed(stats, r);
  const InstrumentId inst = c.coins->find(coin);
  if (!inst.valid()) return unknown(stats, r);
  auto* m = reinterpret_cast<BookTickerMsg*>(c.out.data());
  init_header(*m, EventType::BookTicker, inst, c.venue);
  m->bid_px = side[0].price;
  m->bid_qty = side[0].qty;
  m->ask_px = side[1].price;
  m->ask_qty = side[1].qty;
  m->hdr.venue_seq = static_cast<std::uint64_t>(time);
  m->hdr.exch_ts = ts_from_ms(time);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  ++stats.book_tickers;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookTicker;
  r.len = sizeof(BookTickerMsg);
  r.count = 1;
  return r;
}

// "0x" and an even number of hex digits -> an address; empty if it is not one.
[[nodiscard]] PartyId read_address(std::string_view s) noexcept {
  if (s.size() < 4 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s.size() % 2 != 0 ||
      (s.size() - 2) / 2 > PartyId::kMaxBytes)
    return {};
  const auto nibble = [](char ch) noexcept -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::uint8_t raw[PartyId::kMaxBytes]{};
  const std::size_t n = (s.size() - 2) / 2;
  for (std::size_t i = 0; i < n; ++i) {
    const int hi = nibble(s[2 + 2 * i]);
    const int lo = nibble(s[3 + 2 * i]);
    if (hi < 0 || lo < 0) return {};
    raw[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return PartyId::of_address({raw, n});
}

// users [buyer, seller] -> parties; false if the value is not an array (malformed). A pair that
// is not two addresses leaves `out` empty.
[[gnu::noinline]] bool read_users(od::value v, TradeParties& out) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != sj::SUCCESS) return false;
  PartyId ids[2]{};
  std::size_t n = 0;
  for (auto item : arr) {
    std::string_view s;
    if (item.get_string().get(s) != sj::SUCCESS) return false;
    if (n < 2) ids[n] = read_address(s);
    ++n;
  }
  if (n == 2 && ids[0].known() && ids[1].known()) out = TradeParties{ids[0], ids[1]};
  return true;
}

// data [{coin, side, px, sz, time, hash, tid, users}]
[[gnu::noinline]] MdDecodeResult decode_trades(const Ctx& c,
                                               od::array& data,
                                               MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  for (auto item : data) {
    od::object t;
    if (item.get_object().get(t) != sj::SUCCESS) return malformed(stats, r);
    std::string_view coin;
    std::string_view side;
    std::string_view px;
    std::string_view sz;
    std::int64_t time = 0;
    std::uint64_t tid = 0;
    TradeParties users{};
    for (auto field : t) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
      if (key == "coin") {
        if (field.value().get_string().get(coin) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "side") {
        if (field.value().get_string().get(side) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "px") {
        if (field.value().get_string().get(px) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "sz") {
        if (field.value().get_string().get(sz) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "time") {
        if (field.value().get_int64().get(time) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "tid") {
        if (field.value().get_uint64().get(tid) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "users") {
        od::value v;
        if (field.value().get(v) != sj::SUCCESS || !read_users(v, users))
          return malformed(stats, r);
      }
    }
    if (coin.empty() || px.empty() || sz.empty() || (side != "B" && side != "A"))
      return malformed(stats, r);
    const InstrumentId inst = c.coins->find(coin);
    if (!inst.valid()) {
      ++stats.unknown_symbol;
      continue;
    }
    const auto p = parse_price(px);
    const auto q = parse_rounded<Qty>(sz);
    if (!p || !q) return malformed(stats, r);
    const bool named = users.buyer.known();
    const std::uint32_t len = named ? TradeMsg::kSizeWithParties : sizeof(TradeMsg);
    if (written + len > c.out.size()) {
      ++stats.overflow;
      break;
    }
    auto* m = reinterpret_cast<TradeMsg*>(c.out.data() + written);
    init_header(*m, EventType::Trade, inst, c.venue);
    m->price = *p;
    m->qty = *q;
    m->trade_id = tid;
    m->aggressor = side == "B" ? Side::Buy : Side::Sell;
    m->hdr.venue_seq = tid;
    m->hdr.exch_ts = ts_from_ms(time);
    m->hdr.recv_ts = c.recv_ts;
    m->hdr.t0_cycles = c.t0;
    if (named) m->set_parties(users);
    written += len;
    ++count;
    ++stats.trades;
  }
  if (count == 0) {
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.kind = MdKind::Trade;
  r.len = written;
  r.count = count;
  return r;
}

}  // namespace

MdDecodeResult HlMdParser::decode(std::string_view json,
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
  if (root["channel"].get_string().get(channel) != sj::SUCCESS) return malformed(stats_, r);
  r.channel = channel;
  const Ctx c{&stats_, &coins_, venue_, recv_ts, t0, out};
  if (channel == "l2Book" || channel == "bbo") {
    od::object data;
    if (root["data"].get_object().get(data) != sj::SUCCESS) return malformed(stats_, r);
    return channel == "l2Book" ? decode_l2book(c, data, r) : decode_bbo(c, data, r);
  }
  if (channel == "trades") {
    od::array data;
    if (root["data"].get_array().get(data) != sj::SUCCESS) return malformed(stats_, r);
    return decode_trades(c, data, r);
  }
  ++stats_.control;
  r.status = ParseStatus::Ignored;
  if (channel == "pong") {
    r.control = ControlOp::Pong;
  } else if (channel == "subscriptionResponse") {
    r.control = ControlOp::Subscribe;
    od::object data;
    std::string_view method;
    if (root["data"].get_object().get(data) == sj::SUCCESS &&
        data["method"].get_string().get(method) == sj::SUCCESS && method == "unsubscribe")
      r.control = ControlOp::Unsubscribe;
  } else if (channel == "error") {
    r.control = ControlOp::Error;
    std::string_view text;
    if (root["data"].get_string().get(text) == sj::SUCCESS) r.error = text;
    r.status = ParseStatus::Error;
  } else if (channel == "post") {
    r.control = ControlOp::Post;
  } else {
    r.control = ControlOp::Other;
    ++stats_.ignored;
  }
  return r;
}

}  // namespace fastmm::venues::hyperliquid
