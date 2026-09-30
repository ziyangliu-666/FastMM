#include "fastmm/venues/coinbase/coinbase_md_parser.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/level_spill.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdlib>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct CoinbaseMdParser::Impl {
  od::parser parser;
  LevelSpill bid_spill;
  LevelSpill ask_spill;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

CoinbaseMdParser::CoinbaseMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
CoinbaseMdParser::~CoinbaseMdParser() = default;

namespace {

constexpr std::uint32_t kSide = kMaxBookLevelsPerMsg;

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

// One side of a book message: the first kSide levels in the message itself, more in the spill.
struct SideAcc {
  Level* region = nullptr;
  LevelSpill* spill = nullptr;
  std::uint32_t n = 0;
  bool spilled = false;
  bool overflow = false;

  void add(Level l) noexcept {
    if (!spilled) {
      if (n < kSide) {
        region[n++] = l;
        return;
      }
      std::copy_n(region, n, spill->data());
      spilled = true;
    }
    if (n >= LevelSpill::kCapacity) {
      overflow = true;
      return;
    }
    spill->data()[n++] = l;
  }
  // Keeps the kSide levels nearest the touch in the region; true when some were dropped.
  bool finish(bool bids) noexcept {
    if (!spilled) return false;
    n = spill->keep_nearest(n, bids, region, kSide);
    return true;
  }
};

// [[px, sz], ...] of a snapshot, best first: the first kSide into `out`, the rest only counted.
// -1 malformed or out of order, else the levels kept; `cut` when there were more.
int read_snapshot_side(od::value v, Level* out, bool bids, bool& cut) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != sj::SUCCESS) return -1;
  std::uint32_t n = 0;
  for (auto lvl_res : arr) {
    if (n == kSide) {
      cut = true;
      break;  // On-Demand skips the rest of the array when the object moves on
    }
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
    if (n > 0 && (bids ? p->raw >= out[n - 1].price.raw : p->raw <= out[n - 1].price.raw))
      return -1;
    out[n++] = Level{*p, *q};
  }
  return static_cast<int>(n);
}

