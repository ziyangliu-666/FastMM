#include "fastmm/venues/gate/gate_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/perp_state.hpp"

#include <simdjson.h>

#include <charconv>
#include <cstdlib>
#include <system_error>

namespace fastmm::venues::gate {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

static_assert(kJsonPadding == sj::SIMDJSON_PADDING, "RecvBuffer padding must match simdjson");

struct GateMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

GateMdParser::GateMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
GateMdParser::~GateMdParser() = default;

void GateMdParser::set_funding(InstrumentId inst,
                               Duration interval,
                               std::int64_t next_ms) noexcept {
  if (inst.value >= kMaxInstruments) return;
  interval_[inst.value] = interval;
  next_funding_ms_[inst.value] = next_ms;
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// A size Gate sends as a string ("29", "1.5", "-108.5") or as a JSON number (29, -108). Signed:
// `positive` tells the side of a trade. False if unreadable.
[[gnu::noinline]] bool read_size(od::value v, Qty& out, bool& positive) noexcept {
  std::string_view s;
  if (v.get_string().get(s) == sj::SUCCESS) {
    if (s.empty()) return false;
    positive = s.front() != '-';
    const auto q = parse_rounded<Qty>(positive ? s : s.substr(1));
    if (!q) return false;
    out = *q;
    return true;
  }
  std::int64_t i = 0;
  if (v.get_int64().get(i) == sj::SUCCESS) {
    positive = i >= 0;
    out = Qty::from_int(positive ? i : -i);
    return true;
  }
  double d = 0.0;
  if (v.get_double().get(d) != sj::SUCCESS) return false;
  positive = d >= 0.0;
  char buf[48];
  const auto r =
      std::to_chars(buf, buf + sizeof buf, positive ? d : -d, std::chars_format::fixed, 8);
  if (r.ec != std::errc{}) return false;
  const auto q = parse_rounded<Qty>(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
  if (!q) return false;
  out = *q;
  return true;
}

// Reads [["px","size"],...] (obu levels) into `levels` (at most `max`): count, -1 malformed,
// -2 too many.
[[gnu::noinline]] int read_levels(od::value arr_val, Level* levels, std::uint32_t max) noexcept {
  od::array arr;
  if (arr_val.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto lvl_res : arr) {
    if (n >= max) return -2;
    od::array pair;
    if (lvl_res.get_array().get(pair) != sj::SUCCESS) return -1;
    std::string_view px;
    Qty qty{};
    bool positive = true;
    int idx = 0;
    for (auto item : pair) {
      if (idx == 0) {
        if (item.get_string().get(px) != sj::SUCCESS) return -1;
      } else if (idx == 1) {
        if (!read_size(item.value(), qty, positive)) return -1;
      }
      ++idx;
    }
    if (idx < 2) return -1;
    const auto p = parse_price(px);
    if (!p) return -1;
    levels[n++] = Level{*p, qty};
  }
  return static_cast<int>(n);
}

MdDecodeResult malformed(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  return r;
}

MdDecodeResult ignored(MdParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.ignored;
  r.status = ParseStatus::Ignored;
  return r;
}

[[nodiscard]] ControlOp op_of(std::string_view event, std::string_view channel) noexcept {
  if (channel == "futures.pong") return ControlOp::Pong;
  if (channel == "futures.system") return ControlOp::System;
  if (event == "subscribe") return ControlOp::Subscribe;
  if (event == "unsubscribe") return ControlOp::Unsubscribe;
  if (event == "api") return ControlOp::Api;
  return ControlOp::Other;
}

// {"channel":..,"event":"subscribe","error":{"code":2,"message":".."}?,"result":{"status":..}}
[[gnu::noinline]] MdDecodeResult decode_control(MdParserStats& stats,
                                                od::object& root,
                                                std::string_view channel,
                                                std::string_view event,
                                                MdDecodeResult r) noexcept {
  ++stats.control;
  r.channel = channel;
  r.control = op_of(event, channel);
  r.control_success = true;
  root.reset();
  od::value err;
  if (root["error"].get(err) == sj::SUCCESS) {
    od::object eo;
    if (err.get_object().get(eo) == sj::SUCCESS) {
      std::int64_t code = 0;
      if (eo["code"].get_int64().get(code) == sj::SUCCESS) r.error_code = code;
      std::string_view msg;
      if (eo["message"].get_string().get(msg) == sj::SUCCESS) r.error = msg;
      r.control_success = false;
    }
  }
  r.status = r.control_success ? ParseStatus::Ignored : ParseStatus::Error;
  return r;
}

struct DecodeCtx {
  MdParserStats* stats;
  const SymbolTable* symbols;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::int64_t time_ms;  // the frame's time_ms
  std::span<std::byte> out;
};

// futures.obu: result {t, full?, s, U?, u, b, a}. The stream name is "ob.<contract>.<level>".
[[gnu::noinline]] MdDecodeResult decode_obu(const DecodeCtx& c,
                                            od::object& root,
                                            MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::object res;
  if (root["result"].get_object().get(res) != sj::SUCCESS) return malformed(stats, r);
  std::int64_t t = c.time_ms;
  bool full = false;
  std::string_view stream;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  auto* m = reinterpret_cast<BookDeltaMsg*>(c.out.data());
  Level* levels = m->levels();
  int nb = 0;
  int na = 0;
  bool have_u = false;
  bool have_s = false;
  // One pass in the order Gate sends the fields: t, full, s, U, u, b, a.
  for (auto field : res) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
    if (key == "t") {
      if (field.value().get_int64().get(t) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "full") {
      if (field.value().get_bool().get(full) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "s") {
      if (field.value().get_string().get(stream) != sj::SUCCESS) return malformed(stats, r);
      have_s = true;
    } else if (key == "U") {
      if (field.value().get_uint64().get(first) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "u") {
      if (field.value().get_uint64().get(last) != sj::SUCCESS) return malformed(stats, r);
      have_u = true;
    } else if (key == "b") {
      nb = read_levels(field.value(), levels, kMaxBookLevelsPerMsg);
      if (nb == -1) return malformed(stats, r);
      if (nb == -2) {
        ++stats.overflow;
        r.status = ParseStatus::Overflow;
        return r;
      }
    } else if (key == "a") {
      na = read_levels(field.value(), levels + nb, kMaxBookLevelsPerMsg);
      if (na == -1) return malformed(stats, r);
      if (na == -2) {
        ++stats.overflow;
        r.status = ParseStatus::Overflow;
        return r;
      }
    }
  }
  if (!have_s || !have_u) return malformed(stats, r);
  // "ob.BTC_USDT.400" -> BTC_USDT
  std::string_view symbol = stream;
  if (symbol.starts_with("ob.")) symbol.remove_prefix(3);
  if (const std::size_t dot = symbol.rfind('.'); dot != std::string_view::npos)
    symbol = symbol.substr(0, dot);
  const InstrumentId inst = c.symbols->find(c.venue, symbol);
  if (!inst.valid()) {
    ++stats.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  if (!full && first == 0) return malformed(stats, r);
  const auto bid_count = static_cast<std::uint32_t>(nb);
  const auto ask_count = static_cast<std::uint32_t>(na);
  const std::uint32_t len = BookDeltaMsg::size_for(bid_count, ask_count);
  init_header(*m, full ? EventType::BookSnapshot : EventType::BookDelta, inst, c.venue, len);
  if (full) m->hdr.flags |= EventHeader::kSnapshot;
  m->bid_count = bid_count;
  m->ask_count = ask_count;
  m->first_update_id = full ? last : first;
  m->last_update_id = last;
  m->prev_update_id = 0;
  m->hdr.venue_seq = last;
  m->hdr.exch_ts = ts_from_ms(t);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  if (full) {
    ++stats.book_snapshots;
  } else {
    ++stats.book_deltas;
  }
  r.status = ParseStatus::Ok;
  r.kind = full ? MdKind::BookSnapshot : MdKind::BookDelta;
  r.len = len;
  r.count = 1;
  return r;
}

// futures.book_ticker: result {t, u, s, b, B, a, A}.
[[gnu::noinline]] MdDecodeResult decode_book_ticker(const DecodeCtx& c,
                                                    od::object& root,
                                                    MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::object res;
  if (root["result"].get_object().get(res) != sj::SUCCESS) return malformed(stats, r);
  std::int64_t t = c.time_ms;
  std::uint64_t u = 0;
  std::string_view s;
  std::string_view bid;
  std::string_view ask;
  Qty bid_qty{};
  Qty ask_qty{};
  bool positive = true;
  bool have_s = false;
  for (auto field : res) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
    if (key == "t") {
      if (field.value().get_int64().get(t) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "u") {
      if (field.value().get_uint64().get(u) != sj::SUCCESS) {
        std::string_view us;
        if (field.value().get_string().get(us) != sj::SUCCESS) return malformed(stats, r);
        const auto v = parse_int64(us);
        if (!v) return malformed(stats, r);
        u = static_cast<std::uint64_t>(*v);
      }
    } else if (key == "s") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return malformed(stats, r);
      have_s = true;
    } else if (key == "b") {
      if (field.value().get_string().get(bid) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "B") {
      if (!read_size(field.value(), bid_qty, positive)) return malformed(stats, r);
    } else if (key == "a") {
      if (field.value().get_string().get(ask) != sj::SUCCESS) return malformed(stats, r);
    } else if (key == "A") {
      if (!read_size(field.value(), ask_qty, positive)) return malformed(stats, r);
    }
  }
  if (!have_s) return malformed(stats, r);
  const InstrumentId inst = c.symbols->find(c.venue, s);
  if (!inst.valid()) {
    ++stats.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  auto* m = reinterpret_cast<BookTickerMsg*>(c.out.data());
  init_header(*m, EventType::BookTicker, inst, c.venue);
  m->bid_px = Price{};
  m->bid_qty = Qty{};
  m->ask_px = Price{};
  m->ask_qty = Qty{};
  // An empty price means an empty side ("If a is empty string, it means empty asks").
  if (!bid.empty()) {
    const auto p = parse_price(bid);
    if (!p) return malformed(stats, r);
    m->bid_px = *p;
    m->bid_qty = bid_qty;
  }
  if (!ask.empty()) {
    const auto p = parse_price(ask);
    if (!p) return malformed(stats, r);
    m->ask_px = *p;
    m->ask_qty = ask_qty;
  }
  m->hdr.venue_seq = u;
  m->hdr.exch_ts = ts_from_ms(t);
  m->hdr.recv_ts = c.recv_ts;
  m->hdr.t0_cycles = c.t0;
  ++stats.book_tickers;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::BookTicker;
  r.len = sizeof(BookTickerMsg);
  r.count = 1;
  return r;
}

// futures.trades: result [{id, create_time, create_time_ms, price, size, contract, is_internal?}]
[[gnu::noinline]] MdDecodeResult decode_trades(const DecodeCtx& c,
                                               od::object& root,
                                               MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::array arr;
  if (root["result"].get_array().get(arr) != sj::SUCCESS) return malformed(stats, r);
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  for (auto item : arr) {
    od::object t;
    if (item.get_object().get(t) != sj::SUCCESS) return malformed(stats, r);
    std::uint64_t id = 0;
    std::int64_t time_ms = c.time_ms;
    std::string_view price;
    std::string_view contract;
    Qty qty{};
    bool positive = true;
    bool have_contract = false;
    bool have_size = false;
    bool have_price = false;
    for (auto field : t) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
      if (key == "id") {
        if (field.value().get_uint64().get(id) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "create_time_ms") {
        if (field.value().get_int64().get(time_ms) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "price") {
        if (field.value().get_string().get(price) != sj::SUCCESS) return malformed(stats, r);
        have_price = true;
      } else if (key == "size") {
        if (!read_size(field.value(), qty, positive)) return malformed(stats, r);
        have_size = true;
      } else if (key == "contract") {
        if (field.value().get_string().get(contract) != sj::SUCCESS) return malformed(stats, r);
        have_contract = true;
      }
    }
    if (!have_contract || !have_size || !have_price) return malformed(stats, r);
    const InstrumentId inst = c.symbols->find(c.venue, contract);
    if (!inst.valid()) {
      ++stats.unknown_symbol;
      continue;
    }
    const auto px = parse_price(price);
    if (!px) return malformed(stats, r);
    if (written + sizeof(TradeMsg) > c.out.size()) {
      ++stats.overflow;
      break;
    }
    auto* m = reinterpret_cast<TradeMsg*>(c.out.data() + written);
    init_header(*m, EventType::Trade, inst, c.venue);
    m->price = *px;
    m->qty = qty;
    m->trade_id = id;
    m->aggressor = positive ? Side::Buy : Side::Sell;
    m->hdr.venue_seq = id;
    m->hdr.exch_ts = ts_from_ms(time_ms);
    m->hdr.recv_ts = c.recv_ts;
    m->hdr.t0_cycles = c.t0;
    written += sizeof(TradeMsg);
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

// futures.tickers: result [{contract, mark_price, index_price, funding_rate, t, ...}]
[[gnu::noinline]] MdDecodeResult decode_tickers(
    const DecodeCtx& c,
    od::object& root,
    const std::array<Duration, kMaxInstruments>& ival,
    const std::array<std::int64_t, kMaxInstruments>& nxt,
    MdDecodeResult r) noexcept {
  MdParserStats& stats = *c.stats;
  od::array arr;
  if (root["result"].get_array().get(arr) != sj::SUCCESS) return malformed(stats, r);
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  for (auto item : arr) {
    od::object t;
    if (item.get_object().get(t) != sj::SUCCESS) return malformed(stats, r);
    std::string_view contract;
    std::string_view mark;
    std::string_view index;
    std::string_view rate;
    std::int64_t time_ms = c.time_ms;
    std::int64_t interval_s = 0;
    std::int64_t next_apply_s = 0;
    for (auto field : t) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats, r);
      if (key == "contract") {
        if (field.value().get_string().get(contract) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "mark_price") {
        if (field.value().get_string().get(mark) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "index_price") {
        if (field.value().get_string().get(index) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "funding_rate") {
        if (field.value().get_string().get(rate) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "t") {
        if (field.value().get_int64().get(time_ms) != sj::SUCCESS) return malformed(stats, r);
      } else if (key == "funding_interval") {
        if (field.value().get_int64().get(interval_s) != sj::SUCCESS) interval_s = 0;
      } else if (key == "funding_next_apply") {
        if (field.value().get_int64().get(next_apply_s) != sj::SUCCESS) next_apply_s = 0;
      }
    }
    if (contract.empty()) return malformed(stats, r);
    const InstrumentId inst = c.symbols->find(c.venue, contract);
    if (!inst.valid()) {
      ++stats.unknown_symbol;
      continue;
    }
    if (written + sizeof(PerpStateMsg) > c.out.size()) {
      ++stats.overflow;
      break;
    }
    auto* m = reinterpret_cast<PerpStateMsg*>(c.out.data() + written);
    PerpStateBuilder b(*m, inst, c.venue, time_ms, c.recv_ts, c.t0);
    if (!mark.empty()) {
      if (const auto p = parse_rounded<Price>(mark)) b.mark(*p);
    }
    if (!index.empty()) {
      if (const auto p = parse_rounded<Price>(index)) b.index(*p);
    }
    if (!rate.empty()) {
      double fr = 0.0;
      const auto [end, ec] = std::from_chars(rate.data(), rate.data() + rate.size(), fr);
      if (ec == std::errc{} && end == rate.data() + rate.size()) {
        // The push carries the interval and the next settlement; the contract table is the
        // fallback for a frame without them.
        const Duration interval =
            interval_s > 0 ? Duration{interval_s * 1'000'000'000LL} : ival[inst.value];
        const std::int64_t next_ms = next_apply_s > 0 ? next_apply_s * 1000 : nxt[inst.value];
        b.funding(fr, interval, next_ms);
      }
    }
    if (!b.any()) continue;
    written += sizeof(PerpStateMsg);
    ++count;
    ++stats.perp_states;
  }
  if (count == 0) return ignored(stats, r);
  r.status = ParseStatus::Ok;
  r.kind = MdKind::PerpState;
  r.len = written;
  r.count = count;
  return r;
}

}  // namespace

MdDecodeResult GateMdParser::decode(std::string_view json,
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

  // Frames carry time, time_ms, channel, event, (error,) result, in that order.
  std::int64_t time_ms = 0;
  std::string_view channel;
  std::string_view event;
  if (root["time_ms"].get_int64().get(time_ms) != sj::SUCCESS) time_ms = 0;
  root.reset();
  if (root["channel"].get_string().get(channel) != sj::SUCCESS) {
    // A WebSocket API reply ({"request_id":..,"header":..}) is not market data.
    root.reset();
    od::value req;
    if (root["request_id"].get(req) == sj::SUCCESS) {
      ++stats_.control;
      r.control = ControlOp::Api;
      r.status = ParseStatus::Ignored;
      return r;
    }
    return malformed(stats_, r);
  }
  root.reset();
  if (root["event"].get_string().get(event) != sj::SUCCESS) event = {};
  if (event != "update" && event != "all") return decode_control(stats_, root, channel, event, r);

  root.reset();
  const DecodeCtx c{&stats_, &symbols_, venue_, recv_ts, t0, time_ms, out};
  if (channel == "futures.obu") return decode_obu(c, root, r);
  if (channel == "futures.book_ticker") return decode_book_ticker(c, root, r);
  if (channel == "futures.trades") return decode_trades(c, root, r);
  if (channel == "futures.tickers") return decode_tickers(c, root, interval_, next_funding_ms_, r);
  return ignored(stats_, r);
}

}  // namespace fastmm::venues::gate
