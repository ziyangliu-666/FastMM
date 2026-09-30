#include "fastmm/venues/coinbase/advanced_md_parser.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/level_spill.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

namespace {

constexpr std::uint32_t kSide = kMaxBookLevelsPerMsg;

}  // namespace

struct AdvancedMdParser::Impl {
  od::parser parser;
  LevelSpill bid_spill;
  LevelSpill ask_spill;
  // One event's book message is built here, then copied behind the frame's earlier messages.
  std::unique_ptr<std::byte[]> event_buf;
  explicit Impl(std::size_t capacity)
      : event_buf(std::make_unique<std::byte[]>(kDecoderScratchBytes)) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

AdvancedMdParser::AdvancedMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
AdvancedMdParser::~AdvancedMdParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

AdvancedMdResult malformed(AdvancedMdStats& stats, AdvancedMdResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

[[nodiscard]] Timestamp time_of(std::string_view t) noexcept {
  const std::int64_t ns = parse_time_ns(t);
  return Timestamp{ns > 0 ? ns : 0};
}

// One side of a book message: the first kSide levels in the message itself, more in the spill.
struct SideAcc {
  Level* region = nullptr;
  LevelSpill* spill = nullptr;
  std::uint32_t n = 0;
  bool spilled = false;
  bool overflow = false;
  bool cut = false;       // snapshot: levels past kSide were left out
  bool unsorted = false;  // snapshot: a level not behind the previous one
  bool bids = false;

  void add_update(Level l) noexcept {
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
  void add_snapshot(Level l) noexcept {
    if (n == kSide) {
      cut = true;
      return;
    }
    if (n > 0 &&
        (bids ? l.price.raw >= region[n - 1].price.raw : l.price.raw <= region[n - 1].price.raw))
      unsorted = true;
    region[n++] = l;
  }
  bool finish_update() noexcept {
    if (!spilled) return false;
    n = spill->keep_nearest(n, bids, region, kSide);
    return true;
  }
};

enum class EventResult : std::uint8_t { Ok, Malformed, Unknown, Unsorted, Overflow };

}  // namespace

AdvancedMdResult AdvancedMdParser::decode(std::string_view json,
                                          Timestamp recv_ts,
                                          Cycles t0,
                                          std::span<std::byte> out) noexcept {
  ++stats_.frames;
  AdvancedMdResult r;
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

  std::string_view channel;
  std::uint32_t written = 0;
  bool bad = false;
  bool unsorted = false;
  bool full = false;
  bool unknown = false;
  bool snapshot_trades = false;

  // One l2_data event into the event buffer, then behind the earlier messages in `out`.
  auto l2_event = [&](od::object ev) -> EventResult {
    std::string_view type;
    std::string_view product;
    auto* m = reinterpret_cast<BookDeltaMsg*>(impl_->event_buf.get());
    Level* levels = m->levels();
    SideAcc bids{levels, &impl_->bid_spill};
    bids.bids = true;
    SideAcc asks{levels + kSide, &impl_->ask_spill};
    Timestamp last_event{};
    bool have_updates = false;
    for (auto field : ev) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return EventResult::Malformed;
      if (key == "type") {
        if (field.value().get_string().get(type) != sj::SUCCESS) return EventResult::Malformed;
      } else if (key == "product_id") {
        if (field.value().get_string().get(product) != sj::SUCCESS) return EventResult::Malformed;
      } else if (key == "updates") {
        const bool snap = type == "snapshot";
        if (!snap && type != "update") return EventResult::Malformed;  // type comes first
        have_updates = true;
        od::array arr;
        if (field.value().get_array().get(arr) != sj::SUCCESS) return EventResult::Malformed;
        for (auto u_res : arr) {
          od::object u;
          if (u_res.get_object().get(u) != sj::SUCCESS) return EventResult::Malformed;
          std::string_view side;
          std::string_view px;
          std::string_view qty;
          std::string_view t;
          for (auto uf : u) {
            std::string_view k;
            if (uf.unescaped_key().get(k) != sj::SUCCESS) return EventResult::Malformed;
            std::string_view* dst = k == "side"           ? &side
                                    : k == "price_level"  ? &px
                                    : k == "new_quantity" ? &qty
                                    : k == "event_time"   ? &t
                                                          : nullptr;
            if (dst != nullptr && uf.value().get_string().get(*dst) != sj::SUCCESS)
              return EventResult::Malformed;
          }
          const bool is_bid = side == "bid";
          if (!is_bid && side != "offer") return EventResult::Malformed;
          SideAcc& acc = is_bid ? bids : asks;
          if (snap && acc.cut) continue;  // deeper than a message carries: not parsed
          const auto p = parse_price(px);
          const auto q = parse_qty(qty);
          if (!p || !q) return EventResult::Malformed;
          if (snap) {
            acc.add_snapshot(Level{*p, *q});
          } else {
            acc.add_update(Level{*p, *q});
            if (!t.empty()) last_event = time_of(t);
          }
        }
      }
    }
    if (!have_updates || product.empty()) return EventResult::Malformed;
    const InstrumentId inst = symbols_.find(venue_, product);
    if (!inst.valid()) return EventResult::Unknown;
    r.instrument = inst;
    const bool snap = type == "snapshot";
    if (bids.unsorted || asks.unsorted) return EventResult::Unsorted;
    if (bids.overflow || asks.overflow) return EventResult::Overflow;
    bool cut_bids = false;
    bool cut_asks = false;
    if (snap) {
      if (bids.cut || asks.cut) ++stats_.snapshot_cut;
    } else {
      cut_bids = bids.finish_update();
      cut_asks = asks.finish_update();
      if (cut_bids || cut_asks) ++stats_.truncated;
    }
    const std::uint32_t nb = bids.n;
    const std::uint32_t na = asks.n;
    std::copy_n(asks.region, na, levels + nb);  // close the gap between the sides
    const std::uint32_t len = BookDeltaMsg::size_for(nb, na);
    if (written + len > out.size()) return EventResult::Overflow;
    init_header(*m, snap ? EventType::BookSnapshot : EventType::BookDelta, inst, venue_, len);
    if (snap) {
      m->hdr.flags |= EventHeader::kSnapshot;
    } else {
      m->hdr.flags |= truncation_flags(cut_bids, cut_asks);
    }
    m->bid_count = nb;
    m->ask_count = na;
    m->first_update_id = 0;  // numbered by the feed
    m->last_update_id = 0;
    m->prev_update_id = 0;
    m->hdr.exch_ts = last_event;
    m->hdr.recv_ts = recv_ts;
    m->hdr.t0_cycles = t0;
    std::memcpy(out.data() + written, m, len);
    written += len;
    ++r.count;
    ++(snap ? stats_.book_snapshots : stats_.book_deltas);
    return EventResult::Ok;
  };

  // market_trades: one TradeMsg per trade of an update event.
  auto trades_event = [&](od::object ev) -> EventResult {
    std::string_view type;
    for (auto field : ev) {
      std::string_view key;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return EventResult::Malformed;
      if (key == "type") {
        if (field.value().get_string().get(type) != sj::SUCCESS) return EventResult::Malformed;
        continue;
      }
      if (key != "trades") continue;
      if (type == "snapshot") {
        snapshot_trades = true;
        continue;  // trades before the subscription
      }
      od::array arr;
      if (field.value().get_array().get(arr) != sj::SUCCESS) return EventResult::Malformed;
      for (auto t_res : arr) {
        od::object t;
        if (t_res.get_object().get(t) != sj::SUCCESS) return EventResult::Malformed;
        std::string_view id;
        std::string_view product;
        std::string_view px;
        std::string_view size;
        std::string_view side;
        std::string_view time;
        for (auto tf : t) {
          std::string_view k;
          if (tf.unescaped_key().get(k) != sj::SUCCESS) return EventResult::Malformed;
          std::string_view* dst = k == "trade_id"     ? &id
                                  : k == "product_id" ? &product
                                  : k == "price"      ? &px
                                  : k == "size"       ? &size
                                  : k == "side"       ? &side
                                  : k == "time"       ? &time
                                                      : nullptr;
          if (dst != nullptr && tf.value().get_string().get(*dst) != sj::SUCCESS)
            return EventResult::Malformed;
        }
        const InstrumentId inst = symbols_.find(venue_, product);
        if (!inst.valid()) {
          ++stats_.unknown_symbol;
          continue;
        }
        const auto p = parse_price(px);
        const auto q = parse_qty(size);
        const auto tid = parse_int64(id);
        if (!p || !q || !tid || (side != "BUY" && side != "SELL")) return EventResult::Malformed;
        if (written + sizeof(TradeMsg) > out.size()) return EventResult::Overflow;
        auto* m = reinterpret_cast<TradeMsg*>(out.data() + written);
        init_header(*m, EventType::Trade, inst, venue_);
        m->price = *p;
        m->qty = *q;
        m->trade_id = static_cast<std::uint64_t>(*tid);
        // "The maker's side of the trade": the taker took the other one.
        m->aggressor = side == "SELL" ? Side::Buy : Side::Sell;
        m->hdr.venue_seq = static_cast<std::uint64_t>(*tid);
        m->hdr.exch_ts = time_of(time);
        m->hdr.recv_ts = recv_ts;
        m->hdr.t0_cycles = t0;
        written += sizeof(TradeMsg);
        ++r.count;
        ++stats_.trades;
      }
    }
    return EventResult::Ok;
  };

  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key == "channel") {
      if (field.value().get_string().get(channel) != sj::SUCCESS) return malformed(stats_, r);
    } else if (key == "sequence_num") {
      if (field.value().get_uint64().get(r.sequence) != sj::SUCCESS) return malformed(stats_, r);
      r.has_sequence = true;
    } else if (key == "type") {
      std::string_view type;
      if (field.value().get_string().get(type) != sj::SUCCESS) return malformed(stats_, r);
      if (type == "error") r.control = AdvancedControl::Error;
    } else if (key == "message") {
      if (field.value().get_string().get(r.msg) != sj::SUCCESS) r.msg = {};
    } else if (key == "events") {
      const bool l2 = channel == "l2_data";
      const bool trades = channel == "market_trades";
      if (!l2 && !trades) continue;  // heartbeats, subscriptions: nothing to read
      od::array events;
      if (field.value().get_array().get(events) != sj::SUCCESS) return malformed(stats_, r);
      for (auto ev_res : events) {
        od::object ev;
        if (ev_res.get_object().get(ev) != sj::SUCCESS) return malformed(stats_, r);
        const EventResult er = l2 ? l2_event(ev) : trades_event(ev);
        if (er == EventResult::Malformed) bad = true;
        if (er == EventResult::Unsorted) unsorted = true;
        if (er == EventResult::Overflow) full = true;
        if (er == EventResult::Unknown) unknown = true;
        if (er != EventResult::Ok) break;
      }
    }
  }

  if (r.control == AdvancedControl::Error) {
    ++stats_.control;
    r.status = ParseStatus::Error;
    return r;
  }
  if (bad || unsorted) return malformed(stats_, r);
  if (full) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    r.count = 0;
    return r;
  }
  if (unknown) {
    ++stats_.unknown_symbol;
    r.status = ParseStatus::UnknownSymbol;
    r.count = 0;
    return r;
  }
  if (r.count > 0) {
    r.status = ParseStatus::Ok;
    r.kind = channel == "l2_data" ? MdKind::BookDelta : MdKind::Trade;
    r.len = written;
    return r;
  }
  if (channel == "heartbeats") {
    ++stats_.heartbeats;
    r.control = AdvancedControl::Heartbeat;
  } else if (channel == "subscriptions") {
    ++stats_.control;
    r.control = AdvancedControl::Subscriptions;
  } else if (!snapshot_trades) {
    ++stats_.ignored;
  }
  if (channel.empty() && !r.has_sequence) return malformed(stats_, r);
  r.status = ParseStatus::Ignored;
  return r;
}

}  // namespace fastmm::venues::coinbase
