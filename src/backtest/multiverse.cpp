#include "fastmm/backtest/multiverse.hpp"

#include "tape.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/sim/venue_order.hpp"
#include "fastmm/strategies/registry.hpp"

#include <malloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace fastmm::bt {

namespace {

using sim::SimKey;

// Runs one pass holds at most: each keeps an engine and a simulated venue, several MB.
constexpr std::size_t kMaxRunsPerPass = 128;

Timestamp event_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

bool queue_model(sim::FillModel m) noexcept {
  return m == sim::FillModel::L2Queue || m == sim::FillModel::L3Queue;
}

// The venue an instrument's messages go through (SimTransport::link): its own, else the
// configuration's default; an id past the table goes through the first venue simulated.
std::array<std::uint8_t, kMaxInstruments> venue_of_instruments(const BacktestConfig& c) {
  std::array<std::uint8_t, kMaxInstruments> out{};
  std::uint8_t first = static_cast<std::uint8_t>(kMaxVenues);
  for (const Instrument& i : c.instruments) {
    const VenueId v = i.venue.valid() ? i.venue : c.transport.venue;
    first = std::min(first, v.value);
  }
  if (c.instruments.size() == 0) first = c.transport.venue.value;
  out.fill(first);
  for (const Instrument& i : c.instruments)
    out[i.id.value] = (i.venue.valid() ? i.venue : c.transport.venue).value;
  return out;
}

// ---- the orders the runs rest at the venue ------------------------------------------------------

// Per instrument and side, the prices the runs rest orders at and which runs (each once per price,
// with a count); per instrument the runs with any order on it and those holding a cross. reach()
// lists the runs a recorded event can change (SimTransport::venue_event): a superset is safe, since
// the venue's half of an event is a no-op for a run it does not reach.
class RestingIndex final : public sim::RestingWatch {
 public:
  RestingIndex(std::size_t instruments, std::size_t runs)
      : n_(instruments),
        sides_(instruments * 2),
        own_(runs * instruments * 2),
        on_inst_(instruments),
        crossed_(instruments),
        inst_count_(runs * instruments, 0),
        inst_pos_(runs * instruments, 0),
        crossed_pos_(runs * instruments, kNone),
        marks_(instruments),
        own_changed_(instruments),
        shared_(runs, false),
        stamp_(runs, 0) {}

  void on_rest(
      std::uint32_t run, InstrumentId id, Side side, Price px, bool resting) noexcept override {
    if (id.value >= n_) return;
    const std::size_t si = std::size_t{id.value} * 2 + static_cast<std::size_t>(side);
    Levels& levels = sides_[si];
    std::vector<OwnLevel>& mine = own_[static_cast<std::size_t>(run) * n_ * 2 + si];
    auto it =
        std::find_if(mine.begin(), mine.end(), [&](const OwnLevel& o) { return o.px == px.raw; });
    if (resting) {
      if (it != mine.end()) {
        ++it->count;
      } else {
        auto lv = lower(levels, px.raw);
        if (lv == levels.end() || lv->px != px.raw) lv = levels.insert(lv, PriceRuns{px.raw, {}});
        mine.push_back(OwnLevel{px.raw, 1, static_cast<std::uint32_t>(lv->runs.size())});
        lv->runs.push_back(run);
      }
    } else if (it != mine.end() && --it->count == 0) {
      const auto lv = lower(levels, px.raw);
      std::vector<std::uint32_t>& level = lv->runs;
      const std::uint32_t pos = it->pos;
      level[pos] = level.back();
      level.pop_back();
      if (pos < level.size()) {
        std::vector<OwnLevel>& moved = own_[static_cast<std::size_t>(level[pos]) * n_ * 2 + si];
        for (OwnLevel& o : moved) {
          if (o.px == px.raw) o.pos = pos;
        }
      }
      if (level.empty()) levels.erase(lv);
      *it = mine.back();
      mine.pop_back();
    }
    const std::size_t at = static_cast<std::size_t>(run) * n_ + id.value;
    std::vector<std::uint32_t>& on = on_inst_[id.value];
    if (resting) {
      if (inst_count_[at]++ == 0) {
        inst_pos_[at] = static_cast<std::uint32_t>(on.size());
        on.push_back(run);
      }
    } else if (inst_count_[at] > 0) {
      --inst_count_[at];
      if (inst_count_[at] == 0) remove(on, inst_pos_, inst_pos_[at], id);
    }
  }

  void on_crossed(std::uint32_t run, InstrumentId id, bool crossed) noexcept override {
    if (id.value >= n_) return;
    const std::size_t at = static_cast<std::size_t>(run) * n_ + id.value;
    std::vector<std::uint32_t>& c = crossed_[id.value];
    if (crossed && crossed_pos_[at] == kNone) {
      crossed_pos_[at] = static_cast<std::uint32_t>(c.size());
      c.push_back(run);
    } else if (!crossed && crossed_pos_[at] != kNone) {
      const std::uint32_t pos = crossed_pos_[at];
      crossed_pos_[at] = kNone;
      c[pos] = c.back();
      c.pop_back();
      if (pos < c.size()) crossed_pos_[static_cast<std::size_t>(c[pos]) * n_ + id.value] = pos;
    }
  }

  void on_own_changed(std::uint32_t run, InstrumentId id) noexcept override {
    if (id.value < n_ && shared_[run]) own_changed_[id.value].push_back(run);
  }
  // The runs of a feed group whose resting quantity on `id` changed, for the group to send with
  // its next message of `id`; a run's feed of its own sends it itself (shared(run, false)).
  [[nodiscard]] std::vector<std::uint32_t>& own_changed(InstrumentId id) noexcept {
    return own_changed_[id.value];
  }
  void set_shared(std::uint32_t run, bool shared) { shared_[run] = shared; }

  // Run `run` marks a derivative account on `id` with every event but a trade.
  void add_mark(std::uint32_t run, InstrumentId id) {
    if (id.value < n_) marks_[id.value].push_back(run);
  }

