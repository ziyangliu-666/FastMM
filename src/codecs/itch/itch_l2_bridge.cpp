// ITCH -> L3Book -> L2 events (see itch_l2_bridge.hpp).
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"

#include "fastmm/core/config_macros.hpp"

#include <algorithm>
#include <cstring>

namespace fastmm::codecs::itch {

struct ItchL2Bridge::Slot {
  Slot(InstrumentId i, std::uint16_t idx, Price tick, const L3BookConfig& cfg, std::uint32_t depth)
      : id(i), index(idx), book(tick, cfg), pub(new Level[2 * static_cast<std::size_t>(depth)]) {}

  [[nodiscard]] Level* published(Side side, std::uint32_t depth) const noexcept {
    return pub.get() + (side == Side::Buy ? 0 : depth);
  }

  InstrumentId id;
  std::uint16_t index;  // in slots_
  L3Book book;
  std::unique_ptr<Level[]> pub;  // top last emitted: bids [0, depth), asks [depth, 2 x depth)
  std::uint32_t pub_n[2] = {0, 0};
  std::uint64_t first_seq = 0;  // first message applied since the last emission
  std::uint64_t last_seq = 0;   // last message applied
  std::uint64_t prev_u = 0;     // last_update_id of the last emission
  Timestamp last_exch_ts{};
  bool pending = false;  // messages applied since the last emission
  bool complete = false;
  bool dirty = false;  // in dirty_: the top may have changed
};

namespace {

[[nodiscard]] std::uint32_t cycles_since(Cycles t0) noexcept {
  if (t0.v == 0) return 0;
  return static_cast<std::uint32_t>(rdtscp() - t0);
}

// Levels of `now` that differ from `before` (both best-first) as absolute quantities, and the
// levels of `before` missing from `now` with quantity 0. Returns the count written to `out`.
std::uint32_t diff_levels(Side side,
                          const Level* before,
                          std::uint32_t nb,
                          const Level* now,
                          std::uint32_t nn,
                          Level* out) noexcept {
  std::uint32_t i = 0;
  std::uint32_t j = 0;
  std::uint32_t n = 0;
  while (i < nb || j < nn) {
    if (j == nn || (i < nb && better(side, before[i].price, now[j].price))) {
      out[n++] = Level{before[i++].price, Qty{}};
    } else if (i == nb || better(side, now[j].price, before[i].price)) {
      out[n++] = now[j++];
    } else {
      if (before[i].qty != now[j].qty) out[n++] = now[j];
      ++i;
      ++j;
    }
  }
  return n;
}

}  // namespace

ItchL2Bridge::ItchL2Bridge(venues::EventSink& sink, const ItchL2BridgeConfig& cfg)
    : sink_(sink),
      venue_(cfg.venue),
      depth_(cfg.depth),
      tick_(cfg.tick),
      book_cfg_(cfg.book),
      decoder_(cfg.venue),
      locate_slot_(new std::uint16_t[ItchDecoder::kLocateSlots]),
      top_(new Level[2 * static_cast<std::size_t>(cfg.depth)]),
      changes_(new Level[4 * static_cast<std::size_t>(cfg.depth)]) {
  FASTMM_CHECK(cfg.depth >= 1 && cfg.depth <= kMaxDepth);
  std::fill_n(locate_slot_.get(), ItchDecoder::kLocateSlots, kNoSlot);
}

ItchL2Bridge::~ItchL2Bridge() = default;

bool ItchL2Bridge::add_instrument(std::string_view symbol, InstrumentId id) {
  if (!id.valid() || slot_of(id) != nullptr || slots_.size() >= kNoSlot) return false;
  if (!symbol.empty() && !decoder_.add_symbol(symbol, id)) return false;
  slots_.push_back(std::make_unique<Slot>(
      id, static_cast<std::uint16_t>(slots_.size()), tick_, book_cfg_, depth_));
  dirty_.reserve(slots_.size());
  ++incomplete_;
  return true;
}

bool ItchL2Bridge::map_locate(std::uint16_t locate, InstrumentId id) noexcept {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i]->id != id) continue;
    decoder_.map_locate(locate, id);
    locate_slot_[locate] = static_cast<std::uint16_t>(i);
    return true;
  }
  return false;
}

void ItchL2Bridge::clear_locates() noexcept {
  decoder_.clear_locates();
  std::fill_n(locate_slot_.get(), ItchDecoder::kLocateSlots, kNoSlot);
}

