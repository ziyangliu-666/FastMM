#include "fastmm/venues/alpaca/alpaca_md.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::alpaca {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct AlpacaMdParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

AlpacaMdParser::AlpacaMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
AlpacaMdParser::~AlpacaMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// The text of a JSON number token, without the whitespace simdjson leaves behind it.
[[nodiscard]] bool number_text(od::value v, std::string_view& out) noexcept {
  od::json_type t{};
  if (v.type().get(t) != sj::SUCCESS || t != od::json_type::number) return false;
  std::string_view raw = v.raw_json_token();
  while (!raw.empty() &&
         (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\r' || raw.back() == '\t'))
    raw.remove_suffix(1);
  out = raw;
  return !out.empty();
}

struct Point {
  std::string_view type;
  std::string_view symbol;
  std::string_view time;
  std::string_view msg;
  std::string_view bp, bs, ap, as;  // quote
  std::string_view p, s;            // trade
  std::uint64_t id = 0;
  std::int64_t code = 0;
};

// One object of the array; false when a field it needs is unreadable.
[[gnu::noinline]] bool read_point(od::object& o, Point& pt) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    od::value v;
    if (field.value().get(v) != sj::SUCCESS) return false;
    if (key.size() == 1) {
      switch (key[0]) {
        case 'T':
          if (v.get_string().get(pt.type) != sj::SUCCESS) return false;
          break;
        case 'S':
          if (v.get_string().get(pt.symbol) != sj::SUCCESS) return false;
          break;
        case 't':
          if (v.get_string().get(pt.time) != sj::SUCCESS) return false;
          break;
        case 'p':
          if (!number_text(v, pt.p)) return false;
          break;
        case 's':
          if (!number_text(v, pt.s)) return false;
          break;
        case 'i':
          if (v.get_uint64().get(pt.id) != sj::SUCCESS) return false;
          break;
        default:
          break;
      }
    } else if (key == "bp") {
      if (!number_text(v, pt.bp)) return false;
    } else if (key == "bs") {
      if (!number_text(v, pt.bs)) return false;
    } else if (key == "ap") {
      if (!number_text(v, pt.ap)) return false;
    } else if (key == "as") {
      if (!number_text(v, pt.as)) return false;
    } else if (key == "msg") {
      if (v.get_string().get(pt.msg) != sj::SUCCESS) return false;
    } else if (key == "code") {
      if (v.get_int64().get(pt.code) != sj::SUCCESS) return false;
    }
  }
  return !pt.type.empty();
}

}  // namespace

MdDecodeResult AlpacaMdParser::decode(std::string_view json,
                                      Timestamp recv_ts,
                                      Cycles t0,
                                      std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  const auto bad = [&]() noexcept {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    return r;
  };
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return bad();
  od::array arr;
  if (doc.get_array().get(arr) != sj::SUCCESS) return bad();
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  bool unknown = false;
  for (auto item : arr) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return bad();
    Point pt;
    if (!read_point(o, pt)) return bad();
    if (pt.type == "q" || pt.type == "t") {
      const InstrumentId inst = symbols_.find(venue_, pt.symbol);
      if (!inst.valid()) {
        ++stats_.unknown_symbol;
        unknown = true;
        continue;
      }
      const std::int64_t ts = coinbase::parse_time_ns(pt.time);
      if (ts < 0) return bad();
      if (pt.type == "q") {
        const std::uint32_t len = BookDeltaMsg::size_for(1, 1);
        if (written + len > out.size()) {
          ++stats_.overflow;
          break;
        }
        const auto bp = parse_price(pt.bp);
        const auto ap = parse_price(pt.ap);
        const auto bs = parse_rounded<Qty>(pt.bs);
        const auto as = parse_rounded<Qty>(pt.as);
        if (!bp || !ap || !bs || !as) return bad();
        auto* m = reinterpret_cast<BookDeltaMsg*>(out.data() + written);
        init_header(*m, EventType::BookSnapshot, inst, venue_, len);
        m->hdr.flags |= EventHeader::kSnapshot;
        Level* lv = m->levels();
        std::uint32_t nb = 0;
        std::uint32_t na = 0;
        if (bp->is_positive() && bs->is_positive()) lv[nb++] = Level{*bp, *bs};
        if (ap->is_positive() && as->is_positive()) lv[nb + na++] = Level{*ap, *as};
        m->bid_count = nb;
        m->ask_count = na;
        m->first_update_id = static_cast<std::uint64_t>(ts);
        m->last_update_id = static_cast<std::uint64_t>(ts);
        m->prev_update_id = 0;
        m->hdr.venue_seq = static_cast<std::uint64_t>(ts);
        m->hdr.exch_ts = Timestamp{ts};
        m->hdr.recv_ts = recv_ts;
        m->hdr.t0_cycles = t0;
        written += len;
        ++stats_.quotes;
      } else {
        if (written + sizeof(TradeMsg) > out.size()) {
          ++stats_.overflow;
          break;
        }
        const auto p = parse_price(pt.p);
        const auto q = parse_rounded<Qty>(pt.s);
        if (!p || !q) return bad();
        auto* m = reinterpret_cast<TradeMsg*>(out.data() + written);
        init_header(*m, EventType::Trade, inst, venue_);
        m->price = *p;
        m->qty = *q;
        m->trade_id = pt.id;
        m->aggressor = Side::Buy;
        m->pad_[0] = TradeMsg::kNoAggressor;
        m->hdr.venue_seq = pt.id;
        m->hdr.exch_ts = Timestamp{ts};
        m->hdr.recv_ts = recv_ts;
        m->hdr.t0_cycles = t0;
        written += sizeof(TradeMsg);
        ++stats_.trades;
      }
      ++count;
      continue;
    }
    ++stats_.control;
    if (pt.type == "success") {
      r.control = pt.msg == "authenticated" ? MdControl::Authenticated
                  : pt.msg == "connected"   ? MdControl::Connected
                                            : MdControl::Other;
      r.msg = pt.msg;
    } else if (pt.type == "subscription") {
      r.control = MdControl::Subscription;
    } else if (pt.type == "error") {
      r.control = MdControl::Error;
      r.error_code = static_cast<int>(pt.code);
      r.msg = pt.msg;
    } else {
      ++stats_.ignored;  // corrections, cancel errors, bars, statuses, LULD
    }
  }
  if (count > 0) {
    r.status = ParseStatus::Ok;
    r.len = written;
    r.count = count;
    return r;
  }
  r.status = r.control == MdControl::Error ? ParseStatus::Error
             : unknown                     ? ParseStatus::UnknownSymbol
                                           : ParseStatus::Ignored;
  return r;
}

}  // namespace fastmm::venues::alpaca