  // The runs whose venue half of `md` (taken by `world` already) can change anything, each once,
  // into `out`. `late`: the event's venue time is before one taken earlier, so it may print before
  // an order that rests now (QueuePositionModel::on_trade).
  void reach(const EventHeader& md,
             const sim::SimWorld& world,
             sim::FillModel model,
             bool late,
             std::vector<std::uint32_t>& out) {
    out.clear();
    const InstrumentId id = md.instrument;
    if (id.value >= n_) return;
    ++event_;
    const bool l3 = model == sim::FillModel::L3Queue;
    const Timestamp now = event_time(md);
    switch (md.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
        if (!l3) {
          if (world.snapshot()) {
            add_all(on_inst_[id.value], out);
          } else {
            for (const sim::SimWorld::LevelChange& c : world.changes()) {
              if (c.new_qty < c.old_qty) at_price(id, c.side, c.px, out);
            }
          }
        }
        crosses(id, world, now, out);
        break;
      case EventType::BookTicker:
        if (!l3 && world.touch_newer()) {
          const QueueTouch& t = world.touch_seen();
          if (t.bid_px.is_positive()) at_or_better(id, Side::Buy, t.bid_px, out);
          if (t.ask_px.is_positive()) at_or_better(id, Side::Sell, t.ask_px, out);
        }
        crosses(id, world, now, out);
        break;
      case EventType::Trade: {
        const auto& t = msg_cast<TradeMsg>(&md);
        if (l3 || !t.has_aggressor()) return;
        if (late) {
          add_all(on_inst_[id.value], out);
          return;
        }
        // The maker side's orders the print reaches: at its price, or one it printed through.
        const Side maker = opposite(t.aggressor);
        at_or_better(id, maker, t.price, out);
        return;
      }
      case EventType::OrderAddL3:
      case EventType::OrderExecL3:
      case EventType::OrderCancelL3:
      case EventType::OrderReplaceL3: {
        if (!l3) return;
        const sim::SimWorld::L3Change& c = world.l3();
        if (c.kind == sim::SimWorld::L3Change::Kind::None) return;
        at_price(id, c.side, c.px, out);
        // An execution meets our orders at or better than its price (l3_execution_reaches).
        if (c.kind == sim::SimWorld::L3Change::Kind::Exec) at_or_better(id, c.side, c.exec_px, out);
        return;
      }
      default:
        break;
    }
    add_all(marks_[id.value], out);
  }

 private:
  static constexpr std::uint32_t kNone = 0xFFFF'FFFF;
  // A price one run rests orders at on one side: how many, and the run's place in its level.
  struct OwnLevel {
    std::int64_t px;
    std::uint32_t count;
    std::uint32_t pos;
  };
  // The runs resting at a price; a side keeps them by price, ascending (few prices: the runs'
  // quotes near the touch).
  struct PriceRuns {
    std::int64_t px;
    std::vector<std::uint32_t> runs;
  };
  using Levels = std::vector<PriceRuns>;
  static Levels::iterator lower(Levels& v, std::int64_t px) noexcept {
    return std::lower_bound(
        v.begin(), v.end(), px, [](const PriceRuns& l, std::int64_t p) { return l.px < p; });
  }
  static Levels::const_iterator lower(const Levels& v, std::int64_t px) noexcept {
    return std::lower_bound(
        v.begin(), v.end(), px, [](const PriceRuns& l, std::int64_t p) { return l.px < p; });
  }

  void remove(std::vector<std::uint32_t>& v,
              std::vector<std::uint32_t>& pos_of,
              std::uint32_t pos,
              InstrumentId id) noexcept {
    v[pos] = v.back();
    v.pop_back();
    if (pos < v.size()) pos_of[static_cast<std::size_t>(v[pos]) * n_ + id.value] = pos;
  }
  void add(std::uint32_t run, std::vector<std::uint32_t>& out) {
    if (stamp_[run] == event_) return;
    stamp_[run] = event_;
    out.push_back(run);
  }
  void add_all(const std::vector<std::uint32_t>& runs, std::vector<std::uint32_t>& out) {
    for (const std::uint32_t r : runs) add(r, out);
  }
  void at_price(InstrumentId id, Side side, Price px, std::vector<std::uint32_t>& out) {
    const Levels& levels = sides_[std::size_t{id.value} * 2 + static_cast<std::size_t>(side)];
    const auto it = lower(levels, px.raw);
    if (it != levels.end() && it->px == px.raw) add_all(it->runs, out);
  }
  // Orders on `side` at `px` or better for that side (a bid at or above it, an ask at or below).
  void at_or_better(InstrumentId id, Side side, Price px, std::vector<std::uint32_t>& out) {
    const Levels& levels = sides_[std::size_t{id.value} * 2 + static_cast<std::size_t>(side)];
    if (side == Side::Buy) {
      for (auto it = lower(levels, px.raw); it != levels.end(); ++it) add_all(it->runs, out);
    } else {
      for (auto it = levels.begin(); it != levels.end() && it->px <= px.raw; ++it)
        add_all(it->runs, out);
    }
  }
  // The orders the venue's opposite side reaches after a view of the book, and those holding a
  // cross from the last one (QueuePositionModel::on_cross through queue_apply_cross).
  void crosses(InstrumentId id,
               const sim::SimWorld& world,
               Timestamp now,
               std::vector<std::uint32_t>& out) {
    const L2Book<256>& book = world.books()[id.value];
    const QueueTouch& touch = world.touches()[id.value];
    Price best[2];
    for (const Side s : {Side::Buy, Side::Sell}) {
      for_each_venue_level(book, touch, s, now, [&](const Level& l) {
        best[static_cast<std::size_t>(s)] = l.price;
        return false;
      });
    }
    if (best[1].is_positive()) at_or_better(id, Side::Buy, best[1], out);
    if (best[0].is_positive()) at_or_better(id, Side::Sell, best[0], out);
    add_all(crossed_[id.value], out);
  }

  std::size_t n_;
  std::vector<Levels> sides_;  // [instrument * 2 + side]: per price, the runs resting there
  std::vector<std::vector<OwnLevel>> own_;  // [(run * instruments + instrument) * 2 + side]
  std::vector<std::vector<std::uint32_t>> on_inst_;      // runs with an order on the instrument
  std::vector<std::vector<std::uint32_t>> crossed_;      // runs holding a cross on it
  std::vector<std::uint32_t> inst_count_;                // [run * n + instrument]: its orders there
  std::vector<std::uint32_t> inst_pos_;                  // its place in on_inst_
  std::vector<std::uint32_t> crossed_pos_;               // its place in crossed_, kNone if absent
  std::vector<std::vector<std::uint32_t>> marks_;        // runs marking a derivative account on it
  std::vector<std::vector<std::uint32_t>> own_changed_;  // per instrument (on_own_changed)
  std::vector<bool> shared_;                             // per run: in a feed group
  std::vector<std::uint64_t> stamp_;  // per run, the last event it was listed for
  std::uint64_t event_ = 0;
};

// ---- market data the runs share ---------------------------------------------------------------

// The market data in flight to one venue's engines, in arrival order; grows when a burst fills it.
class MdQueue {
 public:
  void push(const EventHeader& h) {
    if (ring_ == nullptr) ring_ = std::make_unique<MsgRing>(std::size_t{1} << 18, false);
    while (!ring_->try_push(&h, h.len)) {
      auto bigger = std::make_unique<MsgRing>(ring_->capacity() * 2, false);
      while (const std::byte* m = ring_->try_peek()) {
        const auto* e = reinterpret_cast<const EventHeader*>(m);
        static_cast<void>(bigger->try_push(e, e->len));
        ring_->release();
      }
      ring_ = std::move(bigger);
    }
  }
  [[nodiscard]] const EventHeader* head() const noexcept {
    return ring_ == nullptr ? nullptr : reinterpret_cast<const EventHeader*>(ring_->try_peek());
  }
  void pop() noexcept { ring_->release(); }

