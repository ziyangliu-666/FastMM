// MDP3 simulation (plan 7): seeded random order flow into the sim MatchingEngine is published as
// CME MDP 3.0 packets (trade summaries and top-N MBP level changes per event) on lines A and B,
// plus a snapshot loop on demand. The receiver's decoded books and the engine-side L2Books built
// from its events must equal the matching engine's top N after every event, with loss on one
// line, loss on both (snapshot recovery), duplicates and reordering.
#include "mdp3_test_util.hpp"

#include "fastmm/core/rng.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs::mdp3;
using namespace fastmm::codecs::mdp3::test;

namespace {

constexpr std::size_t kInstruments = 3;
constexpr std::array<std::uint8_t, kInstruments> kDepth = {10, 5, 3};
constexpr std::array<std::string_view, kInstruments> kSymbols = {"SIMA", "SIMB", "SIMC"};
constexpr std::int32_t kSidBase = 5000;
constexpr std::int64_t kTickRaw = 25'000'000;  // 0.25
constexpr std::size_t kChunk = 16;             // entries per message

class TradeRecorder final : public sim::MatchingSink {
 public:
  struct Fill {
    Price price;
    Qty qty;
    Side aggressor;
    std::uint64_t id;
  };
  std::vector<Fill> fills;
  void on_trade(
      InstrumentId, Price p, Qty q, Side aggressor, std::uint64_t id, Timestamp) override {
    fills.push_back(Fill{p, q, aggressor, id});
  }
};

class SimExchange {
 public:
  explicit SimExchange(std::uint64_t seed)
      : rng_(seed), me_(std::make_unique<sim::MatchingEngine>(kInstruments, &trades_)) {
    for (std::size_t i = 0; i < kInstruments; ++i) {
      pubs_.emplace_back(kSidBase + static_cast<std::int32_t>(i), kDepth[i]);
      mid_[i] = 16'000 + 400 * static_cast<std::int64_t>(i);
    }
  }

  std::vector<Bytes> definitions() {
    Packer pk(*this);
    for (std::size_t i = 0; i < kInstruments; ++i) {
      DefinitionSpec d;
      d.security_id = kSidBase + static_cast<std::int32_t>(i);
      d.symbol = kSymbols[i];
      d.security_group = "SIM";
      d.asset = "SIM";
      d.tick = Price::from_raw(kTickRaw);
      d.market_depth = kDepth[i];
      pk.add([&](PacketBuilder& p) { return encode_definition54(p, d); });
    }
    return pk.done();
  }

  // One random order (or cancel) and the packets it produced.
  std::vector<Bytes> step() {
    const std::size_t i = rng_.uniform(kInstruments);
    const InstrumentId id{static_cast<std::uint32_t>(i)};
    now_ += 1'000;
    const Timestamp ts{now_};
    trades_.fills.clear();
    const std::uint64_t roll = rng_.uniform(100);
    auto& live = live_[i];
    if (roll < 25 && !live.empty()) {
      const std::size_t k = rng_.uniform(live.size());
      static_cast<void>(me_->cancel(sim::kGeneratorAccount, live[k], ts));
      live[k] = live.back();
      live.pop_back();
    } else {
      sim::NewOrder o;
      o.account = sim::kGeneratorAccount;
      o.cl_ord_id = ClientOrderId{next_id_++};
      o.instrument = id;
      o.side = rng_.uniform(2) == 0 ? Side::Buy : Side::Sell;
      o.type = OrderType::Limit;
      const bool ioc = roll >= 88;
      o.tif = ioc ? TimeInForce::Ioc : TimeInForce::Gtc;
      const std::int64_t off = ioc ? rng_.between(-3, 0) : rng_.between(-1, 14);  // < 0 crosses
      const std::int64_t ticks = o.side == Side::Buy ? mid_[i] - off : mid_[i] + 1 + off;
      o.price = Price::from_raw(ticks * kTickRaw);
      o.qty = Qty::from_int(rng_.between(1, 20));
      const sim::SubmitResult r = me_->submit(o, ts);
      if (r.accepted() && r.resting.is_positive()) live.push_back(o.cl_ord_id);
    }
    if (rng_.uniform(40) == 0) mid_[i] += rng_.between(-1, 1);
    return publish(i);
  }