ItchL2Bridge::Slot* ItchL2Bridge::slot_of(InstrumentId id) const noexcept {
  for (const auto& s : slots_) {
    if (s->id == id) return s.get();
  }
  return nullptr;
}

bool ItchL2Bridge::complete(InstrumentId id) const noexcept {
  const Slot* s = slot_of(id);
  return s != nullptr && s->complete;
}

const L3Book* ItchL2Bridge::book(InstrumentId id) const noexcept {
  const Slot* s = slot_of(id);
  return s == nullptr ? nullptr : &s->book;
}

void ItchL2Bridge::stamp_header(EventHeader& h) const noexcept {
  h.recv_ts = stamp_.recv_ts;
  h.t0_cycles = stamp_.t0_cycles;
  h.t1_delta = cycles_since(stamp_.t0_cycles);
}

venues::ParseStatus ItchL2Bridge::on_itch_message(std::uint64_t seq,
                                                  std::span<const std::byte> msg,
                                                  const DatagramStamp& stamp) noexcept {
  ++stats_.messages;
  const FrameView frame{msg, msg.size(), 0};
  const char type = msg.empty() ? '\0' : static_cast<char>(msg[0]);
  std::uint16_t slot_idx = kNoSlot;
  if (type != 'R' && type != 'S' && msg.size() >= 3) {
    const auto locate = static_cast<std::uint16_t>((static_cast<unsigned>(msg[1]) << 8U) |
                                                   static_cast<unsigned>(msg[2]));
    slot_idx = locate_slot_[locate];
    if (slot_idx == kNoSlot) {
      ++stats_.skipped;
      return venues::ParseStatus::Ignored;
    }
  }
  stamp_ = stamp;
  scratch_.reset();
  decoder_.set_venue_seq(seq);
  const std::uint64_t mapped_before = decoder_.stats().directory_mapped;
  const venues::ParseStatus st = decoder_.decode_into(frame, stamp.recv_ts.ns, scratch_);
  if (FASTMM_UNLIKELY(type == 'R' || type == 'S')) {
    if (st != venues::ParseStatus::Ignored || msg.size() < sizeof(SystemEvent)) return st;
    if (type == 'S') {
      system_event_ = view_as<SystemEvent>(msg.data()).event_code;
      return st;
    }
    const std::uint16_t locate = view_as<StockDirectory>(msg.data()).hdr.stock_locate.get();
    locate_slot_[locate] = kNoSlot;
    if (decoder_.stats().directory_mapped != mapped_before) {
      const InstrumentId id = decoder_.instrument(locate);
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i]->id == id) locate_slot_[locate] = static_cast<std::uint16_t>(i);
      }
    }
    return st;
  }
  EventHeader* ev = scratch_.event();
  if (ev == nullptr) return st;
  Slot& s = *slots_[slot_idx];
  L3Book& book = s.book;
  switch (ev->type) {
    case EventType::OrderAddL3: {
      const auto& m = msg_cast<OrderAddL3Msg>(ev);
      if (FASTMM_UNLIKELY(book.add(m.order_ref, m.side, m.price, m.qty) != L3Error::None)) {
        ++stats_.book_errors;
        break;
      }
      touch(s, m.side, m.price, seq, ev->exch_ts);
      break;
    }
    case EventType::OrderExecL3: {
      const auto& m = msg_cast<OrderExecL3Msg>(ev);
      const L3Book::OrderHandle h = book.find(m.order_ref);
      if (FASTMM_UNLIKELY(!h.valid())) {
        ++stats_.book_errors;
        break;
      }
      const Side side = book.at(h).side;
      const Price price = book.at(h).price;
      book.execute(h, m.exec_qty);
      touch(s, side, price, seq, ev->exch_ts);
      if (s.complete && m.printable()) {
        emit_trade(s,
                   *ev,
                   m.exec_price.is_zero() ? price : m.exec_price,
                   m.exec_qty,
                   m.match_id,
                   opposite(side));
      }
      break;
    }
    case EventType::OrderCancelL3: {
      const auto& m = msg_cast<OrderCancelL3Msg>(ev);
      const L3Book::OrderHandle h = book.find(m.order_ref);
      if (FASTMM_UNLIKELY(!h.valid())) {
        ++stats_.book_errors;
        break;
      }
      const Side side = book.at(h).side;
      const Price price = book.at(h).price;
      book.cancel(h, m.canceled_qty);
      touch(s, side, price, seq, ev->exch_ts);
      break;
    }
    case EventType::OrderReplaceL3: {
      const auto& m = msg_cast<OrderReplaceL3Msg>(ev);
      const L3Book::OrderHandle h = book.find(m.old_order_ref);
      if (FASTMM_UNLIKELY(!h.valid())) {
        ++stats_.book_errors;
        break;
      }
      const Side side = book.at(h).side;
      const Price price = book.at(h).price;
      book.cancel(h);
      touch(s, side, price, seq, ev->exch_ts);
      if (FASTMM_UNLIKELY(book.add(m.new_order_ref, side, m.price, m.qty) != L3Error::None)) {
        ++stats_.book_errors;
        break;
      }
      touch(s, side, m.price, seq, ev->exch_ts);
      break;
    }
    case EventType::Trade: {
      if (!s.complete) break;
      stamp_header(*ev);
      if (FASTMM_UNLIKELY(!sink_.push(*ev))) {
        ++stats_.overflow;
        break;
      }
      ++stats_.trades;
      break;
    }
    default:
      break;
  }
  return st;
}

