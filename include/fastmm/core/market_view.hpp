#pragma once
// MarketView and MdInterest: an engine that shares its market data with others takes only the
// messages it needs.
//
// The engines of a multiverse's runs whose feeds are the same (backtest/multiverse.hpp) read one
// set of books, which the multiverse applies each message to. Much of what an engine does with a
// message does not depend on its own orders: the book, the last valid mid and the time it came
// (risk's collar and stale checks, the positions' marks), the last trade (the fat-finger band),
// the feed's lag per venue, the counts. MarketView keeps that once for every engine that follows
// it (Engine::follow_market). Before such an engine takes anything it catches up with the messages
// it was not given, which leaves it exactly as taking each of them would have, except for what a
// message would have made it do: call the strategy, check max_loss, pull a crossed book's quotes.
//
// MdInterest says when that is: the engine keeps it current after every step, from what the
// strategy watches (StrategyContext::watch, quiet_book), the max_loss budget left over its
// positions, and the settings that act on every message. A message the engine does not need is
// one that, taken, would have changed nothing a catch-up does not restore.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/venue_health.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace fastmm {

struct MarketView {
  struct Instrument {
    std::uint64_t mid_at = 0;    // the message (count, from 1) that last set `mid`, 0: none
    std::uint64_t trade_at = 0;  // ... and `trade`
    Price mid;                   // of the last valid book
    Timestamp mid_ts;            // the engine time it was taken at
    Price trade;                 // the last trade's price
  };
  // What the engines count of the messages (Engine stats): every one, the book updates of a known
  // instrument, the trades.
  struct Counts {
    std::uint64_t messages = 0;
    std::uint64_t book_updates = 0;
    std::uint64_t trades = 0;
  };

  explicit MarketView(const InstrumentTable& instruments)
      : instruments_(&instruments),
        books(std::make_unique<L2Book<256>[]>(kMaxInstruments)),
        inst(std::make_unique<Instrument[]>(kMaxInstruments)) {}

  // Message `m` reached the engines at engine time `now` (its arrival, or later when the clock
  // already was), after its book took it and every engine that needed it did: the rows it changes
  // move on, as each engine's on_book_delta, on_trade and feed-lag tracking would have.
  void take(const EventHeader& m, Timestamp now) noexcept {
    ++counts.messages;
    const InstrumentId id = m.instrument;
    const bool known = instruments_->contains(id);
    switch (m.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
        if (!known) return;
        ++counts.book_updates;
        track_lag(m, now);
        if (const L2Book<256>& b = books[id.value]; b.is_valid()) {
          Instrument& r = inst[id.value];
          r.mid = b.mid();
          r.mid_ts = now;
          r.mid_at = counts.messages;
        }
        return;
      case EventType::Trade:
        ++counts.trades;
        if (known) {
          Instrument& r = inst[id.value];
          r.trade = msg_cast<TradeMsg>(&m).price;
          r.trade_at = counts.messages;
        }
        track_lag(m, now);
        return;
      case EventType::BookTicker:
      case EventType::OptionTicker:
        track_lag(m, now);
        return;
      default:
        return;
    }
  }
  // Engine::track_feed_lag with no limit.
  void track_lag(const EventHeader& h, Timestamp now) noexcept {
    if (!h.exch_ts.valid() || (h.flags & EventHeader::kSnapshot) != 0) return;
    static_cast<void>(health.on_md(h.venue, (h.recv_ts - h.exch_ts).ns, now, Duration{}));
    if (h.venue.value < health_at.size()) health_at[h.venue.value] = counts.messages;
  }

 private:
  const InstrumentTable* instruments_;

 public:
  std::unique_ptr<L2Book<256>[]> books;  // kMaxInstruments, indexed by instrument id
  std::unique_ptr<Instrument[]> inst;    // kMaxInstruments
  VenueHealth health;                    // the feed's lag per venue
  std::array<std::uint64_t, VenueHealth::kVenues> health_at{};  // the message that last moved it
  Counts counts;
};

// What an engine needs of the market data to come (Engine::md_interest). `epoch` moves whenever
// anything below does; an instrument's `epoch` is the one it last changed at.
struct MdInterest {
  static constexpr std::int64_t kBelow = std::numeric_limits<std::int64_t>::min();
  static constexpr std::int64_t kAbove = std::numeric_limits<std::int64_t>::max();
  struct Instrument {
    std::uint64_t epoch = 0;
    // A book update whose mid is at or beyond lo or hi (raw prices), or that leaves the book
    // invalid; every_book: every one.
    bool every_book = true;
    std::int64_t lo = kBelow;
    std::int64_t hi = kAbove;
    bool trades = true;   // trades
    bool tickers = true;  // book and option tickers
  };
  std::uint64_t epoch = 1;
  // Every message: a journal records them all, and the max_param_age, feed-lag and FX checks, the
  // venue's marks, the strategy's queue estimate and a quote held back for a token act on any.
  bool every = true;
  std::array<Instrument, kMaxInstruments> inst{};
};

}  // namespace fastmm
