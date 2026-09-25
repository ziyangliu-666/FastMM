#pragma once
// DataSource (8.4): a timestamp-ordered stream of normalized market-data events consumed
// by the simulator. Every source implements sim::MdSource (a virtual next()/reset() pair;
// one virtual call per event is noise next to the engine step) and satisfies the
// DataSourceLike concept. Row-based sources (CSV, arrays) share RowAssembler, which groups
// consecutive rows of one update into a single BookDelta/BookSnapshot message.
//
// Row format (CSV columns / array dtypes):
//   ts_ns  int64   event time
//   type   S | D | T | B   (0 | 1 | 2 | 3 in arrays): snapshot level, delta level,
//                          trade, book ticker (two rows: bid then ask)
//   inst   uint32  dense instrument id
//   side   B | A   (0 | 1): bid/ask, or the aggressor side for trades
//   price  decimal string / int64 raw 1e-8 / double
//   qty    decimal string / int64 raw 1e-8 / double     (0 == delete level)
//   seq    uint64  venue update id; consecutive S/D rows with equal (ts, inst, seq) form
//                  one message
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace fastmm::bt {

using sim::EventBuf;
using sim::kMaxSourceEventBytes;
using sim::MdSource;

template <class S>
concept DataSourceLike = requires(S& s) {
  { s.next() } -> std::same_as<const EventHeader*>;
  { s.reset() };
};

enum class RowType : std::uint8_t { Snapshot = 0, Delta = 1, Trade = 2, Ticker = 3 };

struct Row {
  std::int64_t ts = 0;
  RowType type = RowType::Delta;
  std::uint32_t inst = 0;
  Side side = Side::Buy;
  Price price{};
  Qty qty{};
  std::uint64_t seq = 0;
};

// Builds messages from a row stream. `Provider` is `bool(Row&)` returning false at end.
class RowAssembler {
 public:
  explicit RowAssembler(VenueId venue = VenueId{0}) noexcept : venue_(venue) {}

  template <class Provider>
  const EventHeader* next(Provider&& provide) {
    if (!have_row_ && !fetch(provide)) return nullptr;
    have_row_ = false;
    const Row first = row_;
    switch (first.type) {
      case RowType::Trade: {
        auto& t = buf_.as<TradeMsg>();
        init_header(t, EventType::Trade, InstrumentId{first.inst}, venue_);
        stamp(t.hdr, first);
        t.price = first.price;
        t.qty = first.qty;
        t.trade_id = first.seq;
        t.aggressor = first.side;
        return &t.hdr;
      }
      case RowType::Ticker: {
        auto& t = buf_.as<BookTickerMsg>();
        init_header(t, EventType::BookTicker, InstrumentId{first.inst}, venue_);
        stamp(t.hdr, first);
        t.bid_px = first.price;
        t.bid_qty = first.qty;
        if (fetch(provide) && row_.type == RowType::Ticker && row_.ts == first.ts &&
            row_.inst == first.inst && row_.side == Side::Sell) {
          t.ask_px = row_.price;
          t.ask_qty = row_.qty;
          have_row_ = false;
        }
        return &t.hdr;
      }
      case RowType::Snapshot:
      case RowType::Delta: {
        // Collect bids and asks of the same (ts, inst, seq, type) group.
        std::uint32_t nb = 0;
        std::uint32_t na = 0;
        Row cur = first;
        for (;;) {
          if (cur.side == Side::Buy) {
            if (nb < kMaxLevels) bids_[nb++] = Level{cur.price, cur.qty};
          } else {
            if (na < kMaxLevels) asks_[na++] = Level{cur.price, cur.qty};
          }
          if (!fetch(provide)) break;
          if (row_.type != first.type || row_.ts != first.ts || row_.inst != first.inst ||
              row_.seq != first.seq) {
            break;  // row_ stays buffered (have_row_ == true)
          }
          have_row_ = false;
          cur = row_;
        }
        auto& d = buf_.as<BookDeltaMsg>();
        const bool snap = first.type == RowType::Snapshot;
        init_header(d,
                    snap ? EventType::BookSnapshot : EventType::BookDelta,
                    InstrumentId{first.inst},
                    venue_,
                    BookDeltaMsg::size_for(nb, na));
        if (snap) d.hdr.flags |= EventHeader::kSnapshot;
        stamp(d.hdr, first);
        d.bid_count = nb;
        d.ask_count = na;
        d.first_update_id = d.last_update_id = first.seq;
        d.prev_update_id = 0;
        if (nb > 0) std::memcpy(d.levels(), bids_, nb * sizeof(Level));
        if (na > 0) std::memcpy(d.levels() + nb, asks_, na * sizeof(Level));
        return &d.hdr;
      }
    }
    return nullptr;
  }
  void reset() noexcept { have_row_ = false; }

 private:
  static constexpr std::uint32_t kMaxLevels = 256;
  template <class Provider>
  bool fetch(Provider& provide) {
    have_row_ = provide(row_);
    return have_row_;
  }
  static void stamp(EventHeader& h, const Row& r) noexcept {
    h.exch_ts = h.recv_ts = Timestamp{r.ts};
    h.t0_cycles = Cycles{static_cast<std::uint64_t>(r.ts)};
    h.venue_seq = r.seq;
  }

  VenueId venue_;
  Row row_{};
  bool have_row_ = false;
  EventBuf buf_{};
  Level bids_[kMaxLevels];
  Level asks_[kMaxLevels];
};

// Concatenation of sources that cover consecutive spans of time (one file per day). The
// caller owns them and is responsible for the order; events must not go back in time across a
// boundary.
class ChainSource final : public MdSource {
 public:
  explicit ChainSource(std::vector<MdSource*> sources) : sources_(std::move(sources)) {}
  const EventHeader* next() override {
    while (at_ < sources_.size()) {
      if (const EventHeader* h = sources_[at_]->next()) return h;
      ++at_;
    }
    return nullptr;
  }
  void reset() override {
    for (MdSource* s : sources_) s->reset();
    at_ = 0;
  }
  [[nodiscard]] Timestamp start_ts() const override {
    return sources_.empty() ? Timestamp{} : sources_.front()->start_ts();
  }

 private:
  std::vector<MdSource*> sources_;
  std::size_t at_ = 0;
};

// The events of `inner` in [t0, t1) of event time (exch_ts, recv_ts when unset), for walk-forward
// folds. The part before t0 is read but not yielded: it only rebuilds the book of every
// (venue, instrument) and keeps the last message of every other kind except trades, and those
// states come out first, stamped t0 (one BookSnapshot per book, then the rest in key order).
// A fold therefore starts on the book the full run would have had at t0. Ends at the first
// event at or after t1. `inner` is borrowed; reset() rewinds it.
class TimeSliceSource final : public MdSource {
 public:
  TimeSliceSource(MdSource& inner, Timestamp t0, Timestamp t1);
  ~TimeSliceSource() override;
  TimeSliceSource(const TimeSliceSource&) = delete;
  TimeSliceSource& operator=(const TimeSliceSource&) = delete;
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return t0_; }

 private:
  struct Primer;
  MdSource& inner_;
  Timestamp t0_;
  Timestamp t1_;
  std::unique_ptr<Primer> primer_;      // the state before t0 while it is being emitted
  const EventHeader* first_ = nullptr;  // first event at or after t0, held back by the primer
  bool started_ = false;
  bool done_ = false;
};

