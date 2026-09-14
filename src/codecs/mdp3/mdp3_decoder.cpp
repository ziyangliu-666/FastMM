#include "fastmm/codecs/mdp3/mdp3_decoder.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fastmm::codecs::mdp3 {

using venues::EventSink;
using venues::ParseStatus;

namespace {

constexpr std::size_t kBid = 0;
constexpr std::size_t kAsk = 1;
// Per-side level capacity of one BookDeltaMsg batch; flushed early before it can overflow.
constexpr std::size_t kBatchLevels = 256;
// Most levels one entry can add to a side's batch (DeleteThru / DeleteFrom clear a side; New
// can push the level that falls off plus the new one).
constexpr std::size_t kMaxLevelsPerEntry = std::size_t{kMaxMbpDepth} + 1;

// Strictly better price for a side: higher bid, lower offer.
[[nodiscard]] constexpr bool better(std::size_t side, Price a, Price b) noexcept {
  return side == kBid ? a > b : a < b;
}

// True when `px` keeps the side strictly ordered with `prev` = level k-1 and `next` = the level
// that will follow it.
[[nodiscard]] bool ordered(
    const MbpSide& s, std::size_t side, std::size_t k, std::size_t next, Price px) noexcept {
  if (k > 0 && !better(side, s.levels[k - 1].price, px)) return false;
  if (next < s.count && !better(side, px, s.levels[next].price)) return false;
  return true;
}

void erase_levels(MbpSide& s, std::size_t from, std::size_t n) noexcept {
  for (std::size_t j = from; j + n < s.count; ++j) s.levels[j] = s.levels[j + n];
  s.count = static_cast<std::uint8_t>(s.count - n);
}

void fill_book_msg(BookDeltaMsg& m,
                   std::uint32_t len,
                   const Level* bids,
                   std::uint32_t nb,
                   const Level* asks,
                   std::uint32_t na) noexcept {
  m.bid_count = nb;
  m.ask_count = na;
  if (nb != 0) std::memcpy(m.levels(), bids, nb * sizeof(Level));
  if (na != 0) std::memcpy(m.levels() + nb, asks, na * sizeof(Level));
  const std::size_t used = sizeof(BookDeltaMsg) + (std::size_t{nb} + na) * sizeof(Level);
  std::memset(reinterpret_cast<std::byte*>(&m) + used, 0, len - used);
}

// Visits (SecurityID, RptSeq) of every entry of a template whose NoMDEntries group carries both.
template <class Msg, class F>
bool walk_rpt(const MessageView& m, F&& step) noexcept {
  const Msg msg(m.body, m.header.block_length, m.header.version);
  if (!msg.valid()) return true;  // decode_packet skips malformed messages as well
  for (const auto e : msg.no_md_entries()) {
    if (!step(e.security_id(), e.rpt_seq())) return false;
  }
  return true;
}

}  // namespace

struct Mdp3Decoder::Batch {
  std::array<std::array<Level, kBatchLevels>, 2> levels{};
  std::array<std::size_t, 2> count{};
  std::int32_t index = -1;
  std::uint32_t first_rpt = 0;  // 0 = no entry yet (RptSeq starts at 1)
  std::uint32_t last_rpt = 0;
  std::uint64_t transact_time = 0;

  void push(std::size_t side, Price px, Qty qty) noexcept {
    levels[side][count[side]++] = Level{px, qty};
  }
  [[nodiscard]] bool room(std::size_t side, std::size_t n) const noexcept {
    return count[side] + n <= kBatchLevels;
  }
};

Mdp3Decoder::Mdp3Decoder(const Mdp3DecoderConfig& cfg)
    : cfg_(cfg),
      instruments_(std::make_unique<Mdp3InstrumentTable>()),
      books_(std::make_unique<MbpBookState[]>(kMaxMdp3Instruments)),
      batch_(std::make_unique<Batch>()) {}

Mdp3Decoder::~Mdp3Decoder() = default;

// ---- registration / recovery state -------------------------------------------------------------

void Mdp3Decoder::init_book(std::size_t index, std::uint8_t depth) noexcept {
  instruments_->at(index).market_depth = std::clamp<std::uint8_t>(depth, 1, kMaxMbpDepth);
  books_[index] = MbpBookState{};
  if (new_recovering_) mark_recovering(index);
}