  // One iteration of the Market Recovery loop: a packet per instrument with book activity.
  [[nodiscard]] std::vector<Bytes> snapshot_loop() const {
    std::vector<std::size_t> active;
    for (std::size_t i = 0; i < kInstruments; ++i) {
      if (pubs_[i].rpt_seq() != 0) active.push_back(i);
    }
    std::vector<Bytes> out;
    std::uint32_t snap_seq = 0;
    for (const std::size_t i : active) {
      out.push_back(make_packet(++snap_seq, [&](PacketBuilder& p) {
        SnapshotSpec s;
        s.last_msg_seq_num_processed = seq_;
        s.tot_num_reports = static_cast<std::uint32_t>(active.size());
        s.security_id = pubs_[i].security_id();
        s.rpt_seq = pubs_[i].rpt_seq();
        s.transact_time = static_cast<std::uint64_t>(now_);
        REQUIRE(encode_snapshot52(p, s, pubs_[i].levels(Side::Buy), pubs_[i].levels(Side::Sell)));
      }));
    }
    return out;
  }

  [[nodiscard]] std::vector<Level> top(std::size_t i, Side side) const {
    std::array<Level, kMaxMbpDepth> buf{};
    const std::size_t n =
        me_->l2_snapshot(InstrumentId{static_cast<std::uint32_t>(i)}, side, buf.data(), kDepth[i]);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
  }
  [[nodiscard]] std::uint32_t last_seq() const noexcept { return seq_; }
  [[nodiscard]] std::size_t trades_published() const noexcept { return trades_published_; }

 private:
  // Packs messages into packets of at most kMaxPacketBytes, opening a new one when full.
  class Packer {
   public:
    explicit Packer(SimExchange& ex) : ex_(ex), builder_(buf_) {}
    template <class F>
    void add(F&& encode) {
      if (!open_) start();
      if (!encode(builder_)) {
        finish();
        start();
        REQUIRE(encode(builder_));
      }
    }
    std::vector<Bytes> done() {
      if (open_) finish();
      return std::move(out_);
    }

   private:
    void start() {
      REQUIRE(builder_.begin(++ex_.seq_, static_cast<std::uint64_t>(ex_.now_)));
      open_ = true;
    }
    void finish() {
      const auto p = builder_.packet();
      out_.emplace_back(p.begin(), p.end());
      open_ = false;
    }
    SimExchange& ex_;
    std::array<std::byte, kMaxPacketBytes> buf_{};
    PacketBuilder builder_;
    std::vector<Bytes> out_;
    bool open_ = false;
  };

  std::vector<Bytes> publish(std::size_t i) {
    Packer pk(*this);
    MbpPublisher& pub = pubs_[i];
    const auto tt = static_cast<std::uint64_t>(now_);
    // CME sends the trade summary of an event before its book updates.
    std::vector<TradeEntry> trades;
    for (const auto& f : trades_.fills) {
      TradeEntry t;
      t.security_id = pub.security_id();
      t.rpt_seq = pub.next_rpt_seq();
      t.price = f.price;
      t.qty = static_cast<std::int32_t>(f.qty.units());
      t.orders = 2;
      t.aggressor =
          f.aggressor == Side::Buy ? schema::AggressorSide::Buy : schema::AggressorSide::Sell;
      t.trade_entry_id = static_cast<std::uint32_t>(f.id);
      trades.push_back(t);
    }
    trades_published_ += trades.size();
    for (std::size_t k = 0; k < trades.size(); k += kChunk) {
      const std::span<const TradeEntry> chunk(trades.data() + k,
                                              std::min(kChunk, trades.size() - k));
      pk.add([&](PacketBuilder& p) { return encode_trade48(p, tt, end_of_event(), chunk); });
    }
    std::array<MbpEntry, 2 * MbpPublisher::kMaxEntriesPerUpdate> entries{};
    std::size_t n = pub.update(Side::Buy, top(i, Side::Buy), entries);
    n += pub.update(Side::Sell, top(i, Side::Sell), std::span<MbpEntry>(entries).subspan(n));
    for (std::size_t k = 0; k < n; k += kChunk) {
      const std::span<const MbpEntry> chunk(entries.data() + k, std::min(kChunk, n - k));
      pk.add([&](PacketBuilder& p) { return encode_book46(p, tt, end_of_event(), chunk); });
    }
    return pk.done();
  }