 private:
  std::unique_ptr<MsgRing> ring_;
};

// Which of a feed group's runs a message reaches (MdInterest): those that take every message, and
// per instrument those that take every book update, every trade or every ticker, and those with a
// band on the book's mid, best bid or best ask, kept in two heaps per band (the highest lower edge
// first, the lowest upper edge first) so a book update finds the runs whose band it left without
// looking at the others. A run's entry is filed again when its engine's interest moves; heap
// entries of an old band are dropped when they come up (a version per run and instrument).
class WakeIndex {
 public:
  void init(std::size_t instruments, std::size_t runs) {
    n_ = instruments;
    every_book_.resize(instruments);
    trades_.resize(instruments);
    tickers_.resize(instruments);
    for (std::size_t a = 0; a < kAxes; ++a) {
      lo_[a].resize(instruments);
      hi_[a].resize(instruments);
    }
    filed_.resize(runs * instruments);
    seen_.assign(runs, 0);
    every_pos_.assign(runs, kNone);
    stamp_.assign(runs, 0);
  }

  // Files run `run` again where its engine's interest moved.
  void refile(std::uint32_t run, const MdInterest& in) {
    if (in.epoch == seen_[run]) return;
    const std::uint64_t seen = seen_[run];
    seen_[run] = in.epoch;
    set(every_, every_pos_[run], run, in.every, [this](std::uint32_t r) -> std::uint32_t& {
      return every_pos_[r];
    });
    for (std::size_t i = 0; i < n_; ++i) {
      const MdInterest::Instrument& n = in.inst[i];
      if (n.epoch <= seen) continue;
      Filed& f = filed_[run * n_ + i];
      // An empty band takes every update.
      const bool every_book =
          n.every_book || n.lo >= n.hi || n.bid_lo >= n.bid_hi || n.ask_lo >= n.ask_hi;
      set(every_book_[i],
          f.every_book,
          run,
          every_book,
          [this, i](std::uint32_t r) -> std::uint32_t& { return filed_[r * n_ + i].every_book; });
      set(trades_[i], f.trades, run, n.trades, [this, i](std::uint32_t r) -> std::uint32_t& {
        return filed_[r * n_ + i].trades;
      });
      set(tickers_[i], f.tickers, run, n.tickers, [this, i](std::uint32_t r) -> std::uint32_t& {
        return filed_[r * n_ + i].tickers;
      });
      ++f.version;
      const std::int64_t lo[kAxes] = {n.lo, n.bid_lo, n.ask_lo};
      const std::int64_t hi[kAxes] = {n.hi, n.bid_hi, n.ask_hi};
      for (std::size_t a = 0; a < kAxes; ++a) {
        f.lo[a] = every_book ? MdInterest::kBelow : lo[a];
        f.hi[a] = every_book ? MdInterest::kAbove : hi[a];
      }
      push_band(run, i, f);
    }
  }

  // The runs message `m` reaches, after `books` took it, each once, into `out`; the band entries
  // it takes off are put back by restore() once the runs took it.
  void reach(const EventHeader& m,
             const InstrumentTable& instruments,
             const L2Book<256>* books,
             std::span<const std::uint32_t> group,
             std::vector<std::uint32_t>& out) {
    ++event_;
    out.clear();
    taken_.clear();
    add_all(every_, out);
    const InstrumentId id = m.instrument;
    const bool known = instruments.contains(id);
    switch (m.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot: {
        if (!known) return;
        const L2Book<256>& b = books[id.value];
        if (!b.is_valid()) {
          add_all(group, out);  // the strategy's on_book and the engine's crossed-book pull
          return;
        }
        add_all(every_book_[id.value], out);
        const std::int64_t value[kAxes] = {
            b.mid().raw, b.best_bid().price.raw, b.best_ask().price.raw};
        for (std::size_t a = 0; a < kAxes; ++a) {
          Heap& lo = lo_[a][id.value];
          while (!lo.empty() && lo.front().bound >= value[a]) take(lo, a, id.value, true, out);
          Heap& hi = hi_[a][id.value];
          while (!hi.empty() && hi.front().bound <= value[a]) take(hi, a, id.value, false, out);
        }
        return;
      }
      case EventType::Trade:
        if (known) add_all(trades_[id.value], out);
        return;
      case EventType::BookTicker:
      case EventType::OptionTicker:
        if (known) add_all(tickers_[id.value], out);
        return;
      default:
        add_all(group, out);
        return;
    }
  }
  // The band entries reach() took off whose band did not move go back: the book still has to come
  // back inside before they stop waking their run.
  void restore() {
    for (const Taken& t : taken_) {
      const Filed& f = filed_[t.run * n_ + t.inst];
      if (f.version != t.version) continue;
      Heap& h = t.low ? lo_[t.axis][t.inst] : hi_[t.axis][t.inst];
      h.push_back(Bound{t.low ? f.lo[t.axis] : f.hi[t.axis], t.run, f.version});
      std::push_heap(h.begin(), h.end(), t.low ? low_first : high_first);
    }
  }