std::int32_t Mdp3Decoder::add_instrument(std::int32_t security_id, std::uint8_t depth) noexcept {
  bool inserted = false;
  if (instruments_->upsert(security_id, inserted) == nullptr) {
    ++stats_.table_full;
    return -1;
  }
  const std::int32_t index = instruments_->index_of(security_id);
  const auto i = static_cast<std::size_t>(index);
  if (inserted) {
    init_book(i, depth);
  } else {
    instruments_->at(i).market_depth = std::clamp<std::uint8_t>(depth, 1, kMaxMbpDepth);
  }
  return index;
}

void Mdp3Decoder::mark_recovering(std::size_t index) noexcept {
  MbpBookState& b = books_[index];
  if (!b.recovering) {
    b.recovering = true;
    ++recovering_;
  }
}

void Mdp3Decoder::mark_all_recovering() noexcept {
  for (std::size_t i = 0; i < instruments_->size(); ++i) mark_recovering(i);
}

Mdp3Decoder::RptCheck Mdp3Decoder::check_rpt(std::size_t index, std::uint32_t rpt) noexcept {
  MbpBookState& b = books_[index];
  if (b.recovering) return RptCheck::Skip;
  if (b.rpt_unknown) {
    b.rpt_unknown = false;
    b.rpt_seq = rpt;
    return RptCheck::Apply;
  }
  if (rpt <= b.rpt_seq) {
    ++stats_.stale_entries;
    return RptCheck::Stale;
  }
  if (rpt != b.rpt_seq + 1) {
    ++stats_.rpt_gaps;
    events_.rpt_gap = true;
    mark_recovering(index);
    return RptCheck::Skip;
  }
  b.rpt_seq = rpt;
  return RptCheck::Apply;
}

void Mdp3Decoder::book_error(std::size_t index) noexcept {
  ++stats_.book_errors;
  events_.book_error = true;
  mark_recovering(index);
}

// ---- packet ------------------------------------------------------------------------------------

ParseStatus Mdp3Decoder::decode_packet(std::span<const std::byte> datagram,
                                       std::int64_t rx_ts,
                                       EventSink& sink,
                                       std::int32_t only) noexcept {
  ++stats_.packets;
  PacketCursor cur(datagram);
  if (!cur.valid()) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const std::uint64_t malformed_before = stats_.malformed;
  const std::uint64_t overflow_before = stats_.sink_overflows;
  bool decoded = false;
  MessageView m;
  while (cur.next(m)) {
    ++stats_.messages;
    if (m.header.schema_id != kMdpSchemaId) {
      ++stats_.other_schema;
      continue;
    }
    if (m.body.size() < m.header.block_length) {
      ++stats_.malformed;
      continue;
    }
    switch (m.header.template_id) {
      case schema::MDIncrementalRefreshBook46::kTemplateId:
        decode_book(m, rx_ts, sink, only);
        break;
      case schema::MDIncrementalRefreshTradeSummary48::kTemplateId:
        decode_trades(m, rx_ts, sink, only);
        break;
      case schema::MDIncrementalRefreshVolume37::kTemplateId:
        account_rpt<schema::MDIncrementalRefreshVolume37>(m, only);
        break;
      case schema::MDIncrementalRefreshDailyStatistics49::kTemplateId:
        account_rpt<schema::MDIncrementalRefreshDailyStatistics49>(m, only);
        break;
      case schema::MDIncrementalRefreshLimitsBanding50::kTemplateId:
        account_rpt<schema::MDIncrementalRefreshLimitsBanding50>(m, only);
        break;
      case schema::MDIncrementalRefreshSessionStatistics51::kTemplateId:
        account_rpt<schema::MDIncrementalRefreshSessionStatistics51>(m, only);
        break;
      case schema::ChannelReset4::kTemplateId:
        if (only < 0) channel_reset(m, rx_ts, sink);
        break;
      case schema::SecurityStatus30::kTemplateId:
        if (only < 0) decode_status(m);
        break;
      case schema::MDInstrumentDefinitionFuture54::kTemplateId:
        if (only < 0) decode_definition(m);
        break;
      case schema::AdminHeartbeat12::kTemplateId:
        ++stats_.heartbeats;
        break;
      default:
        ++stats_.unknown_templates;  // skipped by MsgSize
        continue;
    }
    decoded = true;
  }
  if (cur.malformed()) ++stats_.malformed;
  if (stats_.sink_overflows != overflow_before) return ParseStatus::Overflow;
  if (stats_.malformed != malformed_before) return ParseStatus::Malformed;
  return decoded ? ParseStatus::Ok : ParseStatus::Ignored;
}

