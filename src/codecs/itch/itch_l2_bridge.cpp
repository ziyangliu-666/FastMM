// ITCH -> L3Book -> L2 events (see itch_l2_bridge.hpp).
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"

#include "fastmm/core/config_macros.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fastmm::codecs::itch {

struct ItchL2Bridge::Slot {
  // What changed in a side's top since the last emission. While no level entered or left it,
  // the published and current tops have the same prices at the same indices, and `words` marks
  // the indices whose quantity changed. Once one did (`ranks_changed`), flush() compares them.
  struct Changes {
    std::uint64_t words[(kMaxDepth + 63) / 64]{};
    bool ranks_changed = false;
  };
  Slot(InstrumentId i, std::uint16_t idx, Price tick, const L3BookConfig& cfg, std::uint32_t depth)
      : id(i), index(idx), book(tick, cfg), pub(new Level[4 * static_cast<std::size_t>(depth)]) {}

  [[nodiscard]] Level* published(Side side, std::uint32_t depth) const noexcept {
    return pub.get() + (side == Side::Buy ? 0 : depth);
  }

  [[nodiscard]] Level* current(Side side, std::uint32_t depth) const noexcept {
    return pub.get() + (side == Side::Buy ? 2 * depth : 3 * depth);
  }

  InstrumentId id;
  std::uint16_t index;  // in slots_
  L3Book book;
  std::unique_ptr<Level[]> pub;  // published bids and asks, then the current ones
  std::uint32_t pub_n[2] = {0, 0};
  std::uint32_t current_n[2] = {0, 0};
  Changes changed[2];
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

// The decoder's direct consumer: it applies each book message to the L3 book and the current top
// as the decoder validates it, with no engine message in between. Only published events get a
// header.
struct ItchL2Bridge::Consumer {
  Consumer(ItchL2Bridge& bridge, Slot* book_slot, std::uint64_t sequence)
      : owner(bridge), slot(book_slot), seq(sequence) {}
  void on_add(
      InstrumentId, Timestamp ts, std::uint64_t ref, Side side, Price price, Qty qty) noexcept {
    Qty aggregate;
    if (FASTMM_UNLIKELY(slot->book.add(ref, side, price, qty, &aggregate) != L3Error::None)) {
      ++owner.stats_.book_errors;
      return;
    }
    owner.touch(*slot, side, price, aggregate, seq, ts);
  }
  void on_execute(InstrumentId,
                  Timestamp ts,
                  std::uint64_t ref,
                  Qty qty,
                  Price exec_price,
                  std::uint64_t match,
                  std::uint8_t flags) noexcept {
    L3Book& book = slot->book;
    const auto h = book.find(ref);
    if (FASTMM_UNLIKELY(!h.valid())) {
      ++owner.stats_.book_errors;
      return;
    }
    const Side side = book.at(h).side;
    const Price price = book.at(h).price;
    Qty aggregate;
    book.execute(h, qty, &aggregate);
    owner.touch(*slot, side, price, aggregate, seq, ts);
    if (slot->complete && (flags & OrderExecL3Msg::kNonPrintable) == 0)
      on_trade(slot->id, ts, exec_price.is_zero() ? price : exec_price, qty, match, opposite(side));
  }
  void on_cancel(InstrumentId, Timestamp ts, std::uint64_t ref, Qty qty) noexcept {
    L3Book& book = slot->book;
    const auto h = book.find(ref);
    if (FASTMM_UNLIKELY(!h.valid())) {
      ++owner.stats_.book_errors;
      return;
    }
    const Side side = book.at(h).side;
    const Price price = book.at(h).price;
    Qty aggregate;
    book.cancel(h, qty, &aggregate);
    owner.touch(*slot, side, price, aggregate, seq, ts);
  }
  void on_replace(InstrumentId,
                  Timestamp ts,
                  std::uint64_t old_ref,
                  std::uint64_t new_ref,
                  Price price,
                  Qty qty) noexcept {
    L3Book& book = slot->book;
    const auto h = book.find(old_ref);
    if (FASTMM_UNLIKELY(!h.valid())) {
      ++owner.stats_.book_errors;
      return;
    }
    const Side side = book.at(h).side;
    const Price before = book.at(h).price;
    Qty aggregate;
    book.cancel(h, Qty{}, &aggregate);
    owner.touch(*slot, side, before, aggregate, seq, ts);
    if (FASTMM_UNLIKELY(book.add(new_ref, side, price, qty, &aggregate) != L3Error::None)) {
      ++owner.stats_.book_errors;
      return;
    }
    owner.touch(*slot, side, price, aggregate, seq, ts);
  }
  void on_trade(
      InstrumentId, Timestamp ts, Price price, Qty qty, std::uint64_t match, Side side) noexcept {
    if (!slot->complete) return;
    owner.emit_trade(*slot, seq, ts, price, qty, match, side);
  }
  ItchL2Bridge& owner;
  Slot* slot;
  std::uint64_t seq;
};

ItchL2Bridge::ItchL2Bridge(venues::EventSink& sink, const ItchL2BridgeConfig& cfg)
    : sink_(sink),
      venue_(cfg.venue),
      depth_(cfg.depth),
      tick_(cfg.tick),
      book_cfg_(cfg.book),
      decoder_(cfg.venue),
      locate_slot_(new std::uint16_t[ItchDecoder::kLocateSlots]),
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

ItchL2BridgeBookCounters ItchL2Bridge::book_counters() const noexcept {
  ItchL2BridgeBookCounters c;
  for (const auto& s : slots_) {
    c.recentres += s->book.recentre_count();
    c.recentre_failures += s->book.recentre_failures();
    c.recentre_levels_moved += s->book.recentre_levels_moved();
  }
  return c;
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
    // Executed, Executed With Price, Cancel, Delete and Replace name an existing order at
    // offset 11: start its index lookup now, it overlaps with the decode below.
    if ((type == 'E' || type == 'C' || type == 'X' || type == 'D' || type == 'U') &&
        msg.size() >= 19)
      slots_[slot_idx]->book.prefetch(nasdaq::load_be64(msg.data() + 11));
  }
  stamp_ = stamp;
  decoder_.set_venue_seq(seq);
  const std::uint64_t mapped_before = decoder_.stats().directory_mapped;
  Consumer consumer(*this, slot_idx == kNoSlot ? nullptr : slots_[slot_idx].get(), seq);
  const venues::ParseStatus st = decoder_.decode_into(frame, stamp.recv_ts.ns, consumer);
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
  return st;
}