 private:
  static constexpr std::uint32_t kNone = 0xFFFF'FFFF;
  static constexpr std::size_t kAxes = 3;  // the mid, the best bid, the best ask
  struct Bound {
    std::int64_t bound;
    std::uint32_t run;
    std::uint32_t version;
  };
  using Heap = std::vector<Bound>;
  // The heap of lower edges takes the highest first, that of upper edges the lowest.
  static bool low_first(const Bound& a, const Bound& b) noexcept { return a.bound < b.bound; }
  static bool high_first(const Bound& a, const Bound& b) noexcept { return a.bound > b.bound; }
  struct Filed {
    std::uint32_t every_book = kNone;  // place in every_book_, kNone: not there
    std::uint32_t trades = kNone;
    std::uint32_t tickers = kNone;
    std::uint32_t version = 0;
    std::int64_t lo[kAxes] = {MdInterest::kBelow, MdInterest::kBelow, MdInterest::kBelow};
    std::int64_t hi[kAxes] = {MdInterest::kAbove, MdInterest::kAbove, MdInterest::kAbove};
  };
  struct Taken {
    std::uint32_t run;
    std::uint32_t inst;
    std::uint32_t version;
    std::uint8_t axis;
    bool low;
  };
  // A list of runs with each run's place in it (`place(run)`): `run` in or out.
  template <class Place>
  static void set(std::vector<std::uint32_t>& list,
                  std::uint32_t& pos,
                  std::uint32_t run,
                  bool in,
                  Place&& place) {
    if (in == (pos != kNone)) return;
    if (in) {
      pos = static_cast<std::uint32_t>(list.size());
      list.push_back(run);
      return;
    }
    const std::uint32_t at = pos;
    pos = kNone;
    list[at] = list.back();
    list.pop_back();
    if (at < list.size()) place(list[at]) = at;
  }
  void push_band(std::uint32_t run, std::size_t i, const Filed& f) {
    for (std::size_t a = 0; a < kAxes; ++a) {
      if (f.lo[a] != MdInterest::kBelow) {
        lo_[a][i].push_back(Bound{f.lo[a], run, f.version});
        std::push_heap(lo_[a][i].begin(), lo_[a][i].end(), low_first);
      }
      if (f.hi[a] != MdInterest::kAbove) {
        hi_[a][i].push_back(Bound{f.hi[a], run, f.version});
        std::push_heap(hi_[a][i].begin(), hi_[a][i].end(), high_first);
      }
    }
  }
  void take(Heap& h, std::size_t axis, std::size_t i, bool low, std::vector<std::uint32_t>& out) {
    std::pop_heap(h.begin(), h.end(), low ? low_first : high_first);
    const Bound b = h.back();
    h.pop_back();
    if (filed_[b.run * n_ + i].version != b.version) return;
    taken_.push_back(Taken{
        b.run, static_cast<std::uint32_t>(i), b.version, static_cast<std::uint8_t>(axis), low});
    add(b.run, out);
  }
  void add(std::uint32_t run, std::vector<std::uint32_t>& out) {
    if (stamp_[run] == event_) return;
    stamp_[run] = event_;
    out.push_back(run);
  }
  void add_all(std::span<const std::uint32_t> runs, std::vector<std::uint32_t>& out) {
    for (const std::uint32_t r : runs) add(r, out);
  }

  std::size_t n_ = 0;
  std::vector<std::uint32_t> every_;
  std::vector<std::vector<std::uint32_t>> every_book_;
  std::vector<std::vector<std::uint32_t>> trades_;
  std::vector<std::vector<std::uint32_t>> tickers_;
  std::array<std::vector<Heap>, kAxes> lo_;
  std::array<std::vector<Heap>, kAxes> hi_;
  std::vector<Filed> filed_;              // [run * instruments + instrument]
  std::vector<std::uint64_t> seen_;       // per run, the interest epoch filed
  std::vector<std::uint32_t> every_pos_;  // per run, its place in every_
  std::vector<std::uint64_t> stamp_;      // per run, the last message it was listed for
  std::vector<Taken> taken_;
  std::uint64_t event_ = 0;
};

// Runs whose engines get the same market data: no own orders in their feed, and per venue the
// same fixed latency and arrival rule. They follow one MarketView (Engine::follow_market): the
// group applies each message to its books, hands it to the runs whose engines need it, and then
// lets the market view take it.
struct FeedGroup {
  // A venue's market-data path as the runs have it before the first message: equal paths draw the
  // same delays for the same messages.
  struct VenueMd {
    sim::MdPath path{};
    bool recorded_arrival = false;
    bool used = false;
    friend bool operator==(const VenueMd&, const VenueMd&) = default;
  };
  std::array<VenueMd, kMaxVenues> venues{};
  std::array<std::uint8_t, kMaxInstruments> venue_of{};
  std::unique_ptr<MarketView> market;
  std::array<MdQueue, kMaxVenues> queues;
  std::array<Timestamp, kMaxVenues> last_arrival{};
  // Per venue, the messages pushed and popped, and the OwnLevelsMsg of a run that goes right
  // ahead of one of them (SimTransport::own_levels): in `own`, with the message it goes ahead of
  // and the run in `own_to`, in the order they came.
  struct OwnAhead {
    std::uint64_t ahead_of;
    std::uint32_t run;
  };
  std::array<std::uint64_t, kMaxVenues> pushed{};
  std::array<std::uint64_t, kMaxVenues> popped{};
  std::array<MdQueue, kMaxVenues> own;
  std::array<std::vector<OwnAhead>, kMaxVenues> own_to;
  std::array<std::size_t, kMaxVenues> own_next{};
  std::vector<std::uint32_t> runs;
  WakeIndex wake;
  std::uint64_t forwarded = 0;
  std::uint64_t md_taken = 0;  // market data taken (SimDriver's count of it)
  Timestamp last_md{};         // the latest arrival taken
  // The next event its runs share: a recorded event or its own market data (the runs follow it).
  Timestamp next_shared = Timestamp::max();

