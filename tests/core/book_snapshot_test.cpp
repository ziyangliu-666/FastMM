// write_book_snapshot(): the snapshot fastmm-gateway gives an attaching or lagging strategy out of
// its own copy of a book (AccountBook). An engine book built from it and the deltas after it must
// be the one the venue's own snapshot and the same deltas build.
#include "fastmm/core/book/book_snapshot.hpp"

#include "test_support.hpp"

#include "fastmm/core/account_book.hpp"
#include "fastmm/core/rng.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

using EngineBook = L2Book<256>;  // Engine::Book

constexpr InstrumentId kInst{0};
constexpr VenueId kVenue{0};
constexpr std::int64_t kTick = 1'000'000;  // 0.01 in Price raw units

// One market-data message, as bytes.
using Msg = std::vector<std::uint64_t>;
BookDeltaMsg& as_msg(Msg& m) {
  return *reinterpret_cast<BookDeltaMsg*>(m.data());
}
const BookDeltaMsg& as_msg(const Msg& m) {
  return *reinterpret_cast<const BookDeltaMsg*>(m.data());
}

Msg make_msg(const std::vector<Level>& bids, const std::vector<Level>& asks, bool snapshot) {
  const auto nb = static_cast<std::uint32_t>(bids.size());
  const auto na = static_cast<std::uint32_t>(asks.size());
  Msg m(BookDeltaMsg::size_for(nb, na) / 8, 0);
  BookDeltaMsg& d = as_msg(m);
  init_header(d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              kInst,
              kVenue,
              BookDeltaMsg::size_for(nb, na));
  if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
  d.bid_count = nb;
  d.ask_count = na;
  for (std::uint32_t i = 0; i < nb; ++i) d.levels()[i] = bids[i];
  for (std::uint32_t i = 0; i < na; ++i) d.levels()[nb + i] = asks[i];
  return m;
}

// The venue's full book (deeper than any snapshot message) and the deltas it publishes.
struct Venue {
  std::map<std::int64_t, std::int64_t, std::greater<>> bids;  // best first
  std::map<std::int64_t, std::int64_t> asks;                  // best first
  std::uint64_t seq = 1000;
  std::int64_t ts = 1'700'000'000'000'000'000;
  Xoshiro256ss rng{7};