void ItchL2Bridge::touch(
    Slot& s, Side side, Price price, std::uint64_t seq, Timestamp exch_ts) noexcept {
  if (!s.pending) {
    s.pending = true;
    s.first_seq = seq;
  }
  s.last_seq = seq;
  s.last_exch_ts = exch_ts;
  if (!s.complete || s.dirty) return;
  const auto si = static_cast<std::size_t>(side);
  const std::uint32_t n = s.pub_n[si];
  if (n == depth_ && better(side, s.published(side, depth_)[n - 1].price, price)) return;
  s.dirty = true;
  dirty_.push_back(s.index);
}

void ItchL2Bridge::emit_trade(const Slot& s,
                              const EventHeader& src,
                              Price price,
                              Qty qty,
                              std::uint64_t match,
                              Side aggressor) noexcept {
  auto* t = sink_.reserve<TradeMsg>();
  if (FASTMM_UNLIKELY(t == nullptr)) {
    ++stats_.overflow;
    return;
  }
  *t = TradeMsg{};
  init_header(*t, EventType::Trade, s.id, venue_);
  t->hdr.venue_seq = src.venue_seq;
  t->hdr.exch_ts = src.exch_ts;
  t->price = price;
  t->qty = qty;
  t->trade_id = match;
  t->aggressor = aggressor;
  stamp_header(t->hdr);
  sink_.commit();
  ++stats_.trades;
}

void ItchL2Bridge::end_datagram() noexcept {
  std::size_t keep = 0;
  for (const std::uint16_t idx : dirty_) {
    Slot& s = *slots_[idx];
    if (!s.dirty) continue;
    if (!flush(s)) dirty_[keep++] = idx;
  }
  dirty_.resize(keep);
}

bool ItchL2Bridge::flush(Slot& s) noexcept {
  Level* now_b = top_.get();
  Level* now_a = top_.get() + depth_;
  const auto nb = static_cast<std::uint32_t>(s.book.top_levels(Side::Buy, {now_b, depth_}));
  const auto na = static_cast<std::uint32_t>(s.book.top_levels(Side::Sell, {now_a, depth_}));
  Level* chg = changes_.get();
  const std::uint32_t cb =
      diff_levels(Side::Buy, s.published(Side::Buy, depth_), s.pub_n[0], now_b, nb, chg);
  const std::uint32_t ca =
      diff_levels(Side::Sell, s.published(Side::Sell, depth_), s.pub_n[1], now_a, na, chg + cb);
  if (cb + ca == 0) {
    s.dirty = false;
    return true;
  }
  const std::uint32_t len = BookDeltaMsg::size_for(cb, ca);
  std::byte* p = sink_.reserve_bytes(len);
  if (FASTMM_UNLIKELY(p == nullptr)) {
    ++stats_.overflow;
    return false;
  }
  auto* d = reinterpret_cast<BookDeltaMsg*>(p);
  init_header(*d, EventType::BookDelta, s.id, venue_, len);
  d->bid_count = cb;
  d->ask_count = ca;
  d->first_update_id = s.first_seq;
  d->last_update_id = s.last_seq;
  d->prev_update_id = s.prev_u;
  d->hdr.venue_seq = s.last_seq;
  d->hdr.exch_ts = s.last_exch_ts;
  const std::size_t level_bytes = sizeof(Level) * (cb + ca);
  std::memcpy(d->levels(), chg, level_bytes);
  std::memset(p + sizeof(BookDeltaMsg) + level_bytes, 0, len - sizeof(BookDeltaMsg) - level_bytes);
  stamp_header(d->hdr);
  sink_.commit();
  ++stats_.deltas;
  stats_.delta_levels += cb + ca;
  std::memcpy(s.published(Side::Buy, depth_), now_b, sizeof(Level) * nb);
  std::memcpy(s.published(Side::Sell, depth_), now_a, sizeof(Level) * na);
  s.pub_n[0] = nb;
  s.pub_n[1] = na;
  s.prev_u = s.last_seq;
  s.pending = false;
  s.dirty = false;
  return true;
}

