#pragma once
// A random L2 market for differential tests: several instruments on one or more venues, each a
// random walk of its touch with depth updates (now and then a snapshot, a crossed or one-sided
// book), book tickers, and trades at or through the touch. Each event reaches the "recorder" up to
// `max_delay` after its venue time, so the recording is not in venue-time order (the reorder window
// has work to do); it is yielded in receive order, as a journal would be.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/sim/md_source.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace fastmm::bt::testutil {

struct RandomMarketConfig {
  std::uint64_t seed = 1;
  std::int64_t start_ns = 1'789'344'931'000'000'000LL;
  std::size_t events = 20'000;
  std::vector<VenueId> venues{VenueId{0}};  // per instrument
  std::int64_t tick_raw = 1'000'000;        // 0.01
  std::int64_t start_ticks = 10'000;
  bool tickers = true;
  std::int64_t max_delay_ns = 2'000'000;
  std::int64_t lot_raw = 100'000'000;  // 1
};

class RandomMarket final : public sim::MdSource {
 public:
  explicit RandomMarket(RandomMarketConfig cfg) : cfg_(std::move(cfg)) { generate(); }

  const EventHeader* next() override {
    if (at_ >= order_.size()) return nullptr;
    const std::vector<std::uint64_t>& e = events_[order_[at_++]];
    std::memcpy(&buf_, e.data(), e.size() * sizeof(std::uint64_t));
    return reinterpret_cast<const EventHeader*>(&buf_);
  }
  void reset() override { at_ = 0; }
  [[nodiscard]] Timestamp start_ts() const override { return Timestamp{cfg_.start_ns}; }

 private:
  struct Book {
    std::map<std::int64_t, std::int64_t> bids;  // ticks -> lots
    std::map<std::int64_t, std::int64_t> asks;
    std::int64_t mid = 0;  // ticks, between the touches
    std::uint64_t update_id = 0;
  };

  void push(EventHeader& h, std::int64_t venue_ns) {
    h.exch_ts = Timestamp{venue_ns};
    h.recv_ts = Timestamp{venue_ns + static_cast<std::int64_t>(rng_.uniform(
                                         static_cast<std::uint64_t>(cfg_.max_delay_ns) + 1))};
    std::vector<std::uint64_t> words((h.len + 7) / 8);
    std::memcpy(words.data(), &h, h.len);
    events_.push_back(std::move(words));
  }

  void snapshot(std::uint32_t i, std::int64_t ts, int shape) {
    Book& b = books_[i];
    b.bids.clear();
    b.asks.clear();
    for (int k = 0; k < 8; ++k) {
      b.bids[b.mid - 1 - k] = 1 + static_cast<std::int64_t>(rng_.uniform(9));
      b.asks[b.mid + 1 + k] = 1 + static_cast<std::int64_t>(rng_.uniform(9));
    }
    if (shape == 1) b.bids[b.mid + 2] = 3;  // crossed
    if (shape == 2) b.asks.clear();         // one-sided
    alignas(64) std::byte raw[BookDeltaMsg::size_for(32, 32)];
    auto& d = *reinterpret_cast<BookDeltaMsg*>(raw);
    const auto nb = static_cast<std::uint32_t>(b.bids.size());
    const auto na = static_cast<std::uint32_t>(b.asks.size());
    init_header(d,
                EventType::BookSnapshot,
                InstrumentId{i},
                cfg_.venues[i],
                BookDeltaMsg::size_for(nb, na));
    d.hdr.flags |= EventHeader::kSnapshot;
    d.bid_count = nb;
    d.ask_count = na;
    d.first_update_id = d.last_update_id = ++b.update_id;
    std::uint32_t n = 0;
    for (auto it = b.bids.rbegin(); it != b.bids.rend(); ++it)
      d.levels()[n++] = level(it->first, it->second);
    for (auto it = b.asks.begin(); it != b.asks.end(); ++it)
      d.levels()[n++] = level(it->first, it->second);
    push(d.hdr, ts);
    if (shape != 0) snapshot(i, ts + 1'000, 0);  // and back to a valid book
  }

  Level level(std::int64_t ticks, std::int64_t lots) const {
    return Level{Price::from_raw(ticks * cfg_.tick_raw), Qty::from_raw(lots * cfg_.lot_raw)};
  }

  void delta(std::uint32_t i, std::int64_t ts) {
    Book& b = books_[i];
    std::vector<std::pair<std::int64_t, std::int64_t>> bids;
    std::vector<std::pair<std::int64_t, std::int64_t>> asks;
    // The touch moves by a tick now and then: levels it passes go.
    if (rng_.uniform(4) == 0) {
      const std::int64_t step = rng_.uniform(2) == 0 ? -1 : 1;
      b.mid += step;
      for (auto it = b.bids.begin(); it != b.bids.end();) {
        if (it->first >= b.mid) {
          bids.emplace_back(it->first, 0);
          it = b.bids.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = b.asks.begin(); it != b.asks.end();) {
        if (it->first <= b.mid) {
          asks.emplace_back(it->first, 0);
          it = b.asks.erase(it);
        } else {
          ++it;
        }
      }
    }
    const int changes = 1 + static_cast<int>(rng_.uniform(3));
    for (int k = 0; k < changes; ++k) {
      const bool bid = rng_.uniform(2) == 0;
      const std::int64_t off = 1 + static_cast<std::int64_t>(rng_.uniform(6));
      const std::int64_t px = bid ? b.mid - off : b.mid + off;
      const std::int64_t q =
          rng_.uniform(5) == 0 ? 0 : 1 + static_cast<std::int64_t>(rng_.uniform(12));
      auto& side = bid ? b.bids : b.asks;
      if (q == 0) {
        side.erase(px);
      } else {
        side[px] = q;
      }
      (bid ? bids : asks).emplace_back(px, q);
    }
    if (b.bids.empty()) {
      b.bids[b.mid - 1] = 2;
      bids.emplace_back(b.mid - 1, 2);
    }
    if (b.asks.empty()) {
      b.asks[b.mid + 1] = 2;
      asks.emplace_back(b.mid + 1, 2);
    }
    alignas(64) std::byte raw[BookDeltaMsg::size_for(32, 32)];
    auto& d = *reinterpret_cast<BookDeltaMsg*>(raw);
    const auto nb = static_cast<std::uint32_t>(std::min<std::size_t>(bids.size(), 32));
    const auto na = static_cast<std::uint32_t>(std::min<std::size_t>(asks.size(), 32));
    init_header(
        d, EventType::BookDelta, InstrumentId{i}, cfg_.venues[i], BookDeltaMsg::size_for(nb, na));
    d.bid_count = nb;
    d.ask_count = na;
    d.first_update_id = b.update_id + 1;
    d.last_update_id = ++b.update_id;
    d.prev_update_id = b.update_id - 1;
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < nb; ++k) d.levels()[n++] = level(bids[k].first, bids[k].second);
    for (std::uint32_t k = 0; k < na; ++k) d.levels()[n++] = level(asks[k].first, asks[k].second);
    push(d.hdr, ts);
  }

  void ticker(std::uint32_t i, std::int64_t ts) {
    const Book& b = books_[i];
    if (b.bids.empty() || b.asks.empty()) return;
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, InstrumentId{i}, cfg_.venues[i]);
    m.bid_px = level(b.bids.rbegin()->first, 0).price;
    m.bid_qty = level(0, b.bids.rbegin()->second).qty;
    m.ask_px = level(b.asks.begin()->first, 0).price;
    m.ask_qty = level(0, b.asks.begin()->second).qty;
    m.hdr.venue_seq = b.update_id + rng_.uniform(2);
    push(m.hdr, ts);
  }