  // SimTransport::forward() of every run of the group; the venue's queue it went to and its
  // arrival.
  std::uint8_t forward(const EventHeader& md, Timestamp recorded, Timestamp now, Timestamp& at) {
    const std::uint8_t v =
        md.instrument.value < kMaxInstruments ? venue_of[md.instrument.value] : venue_of[0];
    VenueMd& c = venues[v];
    Timestamp arrival = now + c.path.draw();
    if (c.recorded_arrival && recorded > now) arrival = arrival + (recorded - now);
    if (arrival < last_arrival[v]) arrival = last_arrival[v];
    last_arrival[v] = arrival;
    alignas(64) std::array<std::byte, kMaxSourceEventBytes> buf;
    std::memcpy(buf.data(), &md, md.len);
    auto* h = reinterpret_cast<EventHeader*>(buf.data());
    h->exch_ts = now;
    h->recv_ts = arrival;
    h->t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
    h->t1_delta = h->t2_delta = 0;
    h->seq = 0;
    queues[v].push(*h);
    ++pushed[v];
    ++forwarded;
    at = arrival;
    return v;
  }
  // Run `run`'s own message `m` goes right ahead of the last message pushed to venue `v`'s queue,
  // at its arrival.
  void own_ahead(std::uint8_t v, std::uint32_t run, const EventHeader& m, Timestamp arrival) {
    alignas(64) std::array<std::byte, kMaxSourceEventBytes> buf;
    std::memcpy(buf.data(), &m, m.len);
    auto* h = reinterpret_cast<EventHeader*>(buf.data());
    h->recv_ts = arrival;
    h->t0_cycles = Cycles{static_cast<std::uint64_t>(arrival.ns)};
    own[v].push(*h);
    own_to[v].push_back(OwnAhead{pushed[v], run});
  }
  // The key of the message that goes next and its venue (the lowest on a tie); false: none.
  bool next(sim::SimKey& key, std::size_t& venue) const noexcept {
    bool any = false;
    for (std::size_t v = 0; v < kMaxVenues; ++v) {
      const EventHeader* h = queues[v].head();
      if (h == nullptr || (any && !(h->recv_ts < key.t))) continue;
      key = sim::SimKey{h->recv_ts, sim::SimKey::kMarketData};
      venue = v;
      any = true;
    }
    return any;
  }
};

// The group a run could share, or none (its feed is its own): its venues' market-data paths
// before the first message.
bool shares_feed(const BacktestConfig& c, const sim::SimTransport& t, FeedGroup& g) {
  g.venue_of = venue_of_instruments(c);
  for (const Instrument& i : c.instruments) {
    const std::uint8_t v = g.venue_of[i.id.value];
    const sim::MdPath* path = v < kMaxVenues ? t.md_path(VenueId{v}) : nullptr;
    if (path == nullptr) return false;
    g.venues[v] = FeedGroup::VenueMd{*path, t.md_recorded_arrival(VenueId{v}), true};
  }
  return true;
}

// ---- one pass ---------------------------------------------------------------------------------

struct Run {
  MultiverseRun parts;
  SimClock* clock = nullptr;  // the run's
  std::size_t index = 0;      // into the configurations
  bool live = true;
  bool own_feed = true;  // forwards its own market data
  std::uint32_t group = 0;
  SimKey key = SimKey::never();
  std::uint32_t version = 0;
  bool touched = false;
  std::uint64_t forwarded = 0;  // its group's forwards when it ended early
  std::uint64_t md_due = 0;     // ... and the market data it took then
  std::uint64_t md_taken = 0;   // market data handed to it
};

struct HeapEntry {
  SimKey key;
  std::uint32_t run;
  std::uint32_t version;
  bool operator>(const HeapEntry& o) const noexcept {
    return key != o.key ? key > o.key : run > o.run;
  }
};

class Pass {
 public:
  Pass(const std::vector<BacktestConfig>& cfgs,
       std::span<const std::size_t> which,
       const MultiverseFactory& make,
       MdSource& source)
      : first_(cfgs[which.front()]),
        model_(first_.transport.fill_model),
        world_(first_.instruments.size()),
        index_(first_.instruments.size(), which.size()) {
    world_.set_l3(model_ == sim::FillModel::L3Queue);
    runs_.reserve(which.size());
    // Each run joins its feed group as it is built: an engine that follows a market lets its own
    // books go before the next run's are made.
    for (const std::size_t i : which) {
      const auto k = static_cast<std::uint32_t>(runs_.size());
      Run& r = runs_.emplace_back();
      r.index = i;
      r.parts = make(i, cfgs[i], source);
      if (!r.parts.session || !r.parts.hooks.valid())
        throw std::invalid_argument("multiverse: the run factory built no session");
      r.clock = &r.parts.session->backend().clock;
      const BacktestConfig& c = cfgs[i];
      sim::SimTransport& transport = r.parts.session->backend().transport;
      transport.attach_world(world_);
      transport.set_resting_watch(&index_, k);
      for (const Instrument& inst : c.instruments) {
        if (transport.marks_account(inst.id)) index_.add_mark(k, inst.id);
      }
      FeedGroup g;
      if (shares_feed(c, transport, g) && r.parts.hooks.follow_market != nullptr) {
        r.own_feed = false;
        std::size_t at = groups_.size();
        for (std::size_t j = 0; j < groups_.size(); ++j) {
          if (groups_[j]->venues == g.venues && groups_[j]->venue_of == g.venue_of) at = j;
        }
        if (at == groups_.size()) {
          auto ng = std::make_unique<FeedGroup>(std::move(g));
          ng->market = std::make_unique<MarketView>(first_.instruments);
          ng->wake.init(first_.instruments.size(), which.size());
          groups_.push_back(std::move(ng));
        }
        r.group = static_cast<std::uint32_t>(at);
        groups_[at]->runs.push_back(k);
        index_.set_shared(k, true);
        r.parts.hooks.follow_market(r.parts.hooks.ctx, *groups_[at]->market);
      } else {
        own_feed_.push_back(k);
      }
    }
    for (Run& r : runs_) {
      r.parts.session->begin(r.parts.hooks,
                             r.parts.runner.get(),
                             r.own_feed ? &next_src_ : &groups_[r.group]->next_shared);
    }
    for (std::uint32_t k = 0; k < runs_.size(); ++k) {
      rekey(k);
      refile(k);
    }
  }

  // `events` in venue order; `ordered`, when set, put them in it (its counts are read at the end).
  void run(MdSource& events,
           const sim::VenueOrderSource* ordered,
           std::vector<BacktestResult>& out,
           const ResultHook& done) {
    std::vector<std::uint64_t> by_instrument(first_.instruments.size() + 1);
    const auto count = [&](const EventHeader* h) {
      if (h != nullptr && h->instrument.valid())
        ++by_instrument[std::min<std::size_t>(h->instrument.value, by_instrument.size() - 1)];
      return h;
    };
    const EventHeader* ev = count(events.next());
    for (;;) {
      const SimKey src = ev != nullptr ? SimKey{event_time(*ev), SimKey::kSource} : SimKey::never();
      FeedGroup* g = nullptr;
      std::size_t venue = 0;
      SimKey md = SimKey::never();
      for (const std::unique_ptr<FeedGroup>& fg : groups_) {
        SimKey k;
        std::size_t v = 0;
        if (fg->next(k, v) && k < md) {
          md = k;
          g = fg.get();
          venue = v;
        }
      }
      next_src_ = src.t;
      for (const std::unique_ptr<FeedGroup>& fg : groups_) {
        SimKey k;
        std::size_t v = 0;
        fg->next_shared = fg->next(k, v) ? std::min(src.t, k.t) : src.t;
      }
      if (g != nullptr && md < src) {
        market_data(*g, venue, md);
        continue;
      }
      if (ev == nullptr) break;
      recorded_event(*ev, src);
      ev = count(events.next());
    }
    warn_unfed_instruments(first_.instruments, by_instrument);
    next_src_ = Timestamp::max();
    for (const std::unique_ptr<FeedGroup>& fg : groups_) fg->next_shared = Timestamp::max();
    for (Run& r : runs_) {
      if (r.live) reach(r);
      if (!r.own_feed) {
        const std::uint64_t due = r.live ? groups_[r.group]->md_taken : r.md_due;
        r.parts.session->credit_market_data(due - r.md_taken);
      }
      BacktestResult res = r.parts.session->end(r.parts.strategy_name);
      if (r.parts.on_end) r.parts.on_end();
      if (!r.own_feed)
        res.transport.md_forwarded += r.live ? groups_[r.group]->forwarded : r.forwarded;
      if (ordered != nullptr) {
        res.md_reordered = ordered->reordered();
        res.md_late = ordered->late();
      }
      if (done) done(r.index, res);
      out[r.index] = std::move(res);
    }
  }

