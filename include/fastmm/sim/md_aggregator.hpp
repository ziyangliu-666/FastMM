#pragma once
// MdAggregator: turns the MatchingEngine's per-mutation book changes into the normalized
// market-data stream a venue would publish (8.1 / 8.3):
//
//   * every `interval` (Binance: 100 ms) one BookDeltaMsg per touched instrument with the
//     net level quantities, first_update_id == U and last_update_id == u, so the
//     connector-side sync FSM sees realistic batches;
//   * the first flush for an instrument is a BookSnapshotMsg (kSnapshot) taken from the
//     engine; an interval that overflows the change table is also sent as a snapshot;
//   * an optional BookTickerMsg whenever the top of book differs from the last one sent.
//
// Trades are not aggregated (the venue publishes them immediately); the owner forwards
// MatchingSink::on_trade itself. Emission goes through a plain function pointer + context so
// the aggregator has no dependency on where the bytes go.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace fastmm::sim {

inline constexpr std::size_t kMaxAggLevels = 1024;  // == kMaxBookLevelsPerMsg

struct MdAggregatorConfig {
  Duration interval = milliseconds(100);
  std::uint32_t snapshot_depth = 256;  // per side; L2Book<256> holds no more anyway
  bool book_ticker = true;
  VenueId venue{0};
};

class MdAggregator {
 public:
  using EmitFn = void (*)(void* ctx, EventHeader& msg, Timestamp venue_ts);

  MdAggregator(std::size_t instruments,
               const MatchingEngine& me,
               const MdAggregatorConfig& cfg,
               Timestamp start)
      : cfg_(cfg),
        me_(me),
        count_(std::clamp<std::size_t>(instruments, 1, kMaxInstruments)),
        st_(new InstState[count_]),
        buf_(new std::byte[BookDeltaMsg::size_for(kMaxAggLevels, kMaxAggLevels)]),
        next_flush_(start + cfg.interval) {}
  MdAggregator(const MdAggregator&) = delete;
  MdAggregator& operator=(const MdAggregator&) = delete;

  [[nodiscard]] Timestamp next_flush_ts() const noexcept { return next_flush_; }
  [[nodiscard]] const MdAggregatorConfig& config() const noexcept { return cfg_; }

  // MatchingSink::on_book_change forwarded here.
  void on_book_change(InstrumentId id, Side side, Price px, Qty qty, std::uint64_t uid) noexcept {
    if (id.value >= count_) return;
    InstState& s = st_[id.value];
    if (s.first_uid == 0) s.first_uid = uid;
    s.last_uid = uid;
    s.dirty = true;
    if (s.overflow) return;
    SideAgg& a = s.side[static_cast<std::size_t>(side)];
    if (std::uint32_t* idx = a.index.find(px.raw)) {
      a.changes[*idx].qty = qty;
      return;
    }
    if (a.changes.full() || a.index.size() >= a.index.kMaxSize) {
      s.overflow = true;
      return;
    }
    a.index.insert(px.raw, static_cast<std::uint32_t>(a.changes.size()));
    static_cast<void>(a.changes.push_back(Level{px, qty}));
  }

  // Publishes everything accumulated since the last flush and schedules the next one.
  void flush(Timestamp now, EmitFn emit, void* ctx) noexcept {
    for (std::size_t i = 0; i < count_; ++i)
      flush_instrument(InstrumentId{static_cast<std::uint32_t>(i)}, now, emit, ctx);
    while (next_flush_ <= now) next_flush_ += cfg_.interval;
  }
  // Emits a snapshot for one instrument now (used at start so the engine's book is valid).
  void emit_snapshot(InstrumentId id, Timestamp now, EmitFn emit, void* ctx) noexcept {
    if (id.value >= count_) return;
    InstState& s = st_[id.value];
    auto* d = reinterpret_cast<BookDeltaMsg*>(buf_.get());
    const std::uint32_t depth = cfg_.snapshot_depth;
    Level* lv = reinterpret_cast<Level*>(buf_.get() + sizeof(BookDeltaMsg));
    const auto nb = static_cast<std::uint32_t>(me_.l2_snapshot(id, Side::Buy, lv, depth));
    const auto na = static_cast<std::uint32_t>(me_.l2_snapshot(id, Side::Sell, lv + nb, depth));
    init_header(*d, EventType::BookSnapshot, id, cfg_.venue, BookDeltaMsg::size_for(nb, na));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.exch_ts = now;
    d->bid_count = nb;
    d->ask_count = na;
    d->first_update_id = d->last_update_id = me_.update_id(id);
    d->hdr.venue_seq = d->last_update_id;
    emit(ctx, d->hdr, now);
    clear(s);
    s.snapshot_sent = true;
    ++snapshots_;
  }