  Xoshiro256ss rng_;
  TradeRecorder trades_;
  std::unique_ptr<sim::MatchingEngine> me_;
  std::vector<MbpPublisher> pubs_;
  std::array<std::int64_t, kInstruments> mid_{};
  std::array<std::vector<ClientOrderId>, kInstruments> live_{};
  std::uint64_t next_id_ = 1;
  std::uint32_t seq_ = 0;
  std::int64_t now_ = 1'700'000'000'000'000'000;
  std::size_t trades_published_ = 0;
};

struct Receiver {
  std::unique_ptr<Mdp3Feed> feed;
  RecordingRing ring;
  EngineBooks books{kInstruments};
  std::int64_t rx = 1'000'000'000;

  explicit Receiver(const Mdp3FeedConfig& cfg) : feed(std::make_unique<Mdp3Feed>(cfg)) {}

  void inc(FeedLine line, const Bytes& p) {
    static_cast<void>(feed->on_incremental(line, p, rx, ring.sink));
    books.apply(ring.drain());
  }
  void snap(const Bytes& p) {
    static_cast<void>(feed->on_snapshot(p, rx, ring.sink));
    books.apply(ring.drain());
  }
  void tick(std::int64_t ns) {
    rx += ns;
    feed->on_timer(rx, ring.sink);
    books.apply(ring.drain());
  }
  [[nodiscard]] bool caught_up(const SimExchange& ex) const {
    return feed->next_expected_seq() == ex.last_seq() + 1 && feed->held_packets() == 0;
  }
  // (instrument, side, view) combinations that differ from the matching engine's top N; the
  // decoder's own book and the engine-side L2Book are both checked. Recovering instruments count
  // as mismatches only when `all`.
  [[nodiscard]] std::size_t mismatches(const SimExchange& ex, bool all) const {
    std::size_t bad = 0;
    const Mdp3Decoder& dec = feed->decoder();
    for (std::size_t i = 0; i < kInstruments; ++i) {
      const std::int32_t index =
          dec.instruments().index_of(kSidBase + static_cast<std::int32_t>(i));
      if (index < 0) {
        ++bad;
        continue;
      }
      const auto idx = static_cast<std::size_t>(index);
      const MbpBookState& b = dec.book(idx);
      if (b.recovering) {
        if (all) ++bad;
        continue;
      }
      const L2Book<64>& engine = *books.books[dec.instruments().at(idx).id.value];
      for (const Side side : {Side::Buy, Side::Sell}) {
        const std::vector<Level> expect = ex.top(i, side);
        if (to_vector(b.sides[static_cast<std::size_t>(side)].view()) != expect) ++bad;
        if (l2_levels(engine, side) != expect) ++bad;
      }
    }
    return bad;
  }
};

Mdp3FeedConfig sim_config() {
  Mdp3FeedConfig c;
  c.reorder_window = 32;
  c.gap_timeout_ns = 3'000'000;
  c.recovery_packets = 4096;
  return c;
}

}  // namespace

TEST_CASE("codecs.mdp3.sim: decoded books equal the matching engine top N after every event") {
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    INFO("seed " << seed);
    SimExchange ex(seed);
    Receiver r(sim_config());
    for (const Bytes& p : ex.definitions()) r.inc(FeedLine::A, p);
    std::size_t bad = 0;
    std::size_t events = 0;
    for (int step = 0; step < 1500; ++step) {
      const std::vector<Bytes> pkts = ex.step();
      for (const Bytes& p : pkts) r.inc(FeedLine::A, p);
      if (!pkts.empty()) {
        ++events;
        bad += r.mismatches(ex, true);
      }
    }
    CHECK(bad == 0);
    CHECK(events > 500);
    CHECK(r.feed->state() == ConnState::Live);
    const Mdp3DecoderStats& ds = r.feed->decoder().stats();
    CHECK(ds.definitions == kInstruments);
    CHECK(ds.book_errors == 0);
    CHECK(ds.rpt_gaps == 0);
    CHECK(ds.price_rejects == 0);
    CHECK(r.feed->stats().gaps == 0);
    CHECK(ex.trades_published() > 0);
    CHECK(r.books.trades == ex.trades_published());
    CHECK(r.books.deltas > 0);
  }
}

