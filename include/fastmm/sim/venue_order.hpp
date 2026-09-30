#pragma once
// VenueOrderSource: a recorded stream in the order the venue produced it.
//
// A recording is in receive order, and its streams (depth, trades, book ticker) reach the recorder
// with different delays, so its venue times are not monotone: a trade often arrives after a book
// ticker the venue stamped later. The simulated venue must see events in venue time, as fill-check
// does: otherwise an order that reached it in between is filled by an earlier trade, or cancelled
// before a trade the venue printed while it was still there.
//
// The adapter holds the events it has read and yields the one with the smallest venue time
// (exch_ts, or recv_ts without one; ties in recorded order) once no event still unread can be
// earlier. That uses the source's contract (non-decreasing recv_ts) and a bound on the recording's
// delay: an event is taken to have been received at most `window` after its venue time. One
// received later than that comes out after events stamped after it; late() counts them, and the
// fill model's arrival rule (core/queue_model.hpp) still keeps it from filling an order that
// arrived after it.
//
// Each event keeps its position in the recording in hdr.seq (from 1), so the simulated venue can
// forward it to the engine in recorded order (sim_transport.hpp).
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::sim {

class VenueOrderSource final : public MdSource {
 public:
  VenueOrderSource(MdSource& source, Duration window) : src_(&source), window_(window) {}

  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] const std::vector<SimAccountConfig>* balance_snapshots() const override {
    return src_->balance_snapshots();
  }
  [[nodiscard]] Timestamp start_ts() const override { return src_->start_ts(); }
  [[nodiscard]] std::string note() const override;

  // Events that came out after one stamped later: received more than `window` after their venue
  // time, or in a source that is not in receive order.
  [[nodiscard]] std::uint64_t late() const noexcept { return late_; }
  // Events yielded after an event recorded later than them.
  [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }
  [[nodiscard]] std::size_t max_held() const noexcept { return max_held_; }

 private:
  struct Held {
    std::int64_t venue_ns;
    std::uint64_t seq;
    std::uint32_t slot;
  };
  // Min-heap order: earliest venue time, then recorded order.
  static bool later(const Held& a, const Held& b) noexcept {
    return a.venue_ns != b.venue_ns ? a.venue_ns > b.venue_ns : a.seq > b.seq;
  }
  std::uint32_t take_slot();

  MdSource* src_;
  Duration window_;
  // Copies of the events held, 8-byte words (a snapshot can be larger than EventBuf).
  std::vector<std::vector<std::uint64_t>> slots_;
  std::vector<std::uint32_t> free_;
  std::vector<Held> heap_;
  static constexpr std::uint32_t kNoSlot = 0xFFFF'FFFF;
  std::uint32_t out_ = kNoSlot;  // the slot of the event last yielded
  std::uint64_t seq_ = 0;        // events read
  std::uint64_t max_out_ = 0;    // the latest recorded position yielded
  std::int64_t read_bound_ = 0;  // latest receive time read (venue time without one)
  std::int64_t last_venue_ = 0;  // latest venue time yielded
  bool done_ = false;
  std::uint64_t late_ = 0;
  std::uint64_t reordered_ = 0;
  std::size_t max_held_ = 0;
};

}  // namespace fastmm::sim