 private:
  // A run alone has its clock moved by every recorded event and every message of its market data,
  // taken or not: one here reaches the latest of them before it takes anything.
  void reach(Run& r) const noexcept {
    Timestamp t = last_src_;
    if (!r.own_feed && groups_[r.group]->last_md > t) t = groups_[r.group]->last_md;
    if (r.clock->now() < t) r.clock->set(t);
  }
  // A run of a feed group in its group's wake index again, when its engine's interest moved.
  void refile(std::uint32_t k) {
    Run& r = runs_[k];
    if (!r.live || r.own_feed) return;
    groups_[r.group]->wake.refile(k, *r.parts.hooks.md_interest(r.parts.hooks.ctx));
  }
  // The run's next own event onto the heap, when it moved.
  void rekey(std::uint32_t k) {
    Run& r = runs_[k];
    if (!r.live) return;
    const SimKey key = r.parts.session->next_key();
    if (key == r.key) return;
    r.key = key;
    ++r.version;
    if (key != SimKey::never()) heap_.push(HeapEntry{key, k, r.version});
  }
  void die(std::uint32_t k) {
    Run& r = runs_[k];
    r.live = false;
    if (!r.own_feed) {
      r.forwarded = groups_[r.group]->forwarded;
      r.md_due = groups_[r.group]->md_taken;
    }
  }
  // Steps run k up to `key`: false when it is over.
  bool advance(std::uint32_t k, SimKey key) {
    Run& r = runs_[k];
    if (!r.live) return false;
    if (!(r.key < key)) return true;
    reach(r);
    if (!r.parts.session->advance_before(key)) {
      die(k);
      return false;
    }
    rekey(k);
    refile(k);
    return true;
  }
  // Every run's own events before `key`.
  void advance_all(SimKey key) {
    while (!heap_.empty() && heap_.top().key < key) {
      const HeapEntry e = heap_.top();
      heap_.pop();
      Run& r = runs_[e.run];
      if (!r.live || e.version != r.version) continue;
      advance(e.run, key);
    }
  }

  void recorded_event(const EventHeader& ev, SimKey key) {
    advance_all(key);
    const Timestamp t = key.t;
    const bool late = t < last_src_;
    world_.before(ev, t);
    index_.reach(ev, world_, model_, late, reached_);
    touched_.clear();
    for (const std::uint32_t k : reached_) {
      if (!runs_[k].live) continue;
      reach(runs_[k]);
      runs_[k].parts.session->world_venue_event(ev);
      touch(k);
    }
    for (const std::uint32_t k : own_feed_) {
      if (!runs_[k].live) continue;
      reach(runs_[k]);
      runs_[k].parts.session->world_feed_event(ev);
      touch(k);
    }
    if (!groups_.empty()) forward(ev, t);
    world_.after(ev, t);
    if (t > last_src_) last_src_ = t;
    for (const std::uint32_t k : touched_) {
      runs_[k].touched = false;
      rekey(k);
      refile(k);
    }
  }
  void touch(std::uint32_t k) {
    if (runs_[k].touched) return;
    runs_[k].touched = true;
    touched_.push_back(k);
  }

  // SimTransport::feed_event() of a run whose feed shows none of its orders, for every group.
  void forward(const EventHeader& md, Timestamp now) {
    const InstrumentId id = md.instrument;
    const auto to_groups = [this](const EventHeader& m, Timestamp recorded, Timestamp venue) {
      for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        FeedGroup& g = *groups_[gi];
        Timestamp arrival;
        const std::uint8_t v = g.forward(m, recorded, venue, arrival);
        if (m.instrument.value < first_.instruments.size())
          own_levels(gi, v, m.instrument, venue, arrival);
      }
    };
    if ((id.value < first_.instruments.size() && sim::SimTransport::is_l3_event(md.type)) ||
        (md.type == EventType::BookTicker &&
         sim::SimTransport::hides_level(msg_cast<BookTickerMsg>(&md)))) {
      if (md.seq != 0) in_order_.put(nullptr, md.seq, md.recv_ts, now, to_groups);
      return;
    }
    if (md.seq != 0) {
      in_order_.put(&md, md.seq, md.recv_ts, now, to_groups);
    } else {
      to_groups(md, md.recv_ts, now);
    }
  }

  // The runs of group `gi` whose resting quantity on `id` changed send it right ahead of the
  // group's message of `id` (SimTransport::forward of a run alone).
  void own_levels(
      std::size_t gi, std::uint8_t v, InstrumentId id, Timestamp now, Timestamp arrival) {
    std::vector<std::uint32_t>& changed = index_.own_changed(id);
    if (changed.empty()) return;
    FeedGroup& g = *groups_[gi];
    std::size_t keep = 0;
    for (const std::uint32_t k : changed) {
      Run& r = runs_[k];
      if (r.group != gi) {
        changed[keep++] = k;
        continue;
      }
      if (!r.live) continue;
      const OwnLevelsMsg* m = r.parts.session->backend().transport.own_levels(id, now);
      if (m != nullptr) g.own_ahead(v, k, m->hdr, arrival);
    }
    changed.resize(keep);
  }