// [["buy"|"sell", px, sz], ...] of an l2update. False when malformed.
bool read_changes(od::value v, SideAcc& bids, SideAcc& asks) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != sj::SUCCESS) return false;
  for (auto ch_res : arr) {
    od::array entry;
    if (ch_res.get_array().get(entry) != sj::SUCCESS) return false;
    std::string_view f[3];
    int idx = 0;
    for (auto item : entry) {
      if (idx < 3 && item.get_string().get(f[idx]) != sj::SUCCESS) return false;
      ++idx;
    }
    if (idx < 3) return false;
    const auto p = parse_price(f[1]);
    const auto q = parse_qty(f[2]);
    if (!p || !q) return false;
    if (f[0] == "buy") {
      bids.add(Level{*p, *q});
    } else if (f[0] == "sell") {
      asks.add(Level{*p, *q});
    } else {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Timestamp time_of(std::string_view t) noexcept {
  const std::int64_t ns = parse_time_ns(t);
  return Timestamp{ns > 0 ? ns : 0};
}

}  // namespace

MdDecodeResult CoinbaseMdParser::decode(std::string_view json,
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
  SideAcc bids{levels, &impl_->bid_spill};
  SideAcc asks{levels + kSide, &impl_->ask_spill};
  std::string_view type;
  std::string_view product;
  std::string_view time;
  std::string_view side;
  std::string_view price;
  std::string_view size;
  std::string_view message;
  std::string_view reason;
  std::uint64_t trade_id = 0;
  bool have_trade_id = false;
  bool snapshot = false;
  bool update = false;
  bool cut_bids = false;
  bool cut_asks = false;
  bool bad_levels = false;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key == "type") {
      if (field.value().get_string().get(type) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "product_id") {
      if (field.value().get_string().get(product) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "time") {
      if (field.value().get_string().get(time) != sj::SUCCESS) time = {};
    } else if (key == "asks" || key == "bids") {
      const bool is_bids = key == "bids";
      bool& cut = is_bids ? cut_bids : cut_asks;
      const int n =
          read_snapshot_side(field.value(), is_bids ? bids.region : asks.region, is_bids, cut);
      // Out of order: the product is named after this, so the feed can start its book over.
      if (n < 0) bad_levels = true;
      (is_bids ? bids : asks).n = n < 0 ? 0 : static_cast<std::uint32_t>(n);
      snapshot = true;
    } else if (key == "changes") {
      if (!read_changes(field.value(), bids, asks)) return malformed(stats_, r);
      update = true;
    } else if (key == "trade_id" || key == "last_trade_id") {
      if (field.value().get_uint64().get(trade_id) != sj::SUCCESS) return malformed(stats_, r);
      have_trade_id = true;
    } else if (key == "side") {
      if (field.value().get_string().get(side) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "price") {
      if (field.value().get_string().get(price) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "size") {
      if (field.value().get_string().get(size) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "message") {
      if (field.value().get_string().get(message) != sj::SUCCESS) message = {};
    } else if (key == "reason") {
      if (field.value().get_string().get(reason) != sj::SUCCESS) reason = {};
    }
  }

  if (type == "subscriptions" || type == "error") {
    ++stats_.control;
    r.control = type == "error" ? ControlOp::Error : ControlOp::Subscriptions;
    r.msg = message;
    r.reason = reason;
    r.status = type == "error" ? ParseStatus::Error : ParseStatus::Ignored;
    return r;
  }
  const bool book = type == "snapshot" || type == "l2update";
  const bool trade = type == "match" || type == "last_match";
  const bool heartbeat = type == "heartbeat";
  if (!book && !trade && !heartbeat) {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  if (product.empty()) return malformed(stats_, r);
  const InstrumentId inst = symbols_.find(venue_, product);
  if (!inst.valid()) {
    ++stats_.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    return r;
  }
  r.instrument = inst;
  if (bad_levels) return malformed(stats_, r);

  if (heartbeat) {
    if (!have_trade_id) return malformed(stats_, r);
    ++stats_.heartbeats;
    r.control = ControlOp::Heartbeat;
    r.trade_id = trade_id;
    r.status = ParseStatus::Ignored;
    return r;
  }

  if (trade) {
    if (!have_trade_id) return malformed(stats_, r);
    r.trade_id = trade_id;
    if (type == "last_match") {
      ++stats_.control;
      r.control = ControlOp::LastMatch;
      r.status = ParseStatus::Ignored;
      return r;
    }
    const auto px = parse_price(price);
    const auto qty = parse_qty(size);
    if (!px || !qty || (side != "buy" && side != "sell")) return malformed(stats_, r);
    auto* t = reinterpret_cast<TradeMsg*>(out.data());
    init_header(*t, EventType::Trade, inst, venue_);
    t->price = *px;
    t->qty = *qty;
    t->trade_id = trade_id;
    // "The side field indicates the maker order side": the taker took the other one.
    t->aggressor = side == "sell" ? Side::Buy : Side::Sell;
    t->hdr.venue_seq = trade_id;
    t->hdr.exch_ts = time_of(time);
    t->hdr.recv_ts = recv_ts;
    t->hdr.t0_cycles = t0;
    ++stats_.trades;
    r.status = ParseStatus::Ok;
    r.kind = MdKind::Trade;
    r.len = sizeof(TradeMsg);
    r.count = 1;
    return r;
  }

  const bool is_snapshot = type == "snapshot";
  if (is_snapshot ? (!snapshot || update) : (!update || snapshot)) return malformed(stats_, r);
  if (bids.overflow || asks.overflow) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  if (!is_snapshot) {
    cut_bids = bids.finish(true);
    cut_asks = asks.finish(false);
    if (cut_bids || cut_asks) ++stats_.truncated;
  } else if (cut_bids || cut_asks) {
    ++stats_.snapshot_cut;
  }
  const std::uint32_t nb = bids.n;
  const std::uint32_t na = asks.n;
  std::copy_n(asks.region, na, levels + nb);  // close the gap between the sides
  const std::uint32_t len = BookDeltaMsg::size_for(nb, na);
  init_header(*m, is_snapshot ? EventType::BookSnapshot : EventType::BookDelta, inst, venue_, len);
  if (is_snapshot) {
    m->hdr.flags |= EventHeader::kSnapshot;
  } else {
    m->hdr.flags |= truncation_flags(cut_bids, cut_asks);
  }
  m->bid_count = nb;
  m->ask_count = na;
  m->first_update_id = 0;  // assigned by the feed
  m->last_update_id = 0;
  m->prev_update_id = 0;
  m->hdr.exch_ts = time_of(time);
  m->hdr.recv_ts = recv_ts;
  m->hdr.t0_cycles = t0;
  ++(is_snapshot ? stats_.book_snapshots : stats_.book_deltas);
  r.status = ParseStatus::Ok;
  r.kind = is_snapshot ? MdKind::BookSnapshot : MdKind::BookDelta;
  r.len = len;
  r.count = 1;
  return r;
}

}  // namespace fastmm::venues::coinbase