// ---- MBP book -----------------------------------------------------------------------------------

void Mdp3Decoder::decode_book(const MessageView& m,
                              std::int64_t rx_ts,
                              EventSink& sink,
                              std::int32_t only) noexcept {
  const schema::MDIncrementalRefreshBook46 msg(m.body, m.header.block_length, m.header.version);
  const auto group = msg.no_md_entries();
  if (!msg.valid() || !group.valid()) {
    ++stats_.malformed;
    return;
  }
  Batch& batch = *batch_;
  batch.index = -1;
  batch.transact_time = msg.transact_time();
  for (const auto e : group) {
    const std::int32_t index = instruments_->index_of(e.security_id());
    if (index < 0) {
      ++stats_.unknown_security;
      continue;
    }
    if (only >= 0 && index != only) continue;
    if (index != batch.index) {
      flush(sink, rx_ts);
      batch.index = index;
    }
    const auto i = static_cast<std::size_t>(index);
    const std::uint32_t rpt = e.rpt_seq();
    if (check_rpt(i, rpt) != RptCheck::Apply) continue;
    books_[i].transact_time = batch.transact_time;
    std::size_t side = kBid;
    switch (e.md_entry_type()) {
      case schema::MDEntryTypeBook::Bid:
        side = kBid;
        break;
      case schema::MDEntryTypeBook::Offer:
        side = kAsk;
        break;
      case schema::MDEntryTypeBook::ImpliedBid:
      case schema::MDEntryTypeBook::ImpliedOffer:
        ++stats_.implied_ignored;
        continue;
      case schema::MDEntryTypeBook::BookReset: {
        // Empty the instrument's book (both sides).
        if (!batch.room(kBid, kMaxLevelsPerEntry) || !batch.room(kAsk, kMaxLevelsPerEntry)) {
          flush(sink, rx_ts);
        }
        if (batch.first_rpt == 0) batch.first_rpt = rpt;
        batch.last_rpt = rpt;
        for (std::size_t s = 0; s < 2; ++s) {
          MbpSide& ms = books_[i].sides[s];
          for (std::size_t k = 0; k < ms.count; ++k) batch.push(s, ms.levels[k].price, Qty{});
          ms.count = 0;
        }
        ++stats_.book_entries;
        continue;
      }
      default:
        ++stats_.other_entry_types;
        continue;
    }
    if (!batch.room(side, kMaxLevelsPerEntry)) flush(sink, rx_ts);
    if (batch.first_rpt == 0) batch.first_rpt = rpt;
    batch.last_rpt = rpt;
    apply_entry(i, side, e);
  }
  flush(sink, rx_ts);
}