TEST_CASE("codecs.mdp3.sim: packets lost on line A are covered by line B") {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    INFO("seed " << seed);
    SimExchange ex(seed);
    Receiver r(sim_config());
    Xoshiro256ss net(seed * 7919);
    for (const Bytes& p : ex.definitions()) {
      r.inc(FeedLine::A, p);
      r.inc(FeedLine::B, p);
    }
    std::size_t bad = 0;
    for (int step = 0; step < 1500; ++step) {
      const std::vector<Bytes> pkts = ex.step();
      for (const Bytes& p : pkts) {
        if (net.uniform(100) < 30) {
          r.inc(FeedLine::B, p);
        } else if (net.uniform(2) == 0) {
          r.inc(FeedLine::A, p);
          r.inc(FeedLine::B, p);
        } else {
          r.inc(FeedLine::B, p);
          r.inc(FeedLine::A, p);
        }
      }
      if (!pkts.empty()) bad += r.mismatches(ex, true);
    }
    CHECK(bad == 0);
    CHECK(r.feed->stats().gaps == 0);
    CHECK(r.feed->stats().duplicates > 0);
    CHECK(r.feed->state() == ConnState::Live);
  }
}

TEST_CASE(
    "codecs.mdp3.sim: loss on both lines recovers from the snapshot loop and the books match "
    "again") {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    INFO("seed " << seed);
    SimExchange ex(seed);
    Receiver r(sim_config());
    Xoshiro256ss net(seed * 104729);
    for (const Bytes& p : ex.definitions()) r.inc(FeedLine::A, p);
    std::size_t bad = 0;
    std::int64_t lose_left = 0;
    for (int step = 0; step < 2500; ++step) {
      const bool losses_allowed = step < 2000;
      if (losses_allowed && lose_left == 0 && (step == 300 || net.uniform(400) == 0)) {
        lose_left = net.between(1, 6);
      }
      for (const Bytes& p : ex.step()) {
        if (lose_left > 0) {
          --lose_left;
          continue;
        }
        r.inc(FeedLine::A, p);
        r.inc(FeedLine::B, p);
      }
      r.tick(1'000'000);  // one millisecond per event
      if (r.feed->needs_snapshots() && step % 20 == 0) {
        for (const Bytes& s : ex.snapshot_loop()) r.snap(s);
      }
      if (r.caught_up(ex)) bad += r.mismatches(ex, false);
    }
    CHECK(bad == 0);
    CHECK(r.feed->state() == ConnState::Live);
    CHECK(r.caught_up(ex));
    CHECK(r.mismatches(ex, true) == 0);
    const Mdp3FeedStats& fs = r.feed->stats();
    CHECK(fs.gaps >= 1);
    CHECK(fs.resyncs >= 1);
    CHECK(fs.snapshots_used >= 1);
    CHECK(r.feed->decoder().stats().book_errors == 0);
    CHECK(std::find(r.books.states.begin(), r.books.states.end(), ConnState::Resyncing) !=
          r.books.states.end());
    REQUIRE_FALSE(r.books.states.empty());
    CHECK(r.books.states.back() == ConnState::Live);
  }
}

TEST_CASE("codecs.mdp3.sim: duplicated and reordered packets inside the window change nothing") {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    INFO("seed " << seed);
    SimExchange ex(seed);
    Mdp3FeedConfig cfg = sim_config();
    cfg.gap_timeout_ns = 1'000'000'000;
    Receiver r(cfg);
    Xoshiro256ss net(seed * 31337);
    for (const Bytes& p : ex.definitions()) r.inc(FeedLine::A, p);
    std::deque<std::pair<FeedLine, Bytes>> queue;
    std::vector<Bytes> history;
    const auto deliver_front = [&]() {
      const std::pair<FeedLine, Bytes> item = std::move(queue.front());
      queue.pop_front();
      r.inc(item.first, item.second);
    };
    std::size_t bad = 0;
    for (int step = 0; step < 1500; ++step) {
      for (const Bytes& p : ex.step()) {
        history.push_back(p);
        queue.emplace_back(FeedLine::A, p);
        queue.emplace_back(FeedLine::B, p);
        if (history.size() > 10 && net.uniform(100) < 5) {
          queue.emplace_back(FeedLine::B, history[history.size() - 1 - net.uniform(10)]);
        }
      }
      for (std::size_t k = 1; k < queue.size(); ++k) {
        if (net.uniform(100) < 30) std::swap(queue[k - 1], queue[k]);
      }
      while (queue.size() > 8) deliver_front();
      if (r.caught_up(ex)) bad += r.mismatches(ex, true);
    }
    while (!queue.empty()) deliver_front();
    CHECK(bad == 0);
    CHECK(r.caught_up(ex));
    CHECK(r.mismatches(ex, true) == 0);
    CHECK(r.feed->stats().gaps == 0);
    CHECK(r.feed->stats().held > 0);
    CHECK(r.feed->stats().duplicates > 0);
    CHECK(r.feed->state() == ConnState::Live);
  }
}
