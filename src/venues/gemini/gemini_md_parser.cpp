#include "fastmm/venues/gemini/gemini_md_parser.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/level_spill.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdlib>

namespace fastmm::venues::gemini {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct GeminiMdParser::Impl {
  od::parser parser;
  LevelSpill spill;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

GeminiMdParser::GeminiMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
GeminiMdParser::~GeminiMdParser() = default;

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

// Reads [[px, qty], ...] into `levels` (at most `max`): count, -1 malformed, -2 too many. A side
// longer than `max` goes through the spill, which keeps the `max` levels nearest the touch.
int read_levels(od::array arr,
                Level* levels,
                std::uint32_t max,
                LevelSpill& spill,
                bool bids,
                bool& truncated,
                std::uint64_t& skipped) noexcept {
  std::uint32_t n = 0;
  Level* dst = levels;
  std::uint32_t cap = max;
  for (auto lvl_res : arr) {
    if (n >= cap) {
      if (dst != levels) return -2;
      std::copy_n(levels, n, spill.data());
      dst = spill.data();
      cap = LevelSpill::kCapacity;
    }
    od::array entry;
    if (lvl_res.get_array().get(entry) != sj::SUCCESS) return -1;
    std::string_view px;
    std::string_view qty;
    int idx = 0;
    for (auto item : entry) {
      if (idx < 2) {
        std::string_view s;
        if (item.get_string().get(s) != sj::SUCCESS) return -1;
        (idx == 0 ? px : qty) = s;
      }
      ++idx;
    }
    if (idx < 2) return -1;
    const auto p = parse_price(px);
    const auto q = parse_qty(qty);
    if (!p && p.error() == DecimalError::Overflow) {
      // Production spot books hold asks at 1e10 and more (btcusd, 2026-09-30), past the 8-decimal
      // fixed point: never near the touch. Left out; the side is not marked cut, since no level
      // of the book lies behind it.
      ++skipped;
      continue;
    }
    if (!p || !q) return -1;
    dst[n++] = Level{*p, *q};
  }
  if (dst != levels) {
    n = spill.keep_nearest(n, bids, levels, max);
    truncated = true;
  }
  return static_cast<int>(n);
}

// An id: a string's content, or a number.
void read_id(od::value v, MdControl& c) noexcept {
  od::json_type t{};
  if (v.type().get(t) != sj::SUCCESS) return;
  if (t == od::json_type::string) {
    std::string_view s;
    if (v.get_string().get(s) == sj::SUCCESS) c.id = s;
    return;
  }
  std::int64_t n = 0;
  if (v.get_int64().get(n) == sj::SUCCESS) c.id_number = n;
}

}  // namespace

MdDecodeResult GeminiMdParser::decode(std::string_view json,
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

  auto* m = reinterpret_cast<BookDeltaMsg*>(out.data());
  Level* levels = m->levels();
  constexpr std::uint32_t kHalf = kMaxBookLevelsPerMsg / 2;

  std::string_view event;
  std::string_view symbol;
  std::uint64_t exch_ns = 0;
  std::uint64_t first_id = 0;
  std::uint64_t last_id = 0;
  bool have_first = false;
  bool have_last = false;
  int bid_count = -1;  // depth sides, read into the message
  int ask_count = -1;
  bool cut_bids = false;
  bool cut_asks = false;
  std::string_view bid_px;  // bookTicker
  std::string_view bid_qty;
  std::string_view ask_px;
  std::string_view ask_qty;
  bool have_trade_id = false;
  std::uint64_t trade_id = 0;
  std::string_view trade_px;
  std::string_view trade_qty;
  bool have_maker = false;
  bool buyer_maker = false;
  bool overflow = false;

  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key.size() != 1) {
      if (key == "id") {
        r.control.present = true;
        read_id(field.value(), r.control);
      } else if (key == "status") {
        std::int64_t st = 0;
        if (field.value().get_int64().get(st) != sj::SUCCESS) return malformed(stats_, r);
        r.control.present = true;
        r.control.status = static_cast<int>(st);
      } else if (key == "error") {
        od::object e;
        if (field.value().get_object().get(e) != sj::SUCCESS) return malformed(stats_, r);
        for (auto ef : e) {
          std::string_view ek;
          if (ef.unescaped_key().get(ek) != sj::SUCCESS) return malformed(stats_, r);
          if (ek == "code") {
            std::int64_t c = 0;
            if (ef.value().get_int64().get(c) == sj::SUCCESS)
              r.control.error_code = static_cast<int>(c);
          } else if (ek == "msg") {
            std::string_view msg;
            if (ef.value().get_string().get(msg) == sj::SUCCESS) r.control.msg = msg;
          }
        }
      } else if (key == "result") {
        od::object res;
        if (field.value().get_object().get(res) == sj::SUCCESS) {
          for (auto rf : res) {
            std::string_view rk;
            if (rf.unescaped_key().get(rk) != sj::SUCCESS) break;
            if (rk == "serverTime") {
              std::int64_t t = 0;
              if (rf.value().get_int64().get(t) == sj::SUCCESS) r.control.server_time_ms = t;
            }
          }
        }
      }
      continue;
    }
    switch (key[0]) {
      case 'e':
        if (field.value().get_string().get(event) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'E':
        if (field.value().get_uint64().get(exch_ns) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 's':
        if (field.value().get_string().get(symbol) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'U':
        if (field.value().get_uint64().get(first_id) != sj::SUCCESS) return malformed(stats_, r);
        have_first = true;
        break;
      case 'u':
        if (field.value().get_uint64().get(last_id) != sj::SUCCESS) return malformed(stats_, r);
        have_last = true;
        break;
      case 'b':
      case 'a': {
        const bool bids = key[0] == 'b';
        od::value v = field.value();
        od::json_type t{};
        if (v.type().get(t) != sj::SUCCESS) return malformed(stats_, r);
        if (t == od::json_type::string) {
          if (v.get_string().get(bids ? bid_px : ask_px) != sj::SUCCESS)
            return malformed(stats_, r);
          break;
        }
        od::array arr;
        if (v.get_array().get(arr) != sj::SUCCESS) return malformed(stats_, r);
        const int n = read_levels(arr,
                                  bids ? levels : levels + kHalf,
                                  kHalf,
                                  impl_->spill,
                                  bids,
                                  bids ? cut_bids : cut_asks,
                                  stats_.out_of_range);
        if (n == -1) return malformed(stats_, r);
        if (n == -2) {
          overflow = true;
          break;
        }
        (bids ? bid_count : ask_count) = n;
        break;
      }
      case 'B':
        if (field.value().get_string().get(bid_qty) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'A':
        if (field.value().get_string().get(ask_qty) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 't':
        if (field.value().get_uint64().get(trade_id) != sj::SUCCESS) return malformed(stats_, r);
        have_trade_id = true;
        break;
      case 'p':
        if (field.value().get_string().get(trade_px) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'q':
        if (field.value().get_string().get(trade_qty) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'm':
        if (field.value().get_bool().get(buyer_maker) != sj::SUCCESS) return malformed(stats_, r);
        have_maker = true;
        break;
      default:
        break;
    }
  }

  if (r.control.present) {
    ++stats_.control;
    r.status = r.control.status == 200 ? ParseStatus::Ignored : ParseStatus::Error;
    return r;
  }
  if (overflow) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  const bool depth = event == "depthUpdate";
  const bool ticker = event.empty() && !bid_qty.empty() && !ask_qty.empty();
  const bool trade = event.empty() && have_trade_id && have_maker;
  if (!depth && !ticker && !trade) {
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
  const Timestamp exch_ts{static_cast<std::int64_t>(exch_ns)};

  if (depth) {
    if (!have_first || !have_last || bid_count < 0 || ask_count < 0) return malformed(stats_, r);
    const auto nb = static_cast<std::uint32_t>(bid_count);
    const auto na = static_cast<std::uint32_t>(ask_count);
    std::copy_n(levels + kHalf, na, levels + nb);  // asks right behind the bids
    if (cut_bids || cut_asks) ++stats_.truncated;
    const std::uint32_t len = BookDeltaMsg::size_for(nb, na);
    init_header(*m, EventType::BookDelta, inst, venue_, len);
    m->hdr.flags |= truncation_flags(cut_bids, cut_asks);
    m->bid_count = nb;
    m->ask_count = na;
    m->first_update_id = first_id;
    m->last_update_id = last_id;
    m->prev_update_id = first_id;
    m->hdr.venue_seq = last_id;
    m->hdr.exch_ts = exch_ts;
    m->hdr.recv_ts = recv_ts;
    m->hdr.t0_cycles = t0;
    ++stats_.book_deltas;
    r.status = ParseStatus::Ok;
    r.kind = MdKind::BookDelta;
    r.len = len;
    r.count = 1;
    return r;
  }
  if (ticker) {
    const auto bp = parse_price(bid_px);
    const auto bq = parse_qty(bid_qty);
    const auto ap = parse_price(ask_px);
    const auto aq = parse_qty(ask_qty);
    if (!bp || !bq || !ap || !aq) return malformed(stats_, r);
    auto* t = reinterpret_cast<BookTickerMsg*>(out.data());
    init_header(*t, EventType::BookTicker, inst, venue_);
    if (!bq->is_zero()) {
      t->bid_px = *bp;
      t->bid_qty = *bq;
    }
    if (!aq->is_zero()) {
      t->ask_px = *ap;
      t->ask_qty = *aq;
    }
    t->hdr.venue_seq = have_last ? last_id : 0;
    t->hdr.exch_ts = exch_ts;
    t->hdr.recv_ts = recv_ts;
    t->hdr.t0_cycles = t0;
    ++stats_.book_tickers;
    r.status = ParseStatus::Ok;
    r.kind = MdKind::BookTicker;
    r.len = sizeof(BookTickerMsg);
    r.count = 1;
    return r;
  }
  const auto px = parse_price(trade_px);
  const auto qty = parse_qty(trade_qty);
  if (!px || !qty) return malformed(stats_, r);
  auto* t = reinterpret_cast<TradeMsg*>(out.data());
  init_header(*t, EventType::Trade, inst, venue_);
  t->price = *px;
  t->qty = *qty;
  t->trade_id = trade_id;
  // "m: Whether the buyer is the maker": then the seller took.
  t->aggressor = buyer_maker ? Side::Sell : Side::Buy;
  t->hdr.venue_seq = trade_id;
  t->hdr.exch_ts = exch_ts;
  t->hdr.recv_ts = recv_ts;
  t->hdr.t0_cycles = t0;
  ++stats_.trades;
  r.status = ParseStatus::Ok;
  r.kind = MdKind::Trade;
  r.len = sizeof(TradeMsg);
  r.count = 1;
  return r;
}

}  // namespace fastmm::venues::gemini
