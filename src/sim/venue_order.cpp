#include "fastmm/sim/venue_order.hpp"

#include <algorithm>
#include <cstring>

namespace fastmm::sim {

std::uint32_t VenueOrderSource::take_slot() {
  if (!free_.empty()) {
    const std::uint32_t s = free_.back();
    free_.pop_back();
    return s;
  }
  slots_.emplace_back();
  return static_cast<std::uint32_t>(slots_.size() - 1);
}

const EventHeader* VenueOrderSource::next() {
  if (out_ != kNoSlot) {
    free_.push_back(out_);
    out_ = kNoSlot;
  }
  // Read until the earliest event held cannot be preceded by one still unread: every unread event
  // was received at or after read_bound_, so its venue time is at least read_bound_ - window.
  // (At most 2^kSlotBits events are held; past that the earliest goes out.)
  while (!done_ && (heap_.empty() || read_bound_ - window_.ns < heap_.front().venue_ns) &&
         heap_.size() < (std::size_t{1} << kSlotBits)) {
    const EventHeader* h = src_->next();
    if (h == nullptr) {
      done_ = true;
      break;
    }
    const std::uint32_t slot = take_slot();
    std::vector<std::uint64_t>& b = slots_[slot];
    const std::size_t words = (h->len + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    if (b.size() < words) b.resize(words);
    std::memcpy(b.data(), h, h->len);
    reinterpret_cast<EventHeader*>(b.data())->seq = ++seq_;
    const std::int64_t venue = h->exch_ts.valid() ? h->exch_ts.ns : h->recv_ts.ns;
    read_bound_ = std::max(read_bound_, h->recv_ts.valid() ? h->recv_ts.ns : venue);
    heap_push(Held{venue, seq_ << kSlotBits | slot});
    max_held_ = std::max(max_held_, heap_.size());
  }
  if (heap_.empty()) return nullptr;
  const Held e = heap_pop();
  if (e.venue_ns < last_venue_) ++late_;
  last_venue_ = std::max(last_venue_, e.venue_ns);
  if (e.seq() < max_out_) ++reordered_;
  max_out_ = std::max(max_out_, e.seq());
  out_ = e.slot();
  // The next event out is most likely the new earliest, held since long enough to be out of
  // cache: start loading it while the engine works on this one.
  if (!heap_.empty()) {
    const std::uint64_t* next = slots_[heap_.front().slot()].data();
    __builtin_prefetch(next);
    __builtin_prefetch(next + 8);
  }
  return reinterpret_cast<const EventHeader*>(slots_[out_].data());
}

void VenueOrderSource::heap_push(Held e) noexcept {
  std::size_t i = heap_.size();
  heap_.push_back(e);
  Held* const h = heap_.data();
  while (i > 0) {
    const std::size_t p = (i - 1) / 4;
    if (!before(e, h[p])) break;
    h[i] = h[p];
    i = p;
  }
  h[i] = e;
}

VenueOrderSource::Held VenueOrderSource::heap_pop() noexcept {
  Held* const h = heap_.data();
  const Held top = h[0];
  const Held last = heap_.back();
  heap_.pop_back();
  const std::size_t n = heap_.size();
  if (n == 0) return top;
  std::size_t i = 0;
  for (;;) {
    const std::size_t c = 4 * i + 1;
    if (c >= n) break;
    std::size_t m = c;
    const std::size_t end = std::min(c + 4, n);
    for (std::size_t k = c + 1; k < end; ++k) {
      if (before(h[k], h[m])) m = k;
    }
    if (!before(h[m], last)) break;
    h[i] = h[m];
    i = m;
  }
  h[i] = last;
  return top;
}

void VenueOrderSource::reset() {
  src_->reset();
  heap_.clear();
  free_.clear();
  for (std::uint32_t i = 0; i < slots_.size(); ++i) free_.push_back(i);
  out_ = kNoSlot;
  seq_ = 0;
  max_out_ = 0;
  read_bound_ = 0;
  last_venue_ = 0;
  done_ = false;
  late_ = 0;
  reordered_ = 0;
}

std::string VenueOrderSource::note() const {
  return src_->note();
}

}  // namespace fastmm::sim
