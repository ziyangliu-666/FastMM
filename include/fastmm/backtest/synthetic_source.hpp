#pragma once
// SyntheticSource: a MarketGenerator run against its own MatchingEngine, published through
// MdAggregator as a plain market-data stream (snapshot, 100 ms depth batches, trades, book
// tickers). Same seed => byte-identical stream. Use it to record fixtures / CSVs or to
// backtest with the L2 queue model; the coupled mode (strategy orders in the same book)
// is BacktestRunner's default for `source = "synthetic"` with the matching fill model.
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/matching_engine.hpp"
#include "fastmm/sim/md_aggregator.hpp"

#include <cstdint>
#include <memory>

namespace fastmm::bt {

struct SyntheticSourceConfig {
  sim::MarketGeneratorParams generator{};
  sim::MdAggregatorConfig md{};
  std::uint64_t seed = 42;
  InstrumentId instrument{0};
  VenueId venue{0};
  Timestamp start{seconds(1'700'000'000).ns};
  Duration duration = seconds(60);
  int seed_levels = 20;
};

class SyntheticSource final : public MdSource, private sim::MatchingSink {
 public:
  explicit SyntheticSource(const SyntheticSourceConfig& cfg);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return cfg_.start; }
  [[nodiscard]] const sim::MarketGeneratorStats& generator_stats() const noexcept {
    return gen_->stats();
  }
  [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }  // should be 0

 private:
  void on_book_change(InstrumentId, Side, Price, Qty, std::uint64_t) override;
  void on_trade(InstrumentId, Price, Qty, Side, std::uint64_t, Timestamp) override;
  static void emit(void* ctx, EventHeader& h, Timestamp ts) noexcept;
  void push(EventHeader& h, Timestamp ts) noexcept;
  void rebuild();
  bool produce();  // advances the world until at least one message is queued

  SyntheticSourceConfig cfg_;
  std::unique_ptr<sim::MatchingEngine> me_;
  std::unique_ptr<sim::MdAggregator> agg_;
  std::unique_ptr<sim::MarketGenerator> gen_;
  MsgRing queue_;
  EventBuf buf_{};
  Timestamp end_{};
  std::uint64_t events_ = 0;
  std::uint64_t dropped_ = 0;
  bool done_ = false;
};

}  // namespace fastmm::bt