bool ItchL2Bridge::emit_snapshot(Slot& s) noexcept {
  Level* pb = s.published(Side::Buy, depth_);
  Level* pa = s.published(Side::Sell, depth_);
  const auto nb = static_cast<std::uint32_t>(s.book.top_levels(Side::Buy, {pb, depth_}));
  const auto na = static_cast<std::uint32_t>(s.book.top_levels(Side::Sell, {pa, depth_}));
  const std::uint32_t len = BookDeltaMsg::size_for(nb, na);
  std::byte* p = sink_.reserve_bytes(len);
  if (FASTMM_UNLIKELY(p == nullptr)) {
    ++stats_.overflow;
    s.pub_n[0] = s.pub_n[1] = 0;
    return false;
  }
  auto* d = reinterpret_cast<BookDeltaMsg*>(p);
  init_header(*d, EventType::BookSnapshot, s.id, venue_, len);
  d->hdr.flags |= EventHeader::kSnapshot;
  d->bid_count = nb;
  d->ask_count = na;
  d->first_update_id = s.last_seq;
  d->last_update_id = s.last_seq;
  d->prev_update_id = 0;
  d->hdr.venue_seq = s.last_seq;
  d->hdr.exch_ts = s.last_exch_ts;
  std::memcpy(d->levels(), pb, sizeof(Level) * nb);
  std::memcpy(d->levels() + nb, pa, sizeof(Level) * na);
  const std::size_t used = sizeof(BookDeltaMsg) + sizeof(Level) * (nb + na);
  std::memset(p + used, 0, len - used);
  stamp_header(d->hdr);
  sink_.commit();
  ++stats_.snapshots;
  s.pub_n[0] = nb;
  s.pub_n[1] = na;
  s.prev_u = s.last_seq;
  s.pending = false;
  s.dirty = false;
  return true;
}

void ItchL2Bridge::emit_state(ConnState state, std::int32_t reason) noexcept {
  auto* m = sink_.reserve<ConnectionStateMsg>();
  if (FASTMM_UNLIKELY(m == nullptr)) {
    ++stats_.overflow;
    return;
  }
  *m = ConnectionStateMsg{};
  init_header(*m, EventType::ConnectionState, InstrumentId{}, venue_);
  m->state = state;
  m->channel = 0;
  m->reason_code = reason;
  stamp_header(m->hdr);
  sink_.commit();
}

bool ItchL2Bridge::mark_complete(InstrumentId id) noexcept {
  Slot* s = slot_of(id);
  if (s == nullptr) return false;
  if (s->complete) return true;
  if (!emit_snapshot(*s)) return false;
  s->complete = true;
  if (--incomplete_ == 0) emit_state(ConnState::Live, 0);
  return true;
}

bool ItchL2Bridge::mark_all_complete() noexcept {
  bool ok = true;
  for (const auto& s : slots_) {
    if (!s->complete && !mark_complete(s->id)) ok = false;
  }
  return ok;
}

void ItchL2Bridge::mark_incomplete(std::int32_t reason_code) noexcept {
  for (const auto& s : slots_) {
    s->book.clear();
    s->complete = false;
    s->dirty = false;
    s->pending = false;
    s->pub_n[0] = s->pub_n[1] = 0;
    s->prev_u = 0;
  }
  dirty_.clear();
  incomplete_ = slots_.size();
  emit_state(ConnState::Resyncing, reason_code);
}

}  // namespace fastmm::codecs::itch