// MDUpdateAction semantics for an MBP side (MDPriceLevel is 1-based, level 1 = best):
//   New (0)        insert at the level, shift worse levels down; the level past MarketDepth
//                  falls off (CME wiki, "Market by Price - Multiple Depth Book")
//   Change (1)     new quantity at an unchanged price (same page: "not sent when the price
//                  changes at a given price level")
//   Delete (2)     remove the level, shift worse levels up (same page)
//   DeleteThru (3) remove every level from 1 through MDPriceLevel      (FIX 5.0 SP2 tag 279)
//   DeleteFrom (4) remove MDPriceLevel and every worse level            (FIX 5.0 SP2 tag 279)
//   Overlay (5)    replace price and quantity at the level, no shifting (FIX 5.0 SP2 tag 279)
// Anything that contradicts the local book (level out of range, crossed neighbours, a Change
// whose price differs) is a book error: the instrument goes into recovery.
void Mdp3Decoder::apply_entry(std::size_t index,
                              std::size_t side,
                              const schema::MDIncrementalRefreshBook46::NoMDEntries& e) noexcept {
  MbpSide& s = books_[index].sides[side];
  Batch& batch = *batch_;
  const std::size_t depth = instruments_->at(index).market_depth;
  const std::size_t level = e.md_price_level();
  if (level == 0 || level > depth) {
    book_error(index);
    return;
  }
  const std::size_t k = level - 1;
  Price px{};
  Qty qty{};
  const auto price_and_size = [&]() noexcept {
    if (!e.md_entry_px().to_fixed(px)) {
      ++stats_.price_rejects;
      return false;
    }
    if (!e.has_md_entry_size() || e.md_entry_size() < 0) return false;
    qty = Qty::from_int(e.md_entry_size());
    return true;
  };

  switch (e.md_update_action()) {
    case schema::MDUpdateAction::New: {
      if (!price_and_size() || k > s.count || !ordered(s, side, k, k, px)) {
        book_error(index);
        return;
      }
      if (s.count == depth) {
        batch.push(side, s.levels[depth - 1].price, Qty{});  // falls off the bottom
        s.count = static_cast<std::uint8_t>(s.count - 1);
      }
      for (std::size_t j = s.count; j > k; --j) s.levels[j] = s.levels[j - 1];
      s.levels[k] = Level{px, qty};
      s.count = static_cast<std::uint8_t>(s.count + 1);
      batch.push(side, px, qty);
      break;
    }
    case schema::MDUpdateAction::Change: {
      if (!price_and_size() || k >= s.count || s.levels[k].price != px) {
        book_error(index);
        return;
      }
      s.levels[k].qty = qty;
      batch.push(side, px, qty);
      break;
    }
    case schema::MDUpdateAction::Delete: {
      if (k >= s.count) {
        book_error(index);
        return;
      }
      const auto wire_px = e.md_entry_px();
      if (!wire_px.is_null() && wire_px.to_fixed(px) && px != s.levels[k].price) {
        book_error(index);
        return;
      }
      batch.push(side, s.levels[k].price, Qty{});
      erase_levels(s, k, 1);
      break;
    }
    case schema::MDUpdateAction::DeleteThru: {
      const std::size_t n = std::min<std::size_t>(level, s.count);
      for (std::size_t j = 0; j < n; ++j) batch.push(side, s.levels[j].price, Qty{});
      erase_levels(s, 0, n);
      break;
    }
    case schema::MDUpdateAction::DeleteFrom: {
      for (std::size_t j = k; j < s.count; ++j) batch.push(side, s.levels[j].price, Qty{});
      if (k < s.count) s.count = static_cast<std::uint8_t>(k);
      break;
    }
    case schema::MDUpdateAction::Overlay: {
      if (!price_and_size() || k > s.count || !ordered(s, side, k, k + 1, px)) {
        book_error(index);
        return;
      }
      if (k == s.count) {
        s.count = static_cast<std::uint8_t>(s.count + 1);
      } else if (s.levels[k].price != px) {
        batch.push(side, s.levels[k].price, Qty{});
      }
      s.levels[k] = Level{px, qty};
      batch.push(side, px, qty);
      break;
    }
    default:
      book_error(index);
      return;
  }
  ++stats_.book_entries;
}

void Mdp3Decoder::flush(EventSink& sink, std::int64_t rx_ts) noexcept {
  Batch& b = *batch_;
  const auto nb = static_cast<std::uint32_t>(b.count[kBid]);
  const auto na = static_cast<std::uint32_t>(b.count[kAsk]);
  if (b.index >= 0 && (nb != 0 || na != 0)) {
    const auto index = static_cast<std::size_t>(b.index);
    const std::uint32_t len = BookDeltaMsg::size_for(nb, na);
    auto* msg = sink.reserve<BookDeltaMsg>(len);
    if (msg == nullptr) {
      // The engine's book would miss this delta: resynchronise the instrument.
      ++stats_.sink_overflows;
      events_.overflow = true;
      mark_recovering(index);
    } else {
      init_header(*msg, EventType::BookDelta, instruments_->at(index).id, cfg_.venue, len);
      msg->hdr.venue_seq = b.last_rpt;
      msg->hdr.exch_ts = Timestamp{static_cast<std::int64_t>(b.transact_time)};
      msg->hdr.recv_ts = Timestamp{rx_ts};
      msg->first_update_id = b.first_rpt;
      msg->last_update_id = b.last_rpt;
      msg->prev_update_id = b.first_rpt == 0 ? 0 : b.first_rpt - 1;
      fill_book_msg(*msg, len, b.levels[kBid].data(), nb, b.levels[kAsk].data(), na);
      sink.commit();
      ++stats_.book_deltas;
    }
  }
  b.count = {};
  b.first_rpt = 0;
}

