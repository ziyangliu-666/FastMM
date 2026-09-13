#include "fastmm/backtest/synthetic_source.hpp"

#include <cstring>

namespace fastmm::bt {

SyntheticSource::SyntheticSource(const SyntheticSourceConfig& cfg)
    : cfg_(cfg), queue_(1U << 20), end_(cfg.start + cfg.duration) {
  rebuild();
}

void SyntheticSource::rebuild() {
  // The sink is a private base: convert here, where the base is accessible.
  sim::MatchingSink* sink = this;
  me_ = std::make_unique<sim::MatchingEngine>(static_cast<std::size_t>(cfg_.instrument.value) + 1,
                                              sink);
  sim::MdAggregatorConfig mc = cfg_.md;
  mc.venue = cfg_.venue;
  agg_ = std::make_unique<sim::MdAggregator>(
      static_cast<std::size_t>(cfg_.instrument.value) + 1, *me_, mc, cfg_.start);
  gen_ = std::make_unique<sim::MarketGenerator>(
      cfg_.generator, cfg_.seed, cfg_.instrument, cfg_.start, end_);
  gen_->seed_book(*me_, cfg_.seed_levels, cfg_.start);
  agg_->flush(cfg_.start, &SyntheticSource::emit, this);  // initial snapshot
  events_ = 0;
  done_ = false;
}

void SyntheticSource::reset() {
  // Drain anything queued and start over from the same seed: identical stream.
  while (queue_.try_peek() != nullptr) queue_.release();
  rebuild();
}

void SyntheticSource::on_book_change(InstrumentId id, Side s, Price p, Qty q, std::uint64_t u) {
  agg_->on_book_change(id, s, p, q, u);
}

void SyntheticSource::on_trade(
    InstrumentId id, Price p, Qty q, Side aggr, std::uint64_t tid, Timestamp ts) {
  TradeMsg t{};
  init_header(t, EventType::Trade, id, cfg_.venue);
  t.price = p;
  t.qty = q;
  t.trade_id = tid;
  t.aggressor = aggr;
  t.hdr.venue_seq = tid;
  push(t.hdr, ts);
}

void SyntheticSource::emit(void* ctx, EventHeader& h, Timestamp ts) noexcept {
  static_cast<SyntheticSource*>(ctx)->push(h, ts);
}

void SyntheticSource::push(EventHeader& h, Timestamp ts) noexcept {
  h.exch_ts = h.recv_ts = ts;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(ts.ns)};
  h.seq = 0;
  // 1 MiB queue, drained after every generator action / flush (a few messages each).
  if (!queue_.try_push(&h, h.len)) ++dropped_;
}

bool SyntheticSource::produce() {
  while (queue_.try_peek() == nullptr) {
    if (done_) return false;
    const Timestamp tg = gen_->next_ts();
    const Timestamp tf = agg_->next_flush_ts();
    if (tg == Timestamp::max() && tf > end_) {
      done_ = true;
      return false;
    }
    if (tg <= tf) {
      gen_->step(*me_);
    } else {
      agg_->flush(tf, &SyntheticSource::emit, this);
    }
  }
  return true;
}

const EventHeader* SyntheticSource::next() {
  if (!produce()) return nullptr;
  const std::byte* p = queue_.try_peek();
  const auto* h = reinterpret_cast<const EventHeader*>(p);
  std::memcpy(buf_.bytes, p, h->len);
  queue_.release();
  ++events_;
  return &buf_.hdr();
}

}  // namespace fastmm::bt
