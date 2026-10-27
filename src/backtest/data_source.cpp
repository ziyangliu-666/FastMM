#include "fastmm/backtest/data_source.hpp"

#include "fastmm/core/book/l2_book.hpp"

#include <cstring>
#include <list>
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
  // The recorded orders resting at t0, in arrival order (the queue order within each level), and
  // where each is by (venue, order ref).
  std::list<OrderAddL3Msg> orders;
  std::map<std::pair<std::uint8_t, std::uint64_t>, std::list<OrderAddL3Msg>::iterator> by_ref;
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
      case EventType::OrderAddL3: {
        const auto& m = msg_cast<OrderAddL3Msg>(&h);
        by_ref[{h.venue.value, m.order_ref}] = orders.insert(orders.end(), m);
        break;
      }
      case EventType::OrderExecL3: {
        const auto& m = msg_cast<OrderExecL3Msg>(&h);
        reduce(h.venue.value, m.order_ref, m.exec_qty);
        break;
      }
      case EventType::OrderCancelL3: {
        const auto& m = msg_cast<OrderCancelL3Msg>(&h);
        reduce(h.venue.value, m.order_ref, m.canceled_qty.is_zero() ? Qty::max() : m.canceled_qty);
        break;
      }
      case EventType::OrderReplaceL3: {
        const auto& m = msg_cast<OrderReplaceL3Msg>(&h);
        const auto it = by_ref.find({h.venue.value, m.old_order_ref});
        if (it == by_ref.end()) break;
        OrderAddL3Msg add = *it->second;
        orders.erase(it->second);
        by_ref.erase(it);
        add.hdr = h;
        add.hdr.type = EventType::OrderAddL3;
        add.hdr.len = sizeof(OrderAddL3Msg);
        add.order_ref = m.new_order_ref;
        add.price = m.price;
        add.qty = m.qty;
        by_ref[{h.venue.value, m.new_order_ref}] = orders.insert(orders.end(), add);
        break;
      }
      default: {
        auto& bytes = last[{static_cast<std::uint16_t>(h.type), h.venue.value, h.instrument.value}];
        const auto* p = reinterpret_cast<const std::byte*>(&h);
        bytes.assign(p, p + h.len);
        break;
      }
    }
  }

  void reduce(std::uint8_t venue, std::uint64_t ref, Qty qty) {
    const auto it = by_ref.find({venue, ref});
    if (it == by_ref.end()) return;
    if (qty >= it->second->qty) {
      orders.erase(it->second);
      by_ref.erase(it);
    } else {
      it->second->qty -= qty;
    }
  }

  // The resting orders first, as an order-by-order source starts (ItchSource), then the books.
  void build(Timestamp t0) {
    for (OrderAddL3Msg& m : orders) {
      stamp(m.hdr, t0);
      const auto* p = reinterpret_cast<const std::byte*>(&m);
      out.emplace_back(p, p + sizeof(OrderAddL3Msg));
    }
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