bool Mdp3Decoder::emit_snapshot(std::size_t index,
                                const MbpBookState& state,
                                std::uint64_t transact_time,
                                std::int64_t rx_ts,
                                EventSink& sink) noexcept {
  const std::uint32_t nb = state.sides[kBid].count;
  const std::uint32_t na = state.sides[kAsk].count;
  const std::uint32_t len = BookSnapshotMsg::size_for(nb, na);
  auto* msg = sink.reserve<BookSnapshotMsg>(len);
  if (msg == nullptr) {
    ++stats_.sink_overflows;
    return false;
  }
  init_header(*msg, EventType::BookSnapshot, instruments_->at(index).id, cfg_.venue, len);
  msg->hdr.flags = EventHeader::kSnapshot;
  msg->hdr.venue_seq = state.rpt_seq;
  msg->hdr.exch_ts = Timestamp{static_cast<std::int64_t>(transact_time)};
  msg->hdr.recv_ts = Timestamp{rx_ts};
  msg->first_update_id = state.rpt_seq;
  msg->last_update_id = state.rpt_seq;
  msg->prev_update_id = 0;
  fill_book_msg(
      *msg, len, state.sides[kBid].levels.data(), nb, state.sides[kAsk].levels.data(), na);
  sink.commit();
  ++stats_.book_snapshots;
  return true;
}

// ---- trades / statistics ------------------------------------------------------------------------

void Mdp3Decoder::decode_trades(const MessageView& m,
                                std::int64_t rx_ts,
                                EventSink& sink,
                                std::int32_t only) noexcept {
  const schema::MDIncrementalRefreshTradeSummary48 msg(
      m.body, m.header.block_length, m.header.version);
  const auto group = msg.no_md_entries();
  if (!msg.valid() || !group.valid()) {
    ++stats_.malformed;
    return;
  }
  const auto exch_ts = Timestamp{static_cast<std::int64_t>(msg.transact_time())};
  for (const auto e : group) {
    const std::int32_t index = instruments_->index_of(e.security_id());
    if (index < 0) {
      ++stats_.unknown_security;
      continue;
    }
    if (only >= 0 && index != only) continue;
    const auto i = static_cast<std::size_t>(index);
    if (check_rpt(i, e.rpt_seq()) != RptCheck::Apply) continue;
    // Change (1) is a trade adjustment and Delete (2) a trade cancel ("MDP 3.0 - Market Data
    // Incremental Refresh - Trade Summary"); the engine has no event for either.
    if (e.md_update_action() != schema::MDUpdateAction::New) {
      ++stats_.trade_adjustments;
      continue;
    }
    Price px{};
    if (!e.md_entry_px().to_fixed(px)) {
      ++stats_.price_rejects;
      continue;
    }
    auto* t = sink.reserve<TradeMsg>();
    if (t == nullptr) {
      ++stats_.sink_overflows;  // trades are not recoverable from snapshots: counted only
      continue;
    }
    init_header(*t, EventType::Trade, instruments_->at(i).id, cfg_.venue);
    t->hdr.venue_seq = e.rpt_seq();
    t->hdr.exch_ts = exch_ts;
    t->hdr.recv_ts = Timestamp{rx_ts};
    t->price = px;
    t->qty = Qty::from_int(e.md_entry_size());
    t->trade_id = e.has_md_trade_entry_id() ? e.md_trade_entry_id() : e.rpt_seq();
    std::memset(t->pad_, 0, sizeof(t->pad_));
    switch (e.aggressor_side()) {
      case schema::AggressorSide::Buy:
        t->aggressor = Side::Buy;
        break;
      case schema::AggressorSide::Sell:
        t->aggressor = Side::Sell;
        break;
      default:
        t->aggressor = Side::Buy;
        t->pad_[0] = kTradeNoAggressor;
        break;
    }
    sink.commit();
    ++stats_.trades;
  }
}

template <class Msg>
void Mdp3Decoder::account_rpt(const MessageView& m, std::int32_t only) noexcept {
  const Msg msg(m.body, m.header.block_length, m.header.version);
  const auto group = msg.no_md_entries();
  if (!msg.valid() || !group.valid()) {
    ++stats_.malformed;
    return;
  }
  for (const auto e : group) {
    const std::int32_t index = instruments_->index_of(e.security_id());
    if (index < 0) {
      ++stats_.unknown_security;
      continue;
    }
    if (only >= 0 && index != only) continue;
    ++stats_.stat_entries;
    static_cast<void>(check_rpt(static_cast<std::size_t>(index), e.rpt_seq()));
  }
}