  // Every other tick, so that changes can add levels as well as remove them.
  explicit Venue(int levels) {
    for (int i = 0; i < levels; ++i) {
      bids[(100'000 - 2 * i) * kTick] = qty();
      asks[(100'002 + 2 * i) * kTick] = qty();
    }
  }
  std::int64_t qty() { return static_cast<std::int64_t>(1 + rng() % 50) * 100'000; }
  std::uint64_t below(std::uint64_t n) { return rng() % n; }

  // GET /depth?limit=`limit`: the best levels of each side.
  Msg snapshot(std::size_t limit) {
    std::vector<Level> b;
    std::vector<Level> a;
    for (const auto& [p, q] : bids) {
      if (b.size() == limit) break;
      b.push_back(Level{Price::from_raw(p), Qty::from_raw(q)});
    }
    for (const auto& [p, q] : asks) {
      if (a.size() == limit) break;
      a.push_back(Level{Price::from_raw(p), Qty::from_raw(q)});
    }
    Msg m = make_msg(b, a, true);
    BookDeltaMsg& d = as_msg(m);
    d.first_update_id = d.last_update_id = seq;
    d.hdr.venue_seq = seq;
    d.hdr.exch_ts = Timestamp{ts};
    return m;
  }

  // A few level changes, mostly near the touch, some deep (beyond any snapshot's depth): a price
  // with a level loses it half the time and changes size otherwise, one without gets a level, and
  // now and then the spread narrows. The depth stays about where it started.
  Msg delta() {
    std::vector<Level> b;
    std::vector<Level> a;
    const std::uint64_t n = 1 + below(6);
    for (std::uint64_t k = 0; k < n; ++k) {
      const bool bid = below(2) == 0;
      const std::int64_t best_bid = bids.begin()->first;
      const std::int64_t best_ask = asks.begin()->first;
      const std::uint64_t r = below(100);
      std::int64_t d = 0;
      if (r < 10 && best_ask - best_bid > kTick) {
        d = -1;  // inside the spread
      } else if (r < 75) {
        d = static_cast<std::int64_t>(below(30));
      } else if (r < 92) {
        d = static_cast<std::int64_t>(below(400));
      } else {
        d = static_cast<std::int64_t>(below(4000));
      }
      const std::int64_t px = bid ? best_bid - d * kTick : best_ask + d * kTick;
      std::int64_t q = qty();
      const bool exists = bid ? bids.contains(px) : asks.contains(px);
      if (exists && below(2) == 0 && (bid ? bids.size() : asks.size()) > 2) q = 0;
      if (bid) {
        if (q == 0) {
          bids.erase(px);
        } else {
          bids[px] = q;
        }
        b.push_back(Level{Price::from_raw(px), Qty::from_raw(q)});
      } else {
        if (q == 0) {
          asks.erase(px);
        } else {
          asks[px] = q;
        }
        a.push_back(Level{Price::from_raw(px), Qty::from_raw(q)});
      }
    }
    Msg m = make_msg(b, a, false);
    BookDeltaMsg& d = as_msg(m);
    d.first_update_id = seq + 1;
    seq += n;
    d.last_update_id = seq;
    d.hdr.venue_seq = seq;
    ts += 1'000'000;
    d.hdr.exch_ts = Timestamp{ts};
    return m;
  }
};

std::string differs(const EngineBook& x, const EngineBook& y) {
  for (const Side s : {Side::Buy, Side::Sell}) {
    const auto& a = x.raw(s);
    const auto& b = y.raw(s);
    if (a.size() != b.size())
      return "depth " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].price != b[i].price || a[i].qty != b[i].qty)
        return "level " + std::to_string(i) + " of side " + std::to_string(static_cast<int>(s));
    }
    if (x.truncated(s) != y.truncated(s)) return "truncated";
  }
  if (x.seq() != y.seq()) return "seq";
  if (x.last_update() != y.last_update()) return "last update";
  if (x.has_snapshot() != y.has_snapshot() || x.is_valid() != y.is_valid()) return "validity";
  return {};
}

InstrumentTable one_instrument() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = kVenue;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_raw(kTick);
  i.lot = Qty::from_raw(100'000);
  REQUIRE(t.add(i));
  return t;
}

}  // namespace

TEST_CASE("core.book_snapshot: the message is the connectors' snapshot of the book") {
  EngineBook b;
  Venue v(10);
  b.apply_delta(as_msg(v.snapshot(5)));
  b.apply_delta(as_msg(v.delta()));
  Msg out(kMaxMsgBytes / 8);
  const Timestamp now{123};
  const BookDeltaMsg& d =
      write_book_snapshot(b, kInst, kVenue, now, reinterpret_cast<std::byte*>(out.data()));
  CHECK(d.hdr.type == EventType::BookSnapshot);
  CHECK(d.is_snapshot());
  CHECK(d.hdr.len == book_snapshot_size(b));
  CHECK(d.hdr.len % 64 == 0);
  CHECK(d.hdr.instrument == kInst);
  CHECK(d.hdr.venue == kVenue);
  CHECK(d.last_update_id == b.seq());
  CHECK(d.first_update_id == b.seq());
  CHECK(d.hdr.venue_seq == b.seq());
  CHECK(d.hdr.exch_ts == b.last_update());
  CHECK(d.hdr.recv_ts == now);
  CHECK(d.hdr.t0_cycles.v == 0);
  REQUIRE(d.bid_count == b.depth(Side::Buy));
  REQUIRE(d.ask_count == b.depth(Side::Sell));
  CHECK(d.bids()[0].price == b.best_bid().price);
  CHECK(d.asks()[0].price == b.best_ask().price);
  EngineBook c;
  c.apply_delta(d);
  CHECK(differs(b, c).empty());
}

TEST_CASE(
    "core.book_snapshot: an engine book from the gateway's copy plus the deltas after it is the "
    "one from the venue's own snapshot plus the same deltas") {
  const InstrumentTable t = one_instrument();
  // The venue holds 1500 levels a side and serves 1000 of them (Binance's default limit); the
  // gateway's copy starts from that snapshot and applies every delta.
  Venue v(1500);
  AccountBook gw(t, kVenue);
  REQUIRE(gw.book(kInst) != nullptr);
  CHECK(gw.on_book(as_msg(v.snapshot(1000))));
  std::vector<Msg> deltas;
  constexpr std::size_t kDeltas = 6000;
  deltas.reserve(kDeltas);
  for (std::size_t i = 0; i < kDeltas; ++i) deltas.push_back(v.delta());

  // Strategies attach after k deltas: one from the gateway's snapshot, one from a venue snapshot
  // taken at the same point, then both apply the rest.
  const std::vector<std::size_t> attach_at = {0, 1, 17, 500, 2500, 5999};
  std::size_t next = 0;
  Venue replay(1500);  // the same venue, replayed to take its snapshot at k
  Msg out(kMaxMsgBytes / 8);
  for (std::size_t k = 0; k <= kDeltas && next < attach_at.size(); ++k) {
    if (k == attach_at[next]) {
      ++next;
      const AccountBook::Book& copy = *gw.book(kInst);
      REQUIRE(copy.has_snapshot());
      EngineBook from_gw;
      from_gw.apply_delta(write_book_snapshot(
          copy, kInst, kVenue, Timestamp{1}, reinterpret_cast<std::byte*>(out.data())));
      EngineBook from_venue;
      from_venue.apply_delta(as_msg(replay.snapshot(1000)));
      INFO("attached after " << k << " deltas");
      INFO("gateway depth " << copy.depth(Side::Buy) << "/" << copy.depth(Side::Sell) << ": "
                            << differs(from_gw, from_venue));
      REQUIRE(differs(from_gw, from_venue).empty());
      for (std::size_t j = k; j < kDeltas; ++j) {
        from_gw.apply_delta(as_msg(deltas[j]));
        from_venue.apply_delta(as_msg(deltas[j]));
        const std::string why = differs(from_gw, from_venue);
        INFO("delta " << j);
        REQUIRE(why.empty());
      }
      CHECK(from_gw.is_valid());
    }
    if (k < kDeltas) {
      gw.on_book(as_msg(deltas[k]));
      static_cast<void>(replay.delta());
    }
  }
  CHECK(next == attach_at.size());
}
