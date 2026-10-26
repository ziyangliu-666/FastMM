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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::sim {

class VenueOrderSource final : public MdSource {
 public:
  VenueOrderSource(MdSource& source, Duration window) : src_(&source), window_(window) {}

  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] const RecordedSession* recorded() const override { return src_->recorded(); }
  [[nodiscard]] Timestamp start_ts() const override { return src_->start_ts(); }
  [[nodiscard]] std::string note() const override;

  // Events that came out after one stamped later: received more than `window` after their venue
  // time, or in a source that is not in receive order.
  [[nodiscard]] std::uint64_t late() const noexcept { return late_; }
  // Events yielded after an event recorded later than them.
  [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }
  [[nodiscard]] std::size_t max_held() const noexcept { return max_held_; }

 private:
  // One held event: 16 bytes, so a node's four children share a cache line. `key` is the recorded
  // position (high kSlotBits..63) and the slot (low kSlotBits): ordering by key orders by position.
  struct Held {
    std::int64_t venue_ns;
    std::uint64_t key;
    [[nodiscard]] std::uint64_t seq() const noexcept { return key >> kSlotBits; }
    [[nodiscard]] std::uint32_t slot() const noexcept {
      return static_cast<std::uint32_t>(key & ((1ULL << kSlotBits) - 1));
    }
  };
  static constexpr unsigned kSlotBits = 24;  // 16 M events held; 2^40 events read
  // Earliest venue time, then recorded order.
  [[nodiscard]] static bool before(const Held& a, const Held& b) noexcept {
    return a.venue_ns != b.venue_ns ? a.venue_ns < b.venue_ns : a.key < b.key;
  }
  // A 4-ary min-heap on before(): half the depth of a binary one, its children in one line.
  void heap_push(Held e) noexcept;
  Held heap_pop() noexcept;
  std::uint32_t take_slot();

  MdSource* src_;
  Duration window_;
  // Copies of the events held, in 8-byte words: slot k at arena_[k * kSlotWords], or in big_[k]
  // when the event is longer than a slot (a snapshot can be larger than EventBuf). A slot's address
  // takes no load, so the next event out can be prefetched; freed slots are reused last in, first
  // out, while they are still in cache.
  static constexpr std::size_t kSlotWords = 32;
  [[nodiscard]] std::uint64_t* slot_data(std::uint32_t k) noexcept {
    return is_big_[k] != 0 ? big_[k].data() : &arena_[k * kSlotWords];
  }
  std::vector<std::uint64_t> arena_;
  std::vector<std::vector<std::uint64_t>> big_;
  std::vector<std::uint8_t> is_big_;
  std::uint32_t n_slots_ = 0;
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

// The other half: events the venue took in its own order go on to the engine in recorded order. An
// event at recorded position `seq` waits until every earlier position has gone (the venue applied
// it already); put() hands `forward` the event and then every waiting one it releases, in order.
// SimTransport keeps one for its wires; a multiverse keeps one for the market data its runs share.
class RecordedOrder {
 public:
  // `md` (null: nothing to forward, the position only) at recorded position `seq` (from 1), its
  // recorded time `recorded`, taken in by the venue at `now`. F(const EventHeader&, Timestamp
  // recorded, Timestamp venue).
  template <class F>
  void put(
      const EventHeader* md, std::uint64_t seq, Timestamp recorded, Timestamp now, F&& forward) {
    const auto after = [](const Waiting& a, const Waiting& b) { return a.seq > b.seq; };
    if (seq > next_) {
      if (seq - next_ >= ahead_.size() && ahead_.size() < kMaxAhead) grow_ahead(seq);
      if (seq - next_ < ahead_.size()) {
        Ahead& a = ahead_[seq & (ahead_.size() - 1)];
        if (a.seq == 0) {
          a.seq = seq;
          a.recorded = recorded;
          a.venue = now;
          a.has_md = md != nullptr;
          if (md != nullptr) copy(*md, a.bytes);
          return;
        }
      }
      std::uint32_t slot = kNoSlot;
      if (md != nullptr) {
        if (waiting_free_.empty()) {
          waiting_bytes_.emplace_back();
          slot = static_cast<std::uint32_t>(waiting_bytes_.size() - 1);
        } else {
          slot = waiting_free_.back();
          waiting_free_.pop_back();
        }
        copy(*md, waiting_bytes_[slot]);
      }
      waiting_.push_back(Waiting{seq, recorded, now, slot});
      std::push_heap(waiting_.begin(), waiting_.end(), after);
      return;
    }
    if (md != nullptr) forward(*md, recorded, now);
    if (seq == next_) ++next_;
    // Every position waiting is past next_ until it moves on: the next to go is at it.
    for (;;) {
      if (!ahead_.empty()) {
        Ahead& a = ahead_[next_ & (ahead_.size() - 1)];
        if (a.seq == next_) {
          a.seq = 0;
          if (a.has_md)
            forward(*reinterpret_cast<const EventHeader*>(a.bytes.data()), a.recorded, a.venue);
          ++next_;
          continue;
        }
      }
      if (waiting_.empty() || waiting_.front().seq > next_) break;
      std::pop_heap(waiting_.begin(), waiting_.end(), after);
      const Waiting w = waiting_.back();
      waiting_.pop_back();
      if (w.slot != kNoSlot) {
        forward(*reinterpret_cast<const EventHeader*>(waiting_bytes_[w.slot].data()),
                w.recorded,
                w.venue);
        waiting_free_.push_back(w.slot);
      }
      if (w.seq == next_) ++next_;
    }
  }

 private:
  // A position less than kMaxAhead past next_ waits in `ahead_`, a ring indexed by the position
  // (VenueOrderSource's are dense, the next one to go is found in one load); a farther or repeated
  // one in `waiting_`, a min-heap on the position, its bytes in `waiting_bytes_`.
  struct Waiting {
    std::uint64_t seq;
    Timestamp recorded;
    Timestamp venue;
    std::uint32_t slot;  // kNoSlot: nothing to forward
  };
  struct Ahead {
    std::uint64_t seq = 0;  // 0: free
    Timestamp recorded;
    Timestamp venue;
    bool has_md = false;
    std::vector<std::uint64_t> bytes;
  };
  static constexpr std::uint32_t kNoSlot = 0xFFFF'FFFF;
  static constexpr std::size_t kMaxAhead = std::size_t{1} << 20;

  static void copy(const EventHeader& md, std::vector<std::uint64_t>& to) {
    const std::size_t words = (md.len + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    if (to.size() < words) to.resize(words);
    std::memcpy(to.data(), &md, md.len);
  }
  // Makes `ahead_` hold positions up to `seq` (a power of two size, at most kMaxAhead).
  void grow_ahead(std::uint64_t seq) {
    std::size_t cap = ahead_.empty() ? 1024 : ahead_.size();
    while (cap < kMaxAhead && seq - next_ >= cap) cap *= 2;
    if (cap == ahead_.size()) return;
    std::vector<Ahead> next(cap);
    for (Ahead& a : ahead_) {
      if (a.seq != 0) next[a.seq & (cap - 1)] = std::move(a);
    }
    ahead_ = std::move(next);
  }

  std::vector<Ahead> ahead_;
  std::vector<Waiting> waiting_;
  std::vector<std::vector<std::uint64_t>> waiting_bytes_;
  std::vector<std::uint32_t> waiting_free_;
  std::uint64_t next_ = 1;
};

}  // namespace fastmm::sim