// ---- definitions / status / channel reset -------------------------------------------------------

void Mdp3Decoder::decode_definition(const MessageView& m) noexcept {
  const schema::MDInstrumentDefinitionFuture54 msg(m.body, m.header.block_length, m.header.version);
  if (!msg.valid()) {
    ++stats_.malformed;
    return;
  }
  bool inserted = false;
  Mdp3Instrument* d = instruments_->upsert(msg.security_id(), inserted);
  if (d == nullptr) {
    ++stats_.table_full;
    return;
  }
  ++stats_.definitions;
  d->symbol = Symbol(msg.symbol());
  d->security_group = FixedString<6>(msg.security_group());
  d->asset = FixedString<6>(msg.asset());
  Price tick{};
  d->tick = msg.min_price_increment().to_fixed(tick) ? tick : Price{};
  d->display_factor_e9 = msg.display_factor().mantissa;
  Qty uom{};
  d->unit_of_measure_qty = msg.unit_of_measure_qty().to_fixed(uom) ? uom : Qty{};
  d->contract_multiplier = msg.has_contract_multiplier() ? msg.contract_multiplier() : 0;
  d->appl_id = msg.appl_id();
  d->deleted = msg.security_update_action() == schema::SecurityUpdateAction::Delete;
  std::uint8_t depth = kMaxMbpDepth;
  std::uint8_t implied = 0;
  for (const auto f : msg.no_md_feed_types()) {
    const std::int8_t md = f.market_depth();  // Int8 on the wire
    if (md <= 0) continue;
    const std::uint8_t clamped = std::min(static_cast<std::uint8_t>(md), kMaxMbpDepth);
    if (f.md_feed_type() == "GBX") {
      depth = clamped;
    } else if (f.md_feed_type() == "GBI") {
      implied = clamped;
    }
  }
  d->implied_depth = implied;
  const auto index = static_cast<std::size_t>(instruments_->index_of(msg.security_id()));
  if (inserted) {
    init_book(index, depth);
  } else {
    d->market_depth = depth;
  }
}

void Mdp3Decoder::decode_status(const MessageView& m) noexcept {
  const schema::SecurityStatus30 msg(m.body, m.header.block_length, m.header.version);
  if (!msg.valid()) {
    ++stats_.malformed;
    return;
  }
  ++stats_.security_status;
  if (!msg.has_security_id()) return;  // group-level status (tag 1151 / 6937)
  const std::int32_t index = instruments_->index_of(msg.security_id());
  if (index < 0) return;
  instruments_->at(static_cast<std::size_t>(index)).trading_status =
      static_cast<std::uint8_t>(msg.security_trading_status());
}

// "MDP 3.0 - Channel Reset": the order book and statistics of every instrument on the channel
// are emptied; MBP RptSeq restarts at 1 and the book is rebuilt from the incremental feed.
void Mdp3Decoder::channel_reset(const MessageView& m,
                                std::int64_t rx_ts,
                                EventSink& sink) noexcept {
  const schema::ChannelReset4 msg(m.body, m.header.block_length, m.header.version);
  if (!msg.valid()) {
    ++stats_.malformed;
    return;
  }
  ++stats_.channel_resets;
  events_.channel_reset = true;
  for (std::size_t i = 0; i < instruments_->size(); ++i) {
    MbpBookState& b = books_[i];
    if (b.recovering) --recovering_;
    b = MbpBookState{};
    b.transact_time = msg.transact_time();
    if (!emit_snapshot(i, b, msg.transact_time(), rx_ts, sink)) {
      events_.overflow = true;
      mark_recovering(i);
    }
  }
}

// ---- snapshots ----------------------------------------------------------------------------------