  void market_data(FeedGroup& g, std::size_t venue, SimKey key) {
    const EventHeader& m = *g.queues[venue].head();
    // Every run takes its own events before it, those of the group on the books before it.
    advance_all(key);
    own_ahead(g, venue);
    if ((m.type == EventType::BookDelta || m.type == EventType::BookSnapshot) &&
        first_.instruments.contains(m.instrument))
      g.market->books[m.instrument.value].apply_delta(msg_cast<BookDeltaMsg>(&m));
    g.wake.reach(m, first_.instruments, g.market->books.get(), g.runs, woken_);
    // A run whose timer is due at this instant fires it in the step that takes the message, as
    // alone.
    due_now(key.t, g);
    const bool counted = sim::SimDriver::is_market_data(m.type);
    for (const std::uint32_t k : woken_) {
      Run& r = runs_[k];
      if (!r.live) continue;
      reach(r);
      r.parts.session->shared_market_data(m);
      if (counted) ++r.md_taken;
      if (r.parts.session->stopped()) {
        die(k);
        continue;
      }
      // The step added at most orders and timers; a key it consumed (a timer due now) is left
      // early, which costs one empty advance and is then corrected.
      if (const SimKey s = r.parts.session->step_key(); s < r.key) {
        r.key = s;
        ++r.version;
        heap_.push(HeapEntry{s, k, r.version});
      }
      refile(k);
    }
    g.wake.restore();
    g.market->take(m, std::max(m.recv_ts, last_src_));
    g.last_md = std::max(g.last_md, m.recv_ts);
    if (counted) ++g.md_taken;
    g.queues[venue].pop();
    ++g.popped[venue];
  }
  // The runs' own messages that go right ahead of venue `venue`'s next message of group `g`, each
  // as the run's step, before the group's books take that message.
  void own_ahead(FeedGroup& g, std::size_t venue) {
    std::vector<FeedGroup::OwnAhead>& to = g.own_to[venue];
    std::size_t& next = g.own_next[venue];
    while (next < to.size() && to[next].ahead_of == g.popped[venue] + 1) {
      const std::uint32_t k = to[next++].run;
      const EventHeader& own = *g.own[venue].head();
      Run& r = runs_[k];
      if (r.live) {
        reach(r);
        r.parts.session->shared_market_data(own);
        if (r.parts.session->stopped()) {
          die(k);
        } else {
          if (const SimKey s = r.parts.session->step_key(); s < r.key) {
            r.key = s;
            ++r.version;
            heap_.push(HeapEntry{s, k, r.version});
          }
          refile(k);
        }
      }
      g.own[venue].pop();
    }
    if (next == to.size()) {
      to.clear();
      next = 0;
    }
  }
  // The runs of `g` with a timer due at `t` (the heap holds every run's next key) join woken_.
  void due_now(Timestamp t, FeedGroup& g) {
    held_.clear();
    while (!heap_.empty() && heap_.top().key.t == t && heap_.top().key.rank <= SimKey::kTimer) {
      const HeapEntry e = heap_.top();
      heap_.pop();
      held_.push_back(e);
      const Run& r = runs_[e.run];
      if (!r.live || e.version != r.version || r.own_feed || groups_[r.group].get() != &g) continue;
      if (std::find(woken_.begin(), woken_.end(), e.run) == woken_.end()) woken_.push_back(e.run);
    }
    for (const HeapEntry& e : held_) heap_.push(e);
  }

  const BacktestConfig& first_;
  sim::FillModel model_;
  sim::SimWorld world_;
  RestingIndex index_;
  sim::RecordedOrder in_order_;
  std::vector<Run> runs_;
  std::vector<std::unique_ptr<FeedGroup>> groups_;
  std::vector<std::uint32_t> own_feed_;
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<>> heap_;
  std::vector<std::uint32_t> reached_;
  std::vector<std::uint32_t> woken_;
  std::vector<HeapEntry> held_;

  std::vector<std::uint32_t> touched_;
  Timestamp last_src_{};
  Timestamp next_src_ = Timestamp::max();  // the next recorded event, which the runs follow
};

// The runs of a wave free gigabytes when it ends. glibc keeps most of it, and having freed large
// blocks it serves the next wave's from its heap rather than from fresh mappings, so without this
// each wave's runs land beside the last wave's free memory instead of in it: a sweep of 4,096
// points on 8 threads peaked at 11 GB rather than 7.
void release_freed_memory() noexcept {
#if defined(__GLIBC__)
  ::malloc_trim(0);
#endif
}

// A pass over `source` read on the calling thread, put in venue order by the pass itself.
void run_alone(Pass& pass,
               MdSource& source,
               Duration window,
               std::vector<BacktestResult>& out,
               const ResultHook& done) {
  if (window.ns > 0) {
    sim::VenueOrderSource ordered(source, window);
    pass.run(ordered, &ordered, out, done);
  } else {
    pass.run(source, nullptr, out, done);
  }
}

void check_shareable(const std::vector<BacktestConfig>& cfgs) {
  if (const std::string why = multiverse_refusal(cfgs); !why.empty())
    throw std::invalid_argument("multiverse: " + why);
}

MultiverseFactory registry_factory(const StrategyEntry& entry) {
  return [&entry](std::size_t, const BacktestConfig& cfg, MdSource& source) {
    MultiverseRun r;
    r.session = std::make_unique<BacktestSession>(cfg, &source, entry.schema);
    if (cfg.ab.enabled()) r.session->enable_ab(entry.publisher);
    r.runner = StrategyRegistry::instance().make(entry.name, TransportKind::Sim, r.session->deps());
    if (r.runner == nullptr) {
      throw std::invalid_argument("multiverse: strategy '" + std::string(entry.name) +
                                  "' has no simulator factory");
    }
    r.hooks = r.session->backend().hooks;
    r.strategy_name = std::string(r.runner->strategy_name());
    return r;
  };
}

const StrategyEntry& find_strategy(std::string_view strategy) {
  register_builtin_strategies();
  const StrategyEntry* entry = StrategyRegistry::instance().find(strategy);
  if (entry == nullptr)
    throw std::invalid_argument("multiverse: unknown strategy '" + std::string(strategy) + "'");
  return *entry;
}

}  // namespace

std::string multiverse_refusal(const std::vector<BacktestConfig>& cfgs) {
  if (cfgs.empty()) return {};
  const BacktestConfig& first = cfgs.front();
  for (const BacktestConfig& c : cfgs) {
    if (!queue_model(c.transport.fill_model))
      return "needs fill_model = \"l2_queue\" or \"l3_queue\"";
    if (c.transport.fill_model != first.transport.fill_model)
      return "the runs must share one fill_model";
    if (c.instruments.size() != first.instruments.size())
      return "the runs must share one instrument table";
    for (std::size_t i = 0; i < c.instruments.size(); ++i) {
      const InstrumentId id{static_cast<std::uint32_t>(i)};
      if (c.instruments.get(id).symbol != first.instruments.get(id).symbol ||
          c.instruments.get(id).venue != first.instruments.get(id).venue)
        return "the runs must share one instrument table";
    }
    if (c.reorder_window != first.reorder_window) return "the runs must share one reorder_window";
  }
  return {};
}