void ItchL2Bridge::touch(
    Slot& s, Side side, Price price, Qty level_qty, std::uint64_t seq, Timestamp exch_ts) noexcept {
  if (!s.pending) {
    s.pending = true;
    s.first_seq = seq;
  }
  s.last_seq = seq;
  s.last_exch_ts = exch_ts;
  if (!s.complete) return;
  const auto si = static_cast<std::size_t>(side);
  auto& changed = s.changed[si];
  std::uint32_t& n = s.current_n[si];
  Level* now = s.current(side, depth_);
  if (n == depth_ && better(side, now[n - 1].price, price)) return;
  std::uint32_t lo = 0;
  std::uint32_t hi = n;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (better(side, now[mid].price, price))
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < n && now[lo].price == price) {
    if (now[lo].qty == level_qty) return;
    if (level_qty.is_positive()) {
      now[lo].qty = level_qty;
      if (!changed.ranks_changed) changed.words[lo / 64] |= std::uint64_t{1} << (lo % 64);
    } else {
      const bool full = n == depth_;
      --n;
      std::memmove(now + lo, now + lo + 1, sizeof(Level) * (n - lo));
      if (full) {
        const Level next = n != 0 ? s.book.next_level(side, now[n - 1].price)
                                  : (side == Side::Buy ? s.book.best_bid() : s.book.best_ask());
        if (next.qty.is_positive()) now[n++] = next;
      }
      changed.ranks_changed = true;
    }
  } else {
    if (level_qty.is_zero() || lo == depth_) return;
    const std::uint32_t nn = std::min(n + 1, depth_);
    std::memmove(now + lo + 1, now + lo, sizeof(Level) * (nn - lo - 1));
    now[lo] = Level{price, level_qty};
    n = nn;
    changed.ranks_changed = true;
  }
  if (s.dirty) return;
  s.dirty = true;
  dirty_.push_back(s.index);
}