bool Mdp3Decoder::apply_snapshot(const schema::SnapshotFullRefresh52& snap,
                                 std::int64_t rx_ts,
                                 EventSink& sink) noexcept {
  const auto reject = [this]() noexcept {
    ++stats_.snapshots_rejected;
    return false;
  };
  const auto group = snap.no_md_entries();
  if (!snap.valid() || !group.valid()) {
    ++stats_.malformed;
    return reject();
  }
  const std::int32_t index = instruments_->index_of(snap.security_id());
  if (index < 0) {
    ++stats_.unknown_security;
    return reject();
  }
  const auto i = static_cast<std::size_t>(index);
  const std::size_t depth = instruments_->at(i).market_depth;
  MbpBookState next{};
  next.rpt_seq = snap.rpt_seq();
  next.transact_time = snap.transact_time();
  std::array<std::uint32_t, 2> present{};
  for (const auto e : group) {
    std::size_t side = kBid;
    switch (e.md_entry_type()) {
      case schema::MDEntryType::Bid:
        side = kBid;
        break;
      case schema::MDEntryType::Offer:
        side = kAsk;
        break;
      case schema::MDEntryType::ImpliedBid:
      case schema::MDEntryType::ImpliedOffer:
        ++stats_.implied_ignored;
        continue;
      default:
        continue;  // statistics carried by the snapshot are not decoded
    }
    const int level = e.has_md_price_level() ? e.md_price_level() : 0;
    Price px{};
    if (level < 1 || static_cast<std::size_t>(level) > depth || !e.has_md_entry_size() ||
        e.md_entry_size() < 0) {
      return reject();
    }
    if (!e.md_entry_px().to_fixed(px)) {
      ++stats_.price_rejects;
      return reject();
    }
    const auto k = static_cast<std::size_t>(level - 1);
    const std::uint32_t bit = 1U << k;
    if ((present[side] & bit) != 0) return reject();
    present[side] |= bit;
    next.sides[side].levels[k] = Level{px, Qty::from_int(e.md_entry_size())};
  }
  for (std::size_t s = 0; s < 2; ++s) {
    const auto n = static_cast<std::uint32_t>(std::popcount(present[s]));
    if (present[s] != (n == 32 ? ~0U : (1U << n) - 1U)) return reject();  // holes
    MbpSide& side = next.sides[s];
    side.count = static_cast<std::uint8_t>(n);
    for (std::size_t k = 1; k < side.count; ++k) {
      if (!better(s, side.levels[k - 1].price, side.levels[k].price)) return reject();
    }
  }
  if (!emit_snapshot(i, next, next.transact_time, rx_ts, sink)) return reject();
  if (books_[i].recovering) --recovering_;
  books_[i] = next;
  ++stats_.snapshots_applied;
  return true;
}

bool Mdp3Decoder::reset_empty(std::size_t index, std::int64_t rx_ts, EventSink& sink) noexcept {
  MbpBookState next{};
  next.rpt_unknown = true;
  if (!emit_snapshot(index, next, 0, rx_ts, sink)) return false;
  if (books_[index].recovering) --recovering_;
  books_[index] = next;
  return true;
}

bool Mdp3Decoder::rpt_continuous(std::span<const std::byte> datagram,
                                 std::int32_t index,
                                 std::uint32_t& next_expected) const noexcept {
  const auto step = [&](std::int32_t security_id, std::uint32_t rpt) noexcept {
    if (instruments_->index_of(security_id) != index || rpt < next_expected) return true;
    if (rpt != next_expected) return false;
    ++next_expected;
    return true;
  };
  PacketCursor cur(datagram);
  MessageView m;
  while (cur.next(m)) {
    if (m.header.schema_id != kMdpSchemaId) continue;
    bool ok = true;
    switch (m.header.template_id) {
      case schema::MDIncrementalRefreshBook46::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshBook46>(m, step);
        break;
      case schema::MDIncrementalRefreshTradeSummary48::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshTradeSummary48>(m, step);
        break;
      case schema::MDIncrementalRefreshVolume37::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshVolume37>(m, step);
        break;
      case schema::MDIncrementalRefreshDailyStatistics49::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshDailyStatistics49>(m, step);
        break;
      case schema::MDIncrementalRefreshLimitsBanding50::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshLimitsBanding50>(m, step);
        break;
      case schema::MDIncrementalRefreshSessionStatistics51::kTemplateId:
        ok = walk_rpt<schema::MDIncrementalRefreshSessionStatistics51>(m, step);
        break;
      default:
        break;
    }
    if (!ok) return false;
  }
  return true;
}

}  // namespace fastmm::codecs::mdp3