std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                           const MultiverseFactory& make,
                                           const SourceFactory& source_factory,
                                           int threads,
                                           const ResultHook& done) {
  std::vector<BacktestResult> out(cfgs.size());
  if (cfgs.empty()) return out;
  check_shareable(cfgs);
  if (!source_factory) throw std::invalid_argument("multiverse: needs recorded data");
  std::size_t w = threads > 0 ? static_cast<std::size_t>(threads)
                              : std::size_t{std::thread::hardware_concurrency()};
  if (w == 0) w = 1;
  // Passes of at most kMaxRunsPerPass runs, w at a time: as many waves of w passes as that takes.
  // Pass k takes runs k, k + G, k + 2G, ... of the G passes, so neighbouring grid points, which
  // cost alike, spread over them and the passes of a wave end together.
  const std::size_t per_wave = w * kMaxRunsPerPass;
  const std::size_t g = std::min(cfgs.size(), (cfgs.size() + per_wave - 1) / per_wave * w);
  w = std::min(w, g);
  std::vector<std::vector<std::size_t>> passes(g);
  for (std::size_t i = 0; i < cfgs.size(); ++i) passes[i % g].push_back(i);
  const Duration window = cfgs.front().reorder_window;
  const auto open = [&] {
    std::unique_ptr<MdSource> source = source_factory();
    if (source == nullptr) throw std::invalid_argument("multiverse: needs recorded data");
    return source;
  };
  if (w == 1) {
    for (const std::vector<std::size_t>& runs : passes) {
      {
        const std::unique_ptr<MdSource> source = open();
        Pass pass(cfgs, runs, make, *source);
        run_alone(pass, *source, window, out, done);
      }
      release_freed_memory();
    }
    return out;
  }
  // The passes end on their own threads: one result at a time to `done`.
  std::mutex done_mutex;
  const ResultHook serial = !done ? ResultHook{} : [&](std::size_t i, BacktestResult& r) {
    const std::lock_guard lock(done_mutex);
    done(i, r);
  };
  // Each wave reads the data once, on a thread of its own, for its passes.
  for (std::size_t first = 0; first < g; first += w) {
    const std::size_t m = std::min(w, g - first);
    {
      const std::unique_ptr<MdSource> source = open();
      Tape tape(*source, window, m);
      std::exception_ptr error;
      std::atomic<bool> failed{false};
      const auto fail = [&] {
        if (!failed.exchange(true)) error = std::current_exception();
        tape.stop();
      };
      {
        std::vector<std::jthread> pool;
        pool.reserve(m + 1);
        pool.emplace_back([&] {
          try {
            tape.fill();
          } catch (...) {
            fail();
          }
        });
        for (std::size_t k = first; k < first + m; ++k) {
          pool.emplace_back([&, k] {
            try {
              Tape::Cursor cursor(tape);
              Pass pass(cfgs, passes[k], make, cursor);
              pass.run(cursor, tape.ordered(), out, serial);
            } catch (...) {
              fail();
            }
          });
        }
      }
      if (error) std::rethrow_exception(error);
    }
    release_freed_memory();
  }
  return out;
}

std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                           std::string_view strategy,
                                           const SourceFactory& source_factory,
                                           int threads,
                                           const ResultHook& done) {
  if (cfgs.empty()) return {};
  check_shareable(cfgs);
  return run_multiverse(
      cfgs, registry_factory(find_strategy(strategy)), source_factory, threads, done);
}

std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                           std::string_view strategy,
                                           MdSource& source) {
  std::vector<BacktestResult> out(cfgs.size());
  if (cfgs.empty()) return out;
  check_shareable(cfgs);
  const MultiverseFactory make = registry_factory(find_strategy(strategy));
  std::vector<std::size_t> all(cfgs.size());
  for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
  Pass pass(cfgs, all, make, source);
  run_alone(pass, source, cfgs.front().reorder_window, out, {});
  return out;
}

std::vector<SweepPoint> sweep_multiverse(const BacktestConfig& base,
                                         std::string_view strategy,
                                         const ParamGrid& grid,
                                         const SourceFactory& source_factory,
                                         int threads,
                                         const ResultHook& done) {
  const std::vector<ParamMap> points = expand_grid(grid);
  const BacktestConfig start = shared_start(base);
  std::vector<BacktestConfig> cfgs;
  cfgs.reserve(points.size());
  for (const ParamMap& p : points) {
    BacktestConfig c = start;
    for (const auto& [k, v] : p) c.params[k] = v;
    c.output_dir.clear();
    c.journal_out.clear();
    c.keep_orders = false;
    cfgs.push_back(std::move(c));
  }
  // Without data, the synthetic market as a stream (l2_queue), the one each point would read.
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    std::unique_ptr<MdSource> s = source_factory ? source_factory() : nullptr;
    if (s == nullptr) s = std::make_unique<SyntheticSource>(synthetic_source_config(base));
    return s;
  };
  std::vector<BacktestResult> results = run_multiverse(cfgs, strategy, factory, threads, done);
  std::vector<SweepPoint> out(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    out[i].params = points[i];
    out[i].result = std::move(results[i]);
  }
  return out;
}

std::vector<SweepPoint> sweep_by_name(const BacktestConfig& base,
                                      std::string strategy,
                                      const ParamGrid& grid,
                                      const SourceFactory& source_factory,
                                      int threads,
                                      bool separately,
                                      const ResultHook& done) {
  if (!separately && queue_model(base.transport.fill_model))
    return sweep_multiverse(base, strategy, grid, source_factory, threads, done);
  return sweep(
      base,
      grid,
      source_factory,
      [strategy = std::move(strategy)](const BacktestConfig& c, MdSource* s) {
        return run_backtest(c, strategy, s);
      },
      threads,
      done);
}

WalkForwardReport walk_forward_by_name(const BacktestConfig& base,
                                       std::string strategy,
                                       const ParamGrid& grid,
                                       const SourceFactory& source_factory,
                                       int folds,
                                       std::string_view metric,
                                       int threads) {
  if (!queue_model(base.transport.fill_model)) {
    return walk_forward(
        base,
        grid,
        source_factory,
        [strategy = std::move(strategy)](const BacktestConfig& c, MdSource* s) {
          return run_backtest(c, strategy, s);
        },
        folds,
        metric,
        threads);
  }
  if (folds < 1) throw std::invalid_argument("walk_forward: folds must be >= 1");
  static_cast<void>(score_metric(metric));  // an unknown metric fails before anything runs
  WalkForwardReport rep;
  rep.metric = std::string(metric);
  for (const auto& [k, values] : grid) {
    if (!values.empty()) rep.param_names.push_back(k);
  }
  const auto k = static_cast<std::size_t>(folds);
  rep.folds.resize(k);
  if (k == 1) {
    rep.folds[0].points = sweep_multiverse(base, strategy, grid, source_factory, threads);
  } else {
    const std::vector<Timestamp> edges = fold_edges(base, source_factory, folds);
    for (std::size_t i = 0; i < k; ++i) {
      // Each fold starts on the book as of its first instant with a flat engine, as alone.
      const SourceFactory slice = [&]() -> std::unique_ptr<MdSource> {
        return fold_source(
            base, source_factory ? source_factory() : nullptr, edges[i], edges[i + 1]);
      };
      rep.folds[i].start_ts = edges[i].ns;
      rep.folds[i].end_ts = edges[i + 1].ns;
      rep.folds[i].points = sweep_multiverse(base, strategy, grid, slice, threads);
    }
  }
  score_walk_forward(rep);
  return rep;
}

}  // namespace fastmm::bt