  [[nodiscard]] std::uint64_t deltas_sent() const noexcept { return deltas_; }
  [[nodiscard]] std::uint64_t snapshots_sent() const noexcept { return snapshots_; }
  [[nodiscard]] std::uint64_t tickers_sent() const noexcept { return tickers_; }

 private:
  struct SideAgg {
    StaticVector<Level, kMaxAggLevels> changes;
    OpenHashMap<std::int64_t, std::uint32_t, kMaxAggLevels * 2> index;
  };
  struct InstState {
    SideAgg side[2];
    std::uint64_t first_uid = 0;
    std::uint64_t last_uid = 0;
    Level last_bid{};
    Level last_ask{};
    bool dirty = false;
    bool overflow = false;
    bool snapshot_sent = false;
  };

  static void clear(InstState& s) noexcept {
    for (SideAgg& a : s.side) {
      a.changes.clear();
      a.index.clear();
    }
    s.first_uid = s.last_uid = 0;
    s.dirty = false;
    s.overflow = false;
  }

  void flush_instrument(InstrumentId id, Timestamp now, EmitFn emit, void* ctx) noexcept {
    InstState& s = st_[id.value];
    if (!s.snapshot_sent || s.overflow) {
      emit_snapshot(id, now, emit, ctx);
    } else if (s.dirty) {
      auto* d = reinterpret_cast<BookDeltaMsg*>(buf_.get());
      const auto& bids = s.side[0].changes;
      const auto& asks = s.side[1].changes;
      const auto nb = static_cast<std::uint32_t>(bids.size());
      const auto na = static_cast<std::uint32_t>(asks.size());
      init_header(*d, EventType::BookDelta, id, cfg_.venue, BookDeltaMsg::size_for(nb, na));
      d->hdr.exch_ts = now;
      d->hdr.venue_seq = s.last_uid;
      d->bid_count = nb;
      d->ask_count = na;
      d->first_update_id = s.first_uid;
      d->last_update_id = s.last_uid;
      d->prev_update_id = 0;
      if (nb > 0) std::memcpy(d->levels(), bids.data(), nb * sizeof(Level));
      if (na > 0) std::memcpy(d->levels() + nb, asks.data(), na * sizeof(Level));
      emit(ctx, d->hdr, now);
      clear(s);
      ++deltas_;
    }
    if (cfg_.book_ticker) {
      const MatchingEngine::TopOfBook top = me_.top_of_book(id);
      if (top.bid != s.last_bid || top.ask != s.last_ask) {
        BookTickerMsg t{};
        init_header(t, EventType::BookTicker, id, cfg_.venue);
        t.hdr.exch_ts = now;
        t.hdr.venue_seq = me_.update_id(id);
        t.bid_px = top.bid.price;
        t.bid_qty = top.bid.qty;
        t.ask_px = top.ask.price;
        t.ask_qty = top.ask.qty;
        emit(ctx, t.hdr, now);
        s.last_bid = top.bid;
        s.last_ask = top.ask;
        ++tickers_;
      }
    }
  }

  MdAggregatorConfig cfg_;
  const MatchingEngine& me_;
  std::size_t count_;
  std::unique_ptr<InstState[]> st_;
  std::unique_ptr<std::byte[]> buf_;
  Timestamp next_flush_;
  std::uint64_t deltas_ = 0;
  std::uint64_t snapshots_ = 0;
  std::uint64_t tickers_ = 0;
};

}  // namespace fastmm::sim