// k-way merge of several sources by event time (stable: lower source index first on ties).
class MergedSource final : public MdSource {
 public:
  explicit MergedSource(std::vector<MdSource*> sources) : sources_(std::move(sources)) {
    heads_.assign(sources_.size(), nullptr);
    for (std::size_t i = 0; i < sources_.size(); ++i) heads_[i] = sources_[i]->next();
  }
  const EventHeader* next() override {
    std::size_t best = sources_.size();
    Timestamp best_ts = Timestamp::max();
    for (std::size_t i = 0; i < heads_.size(); ++i) {
      if (heads_[i] == nullptr) continue;
      const Timestamp ts = time_of(*heads_[i]);
      if (best == sources_.size() || ts < best_ts) {
        best = i;
        best_ts = ts;
      }
    }
    if (best == sources_.size()) return nullptr;
    std::memcpy(buf_.bytes, heads_[best], heads_[best]->len);
    heads_[best] = sources_[best]->next();
    return &buf_.hdr();
  }
  void reset() override {
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      sources_[i]->reset();
      heads_[i] = sources_[i]->next();
    }
  }
  // Earliest start among the inputs (invalid if none reports one).
  [[nodiscard]] Timestamp start_ts() const override {
    Timestamp t{};
    for (const MdSource* s : sources_) {
      const Timestamp st = s->start_ts();
      if (st.valid() && (!t.valid() || st < t)) t = st;
    }
    return t;
  }
  [[nodiscard]] static Timestamp time_of(const EventHeader& h) noexcept {
    return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
  }

 private:
  std::vector<MdSource*> sources_;
  std::vector<const EventHeader*> heads_;
  EventBuf buf_{};
};

}  // namespace fastmm::bt
