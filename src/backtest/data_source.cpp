#include "fastmm/backtest/data_source.hpp"

#include "fastmm/core/book/l2_book.hpp"

#include <cstring>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace fastmm::bt {

namespace {
Timestamp event_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}
void stamp(EventHeader& h, Timestamp t) noexcept {
  h.exch_ts = h.recv_ts = t;
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(t.ns)};
  h.t1_delta = h.t2_delta = 0;
  h.seq = 0;
}
}  // namespace

struct TimeSliceSource::Primer {
  using Key = std::tuple<std::uint8_t, std::uint32_t>;  // venue, instrument
  std::map<Key, L2Book<256>> books;
  // Last message of every other kind: (type, venue, instrument) -> bytes.
  std::map<std::tuple<std::uint16_t, std::uint8_t, std::uint32_t>, std::vector<std::byte>> last;
  std::vector<std::vector<std::byte>> out;  // what next() yields before the slice itself
  std::size_t at = 0;
  EventBuf buf{};

  void absorb(const EventHeader& h) {
    switch (h.type) {
      case EventType::BookDelta:
      case EventType::BookSnapshot:
        books[Key{h.venue.value, h.instrument.value}].apply_delta(msg_cast<BookDeltaMsg>(&h));
        break;
      case EventType::Trade:
        break;  // history, not state
      default: {
        auto& bytes = last[{static_cast<std::uint16_t>(h.type), h.venue.value, h.instrument.value}];
        const auto* p = reinterpret_cast<const std::byte*>(&h);
        bytes.assign(p, p + h.len);
        break;
      }
    }
  }

  void build(Timestamp t0) {
    for (const auto& [key, book] : books) {
      const auto nb = static_cast<std::uint32_t>(book.depth(Side::Buy));
      const auto na = static_cast<std::uint32_t>(book.depth(Side::Sell));
      auto& d = buf.as<BookDeltaMsg>();
      init_header(d,
                  EventType::BookSnapshot,
                  InstrumentId{std::get<1>(key)},
                  VenueId{std::get<0>(key)},
                  BookDeltaMsg::size_for(nb, na));
      d.hdr.flags |= EventHeader::kSnapshot;
      stamp(d.hdr, t0);
      d.hdr.venue_seq = book.seq();
      d.bid_count = nb;
      d.ask_count = na;
      d.first_update_id = d.last_update_id = book.seq();
      d.prev_update_id = 0;
      Level* l = d.levels();
      book.for_each_level(Side::Buy, [&](const Level& x) { *l++ = x; });
      book.for_each_level(Side::Sell, [&](const Level& x) { *l++ = x; });
      const auto* p = reinterpret_cast<const std::byte*>(&d);
      out.emplace_back(p, p + d.hdr.len);
    }
    for (auto& [key, bytes] : last) {
      stamp(*reinterpret_cast<EventHeader*>(bytes.data()), t0);
      out.push_back(std::move(bytes));
    }
  }
};

TimeSliceSource::TimeSliceSource(MdSource& inner, Timestamp t0, Timestamp t1)
    : inner_(inner), t0_(t0), t1_(t1) {}

TimeSliceSource::~TimeSliceSource() = default;

const EventHeader* TimeSliceSource::next() {
  if (done_) return nullptr;
  if (!started_) {
    started_ = true;
    auto primer = std::make_unique<Primer>();
    const EventHeader* h = inner_.next();
    for (; h != nullptr && event_time(*h) < t0_; h = inner_.next()) primer->absorb(*h);
    first_ = h;  // stays valid: inner_ is not advanced while the primer is emitted
    primer->build(t0_);
    if (!primer->out.empty()) primer_ = std::move(primer);
  }
  if (primer_) {
    if (primer_->at < primer_->out.size()) {
      const std::vector<std::byte>& m = primer_->out[primer_->at++];
      std::memcpy(primer_->buf.bytes, m.data(), m.size());
      return &primer_->buf.hdr();
    }
    primer_.reset();
  }
  const EventHeader* h = first_ != nullptr ? std::exchange(first_, nullptr) : inner_.next();
  if (h == nullptr || event_time(*h) >= t1_) {
    done_ = true;
    return nullptr;
  }
  return h;
}

void TimeSliceSource::reset() {
  inner_.reset();
  primer_.reset();
  first_ = nullptr;
  started_ = false;
  done_ = false;
}

}  // namespace fastmm::bt
