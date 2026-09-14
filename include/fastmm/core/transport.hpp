#pragma once
// The two seams that make the same Engine template run live, in the simulator and in
// replay (5.11):
//
//   FeedLike       - where consumed events come from   (RingFeed / InlineFeed / JournalFeed)
//   TransportLike  - where outbound messages go        (LiveTransport / SimTransport / Replay)
//
// ClockLike lives in time.hpp. All three are concepts; no virtual calls in the loop.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fastmm {

inline constexpr std::size_t kMaxVenues = 8;
inline constexpr std::size_t kMaxFeedRings = 16;
inline constexpr std::uint32_t kFeedBudgetPerRing = 64;  // anti-starvation (5.1)

template <class F>
concept FeedLike = requires(F& f) {
  { f.next() } noexcept -> std::same_as<const EventHeader*>;  // nullptr when nothing is ready
  { f.release() } noexcept;                                   // done with the last next()
};

template <class T>
concept TransportLike =
    requires(T& t, const EventHeader& m, std::span<const EventHeader* const> batch, VenueId v) {
      { t.send(m) } noexcept -> std::same_as<bool>;  // one Out*Msg; false == queue full
      // Accepts a prefix of the batch and returns its length (the rest was not sent).
      { t.send(batch) } noexcept -> std::same_as<std::size_t>;
      { t.supports_replace(v) } noexcept -> std::same_as<bool>;
    };

// Polls N inbound MsgRings round-robin, at most kFeedBudgetPerRing messages per ring per
// visit so a chatty venue cannot starve the others. Consumption order is the canonical
// order that the journal records.
class RingFeed {
 public:
  RingFeed() noexcept = default;
  bool add_ring(MsgRing* ring) noexcept {
    if (count_ >= kMaxFeedRings || ring == nullptr) return false;
    rings_[count_++] = ring;
    return true;
  }
  [[nodiscard]] std::size_t ring_count() const noexcept { return count_; }

  [[nodiscard]] const EventHeader* next() noexcept {
    if (count_ == 0) return nullptr;
    for (std::size_t visited = 0; visited < count_; ++visited) {
      if (budget_ == 0) advance();
      const std::byte* p = rings_[cur_]->try_peek();
      if (p != nullptr) return reinterpret_cast<const EventHeader*>(p);
      advance();
    }
    return nullptr;
  }
  void release() noexcept {
    rings_[cur_]->release();
    --budget_;
  }

 private:
  void advance() noexcept {
    cur_ = (cur_ + 1) % count_;
    budget_ = kFeedBudgetPerRing;
  }
  MsgRing* rings_[kMaxFeedRings] = {};
  std::size_t count_ = 0;
  std::size_t cur_ = 0;
  std::uint32_t budget_ = kFeedBudgetPerRing;
};

// Single-threaded queue the simulator/backtester pushes events into before calling
// Engine::step(). Backed by an owned MsgRing (no allocation after construction).
class InlineFeed {
 public:
  explicit InlineFeed(std::size_t bytes = 1U << 22) : ring_(bytes) {}
  // Copies the message; false if the queue is full.
  [[nodiscard]] bool push(const EventHeader& m) noexcept { return ring_.try_push(&m, m.len); }
  // In-place construction: reserve, fill, commit.
  [[nodiscard]] std::byte* reserve(std::uint32_t len) noexcept { return ring_.try_reserve(len); }
  void commit() noexcept { ring_.commit(); }
  [[nodiscard]] const EventHeader* next() noexcept {
    return reinterpret_cast<const EventHeader*>(ring_.try_peek());
  }
  void release() noexcept { ring_.release(); }
  [[nodiscard]] bool empty() const noexcept { return ring_.empty_approx(); }
  [[nodiscard]] MsgRing& ring() noexcept { return ring_; }

 private:
  MsgRing ring_;
};

// Writes Out*Msg into the per-venue outbound MsgRing consumed by that venue's net thread,
// then optionally pokes a wake hook (the net layer registers an eventfd writer) so a
// reactor blocked in epoll_wait picks it up immediately.
class LiveTransport {
 public:
  using WakeFn = void (*)(void* ctx, VenueId venue) noexcept;

  LiveTransport() noexcept = default;

  bool set_venue(VenueId v, MsgRing* ring, bool supports_replace) noexcept {
    if (v.value >= kMaxVenues || ring == nullptr) return false;
    rings_[v.value] = ring;
    replace_[v.value] = supports_replace;
    return true;
  }
  void set_wake_hook(WakeFn fn, void* ctx) noexcept {
    wake_ = fn;
    wake_ctx_ = ctx;
  }

  [[nodiscard]] bool send(const EventHeader& m) noexcept {
    MsgRing* r = ring_for(m.venue);
    if (FASTMM_UNLIKELY(r == nullptr)) return false;
    if (FASTMM_UNLIKELY(!r->try_push(&m, m.len))) {
      ++full_;
      return false;
    }
    ++sent_;
    if (wake_ != nullptr) wake_(wake_ctx_, m.venue);
    return true;
  }
  // Batch: all messages are enqueued first, then each touched venue is woken once. Stops at the
  // first message a ring refuses, so the accepted messages are a prefix of the batch.
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    std::size_t ok = 0;
    std::uint32_t touched = 0;
    for (const EventHeader* m : batch) {
      MsgRing* r = ring_for(m->venue);
      if (r == nullptr || !r->try_push(m, m->len)) {
        full_ += batch.size() - ok;
        break;
      }
      ++ok;
      touched |= 1U << m->venue.value;
    }
    sent_ += ok;
    if (wake_ != nullptr) {
      for (std::uint32_t v = 0; v < kMaxVenues; ++v) {
        if ((touched & (1U << v)) != 0) wake_(wake_ctx_, VenueId{static_cast<std::uint8_t>(v)});
      }
    }
    return ok;
  }
  [[nodiscard]] bool supports_replace(VenueId v) const noexcept {
    return v.value < kMaxVenues && replace_[v.value];
  }
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] std::uint64_t dropped_full() const noexcept { return full_; }

 private:
  [[nodiscard]] MsgRing* ring_for(VenueId v) const noexcept {
    return v.value < kMaxVenues ? rings_[v.value] : nullptr;
  }
  MsgRing* rings_[kMaxVenues] = {};
  bool replace_[kMaxVenues] = {};
  WakeFn wake_ = nullptr;
  void* wake_ctx_ = nullptr;
  std::uint64_t sent_ = 0;
  std::uint64_t full_ = 0;
};

static_assert(FeedLike<RingFeed> && FeedLike<InlineFeed>);
static_assert(TransportLike<LiveTransport>);

}  // namespace fastmm
