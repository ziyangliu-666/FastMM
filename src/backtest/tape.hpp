#pragma once
// Tape: a recorded stream in venue order, read once for several readers on their own threads.
//
// One thread (fill()) reads the source, puts its events in venue order (sim::VenueOrderSource)
// and copies them into chunks; each reader takes the chunks at its own pace through a Cursor, an
// MdSource whose events stay valid until its next next(). A chunk is filled again once every
// reader has read it, so the tape holds at most `slots` chunks of 64 KiB, however long the data
// is, and the reading runs ahead of the slowest reader by at most that much. The threads wait on
// the counters themselves (std::atomic::wait); stop() sets kStop in each, so every wait returns:
// fill() then stops and the cursors throw. bt::run_multiverse gives the passes of a wave one tape.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"
#include "fastmm/sim/venue_order.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fastmm::bt {

class Tape {
  static constexpr std::size_t kChunkBytes = std::size_t{1} << 16;
  static_assert(kMaxMsgBytes <= kChunkBytes, "a chunk holds the longest message");
  static constexpr std::uint32_t kStop = 1U << 31;
  struct alignas(64) Block {
    std::byte bytes[64];
  };
  struct Slot {
    std::unique_ptr<Block[]> data;
    std::size_t used = 0;
    bool last = false;
    std::atomic<std::uint32_t> unread{0};  // the cursors still to read it
  };

 public:
  // 1024 chunks: 64 MiB, which 64 readers on 64 cores share without waiting on one another.
  static constexpr std::size_t kSlots = 1024;

  // `window` > 0 puts the events in venue order with that reorder window; `readers` cursors will
  // read the tape, each to its end.
  Tape(sim::MdSource& source, Duration window, std::size_t readers, std::size_t slots = kSlots)
      : source_(&source),
        recorded_(source.recorded()),
        start_(source.start_ts()),
        order_by_order_(source.order_by_order()),
        readers_(static_cast<std::uint32_t>(readers)),
        slots_(slots) {
    if (window.ns > 0) ordered_ = std::make_unique<sim::VenueOrderSource>(source, window);
    for (Slot& s : slots_) s.data = std::make_unique<Block[]>(kChunkBytes / sizeof(Block));
  }

  // The reading thread: every event, then the end; returns early once stop() is called.
  void fill() {
    sim::MdSource& in = ordered_ ? *ordered_ : *source_;
    const EventHeader* ev = in.next();
    for (std::uint32_t chunk = 0;; ++chunk) {
      Slot& s = slots_[chunk % slots_.size()];
      for (std::uint32_t u = s.unread.load(std::memory_order_acquire); u != 0;
           u = s.unread.load(std::memory_order_acquire)) {
        if ((u & kStop) != 0) return;
        s.unread.wait(u, std::memory_order_acquire);
      }
      auto* at = reinterpret_cast<std::byte*>(s.data.get());
      std::size_t used = 0;
      while (ev != nullptr && used + ev->len <= kChunkBytes) {
        std::memcpy(at + used, ev, ev->len);  // a multiple of 64: the next one stays aligned
        used += ev->len;
        ev = in.next();
      }
      if (used == 0 && ev != nullptr) throw std::length_error("tape: an event over 64 KiB");
      s.used = used;
      s.last = ev == nullptr;
      s.unread.store(readers_, std::memory_order_relaxed);
      // An add keeps kStop if stop() set it meanwhile.
      if ((filled_.fetch_add(1, std::memory_order_acq_rel) & kStop) != 0) return;
      filled_.notify_all();
      if (s.last) return;
    }
  }
  // A reader or the reading failed: whatever waits returns, and the cursors throw.
  void stop() noexcept {
    filled_.fetch_or(kStop, std::memory_order_acq_rel);
    filled_.notify_all();
    for (Slot& s : slots_) {
      s.unread.fetch_or(kStop, std::memory_order_acq_rel);
      s.unread.notify_all();
    }
  }
  // What ordering the events did (null without a window); final once a cursor read to the end.
  [[nodiscard]] const sim::VenueOrderSource* ordered() const noexcept { return ordered_.get(); }

  // One reader's reading of the tape: the source's events in venue order, once.
  class Cursor final : public sim::MdSource {
   public:
    explicit Cursor(Tape& tape) noexcept : tape_(&tape) {}
    const EventHeader* next() override {
      for (;;) {
        if (slot_ != nullptr && at_ < slot_->used) {
          const auto* h = reinterpret_cast<const EventHeader*>(
              reinterpret_cast<const std::byte*>(slot_->data.get()) + at_);
          at_ += h->len;
          return h;
        }
        if (slot_ != nullptr) {
          if (slot_->last) return nullptr;
          if (slot_->unread.fetch_sub(1, std::memory_order_acq_rel) == 1)
            slot_->unread.notify_one();
          ++chunk_;
        }
        slot_ = &tape_->await(chunk_);
        at_ = 0;
      }
    }
    void reset() override { throw std::logic_error("tape: a cursor reads once"); }
    [[nodiscard]] const sim::RecordedSession* recorded() const override { return tape_->recorded_; }
    [[nodiscard]] Timestamp start_ts() const override { return tape_->start_; }
    [[nodiscard]] bool order_by_order() const override { return tape_->order_by_order_; }

   private:
    Tape* tape_;
    Slot* slot_ = nullptr;
    std::uint32_t chunk_ = 0;
    std::size_t at_ = 0;
  };

 private:
  Slot& await(std::uint32_t chunk) {
    for (std::uint32_t f = filled_.load(std::memory_order_acquire);;
         f = filled_.load(std::memory_order_acquire)) {
      if ((f & kStop) != 0) throw std::runtime_error("tape: stopped");
      if (f > chunk) return slots_[chunk % slots_.size()];
      filled_.wait(f, std::memory_order_acquire);
    }
  }

  sim::MdSource* source_;
  std::unique_ptr<sim::VenueOrderSource> ordered_;
  const sim::RecordedSession* recorded_;
  Timestamp start_;
  bool order_by_order_;
  std::uint32_t readers_;
  std::vector<Slot> slots_;
  std::atomic<std::uint32_t> filled_{0};  // chunks written, and kStop
};

}  // namespace fastmm::bt