  void trade(std::uint32_t i, std::int64_t ts) {
    const Book& b = books_[i];
    if (b.bids.empty() || b.asks.empty()) return;
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{i}, cfg_.venues[i]);
    const bool buy = rng_.uniform(2) == 0;
    const std::int64_t through = rng_.uniform(4) == 0 ? 1 : 0;
    t.aggressor = buy ? Side::Buy : Side::Sell;
    t.price =
        level(buy ? b.asks.begin()->first + through : b.bids.rbegin()->first - through, 0).price;
    t.qty = level(0, 1 + static_cast<std::int64_t>(rng_.uniform(15))).qty;
    t.trade_id = ++trade_id_;
    push(t.hdr, ts);
  }

  void generate() {
    rng_ = Xoshiro256ss(cfg_.seed);
    const std::size_t n = cfg_.venues.size();
    books_.assign(n, Book{});
    std::int64_t t = cfg_.start_ns;
    for (std::uint32_t i = 0; i < n; ++i) {
      books_[i].mid = cfg_.start_ticks + static_cast<std::int64_t>(i) * 37;
      snapshot(i, t, 0);
    }
    while (events_.size() < cfg_.events) {
      t += 20'000 + static_cast<std::int64_t>(rng_.uniform(3'000'000));
      const auto i = static_cast<std::uint32_t>(rng_.uniform(n));
      const std::uint64_t r = rng_.uniform(1000);
      if (r < 4) {
        snapshot(i, t, 1 + static_cast<int>(rng_.uniform(2)));
      } else if (r < 30) {
        snapshot(i, t, 0);
      } else if (r < 650) {
        delta(i, t);
      } else if (r < 850) {
        trade(i, t);
      } else if (cfg_.tickers) {
        ticker(i, t);
      }
    }
    order_.resize(events_.size());
    for (std::size_t k = 0; k < order_.size(); ++k) order_[k] = k;
    std::stable_sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
      return recv(a) < recv(b);
    });
  }
  [[nodiscard]] std::int64_t recv(std::size_t k) const noexcept {
    return reinterpret_cast<const EventHeader*>(events_[k].data())->recv_ts.ns;
  }

  RandomMarketConfig cfg_;
  Xoshiro256ss rng_{1};
  std::vector<Book> books_;
  std::vector<std::vector<std::uint64_t>> events_;
  std::vector<std::size_t> order_;
  std::size_t at_ = 0;
  std::uint64_t trade_id_ = 0;
  EventBuf buf_{};
};

}  // namespace fastmm::bt::testutil