void ItchL2Bridge::emit_trade(const Slot& s,
                              std::uint64_t seq,
                              Timestamp exch_ts,
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
  t->hdr.venue_seq = seq;
  t->hdr.exch_ts = exch_ts;
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
  Level* now_b = s.current(Side::Buy, depth_);
  Level* now_a = s.current(Side::Sell, depth_);
  // The current top is maintained by mutations. Quantities keep their published indices
  // until a rank changes, so that common case emits only the marked slots, already in order.
  auto collect = [&](Side side, const Level* now, Level* out) {
    const auto si = static_cast<std::size_t>(side);
    const auto& changed = s.changed[si];
    const Level* pub = s.published(side, depth_);
    if (changed.ranks_changed)
      return diff_levels(side, pub, s.pub_n[si], now, s.current_n[si], out);
    std::uint32_t count = 0;
    for (std::size_t word = 0; word < (depth_ + 63) / 64; ++word) {
      std::uint64_t bits = changed.words[word];
      while (bits != 0) {
        const auto i = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
        if (now[i].qty != pub[i].qty) out[count++] = now[i];
        bits &= bits - 1;
      }
    }
    return count;
  };
  const std::uint32_t nb = s.current_n[0];
  const std::uint32_t na = s.current_n[1];
  Level* chg = changes_.get();
  const std::uint32_t cb = collect(Side::Buy, now_b, chg);
  const std::uint32_t ca = collect(Side::Sell, now_a, chg + cb);
  if (cb + ca == 0) {
    s.changed[0] = {};
    s.changed[1] = {};
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
  // A failed reservation leaves both the publication baseline and the journal intact.
  auto publish = [&](Side side, const Level* now, std::uint32_t n) {
    const auto si = static_cast<std::size_t>(side);
    auto& changed = s.changed[si];
    Level* pub = s.published(side, depth_);
    if (changed.ranks_changed) {
      std::memcpy(pub, now, sizeof(Level) * n);
    } else {
      for (std::size_t word = 0; word < (depth_ + 63) / 64; ++word) {
        std::uint64_t bits = changed.words[word];
        while (bits != 0) {
          const auto i = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
          pub[i] = now[i];
          bits &= bits - 1;
        }
      }
    }
    changed = {};
  };
  publish(Side::Buy, now_b, nb);
  publish(Side::Sell, now_a, na);
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
  s.current_n[0] = nb;
  s.current_n[1] = na;
  std::memcpy(s.current(Side::Buy, depth_), pb, sizeof(Level) * nb);
  std::memcpy(s.current(Side::Sell, depth_), pa, sizeof(Level) * na);
  s.prev_u = s.last_seq;
  s.pending = false;
  s.dirty = false;
  s.changed[0] = {};
  s.changed[1] = {};
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
    s->changed[0] = {};
    s->changed[1] = {};
    s->pending = false;
    s->pub_n[0] = s->pub_n[1] = 0;
    s->current_n[0] = s->current_n[1] = 0;
    s->prev_u = 0;
  }
  dirty_.clear();
  incomplete_ = slots_.size();
  emit_state(ConnState::Resyncing, reason_code);
}

}  // namespace fastmm::codecs::itch
