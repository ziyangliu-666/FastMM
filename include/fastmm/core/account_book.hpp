#pragma once
// AccountBook: the positions of an account on one venue, as fastmm-gateway sees them. Every
// execution that passes through is booked once (keyed like the OMS dedupe: venue execution id,
// instrument, side) into a PositionTracker, marked at the mid of the venue's books as the engine
// marks its own. check_exposure() is the account's version of RiskEngine's portfolio check.
// Single-threaded: it belongs to the venue's network thread.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/position.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fastmm {

class AccountBook {
 public:
  // As deep as a snapshot message (and so any connector's snapshot) can be: the gateway gives an
  // attaching strategy a snapshot of this book, and an engine book (256 levels) built from it
  // then holds what one built from the venue's own snapshot would. The mids are the engine's.
  using Book = L2Book<kMaxBookLevelsPerMsg>;
  // Executions remembered for the dedupe, per venue.
  static constexpr std::size_t kExecWindow = std::size_t{1} << 16;

  AccountBook(const InstrumentTable& insts, VenueId venue)
      : insts_(&insts), seen_(std::make_unique<Seen>()), books_(kMaxInstruments) {
    for (const Instrument& i : insts) {
      if (i.venue == venue) books_[i.id.value] = std::make_unique<Book>();
    }
  }

  [[nodiscard]] const PositionTracker& positions() const noexcept { return pos_; }

  // Has this execution been booked? Remembers it if not. An execution without an id cannot be
  // recognised again and counts as new, as in the OMS.
  [[nodiscard]] bool first_time(const OrderFillMsg& m) noexcept {
    if (m.exec_id.empty()) return true;
    const std::uint64_t key = m.exec_id.hash() ^
                              (static_cast<std::uint64_t>(m.hdr.instrument.value) << 1U) ^
                              (static_cast<std::uint64_t>(m.side) * 0x9E3779B97F4A7C15ULL);
    return seen_->assign(key, 1);
  }

  // Books an execution first_time() accepted, as Engine::on_fill books one: commission in the base
  // asset changes the quantity held and costs fee * price; in another asset it is not valued; an
  // out-of-range fee_asset counts as quote.
  void book(const OrderFillMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!insts_->contains(id)) return;
    const Instrument& inst = insts_->get(id);
    Qty booked = m.qty;
    Notional fee = m.fee;
    if (m.fee_asset == FeeAsset::Base) {
      const Qty fee_base = Qty::from_raw(m.fee.raw);
      fee = inst.notional(m.price, fee_base);
      const Qty held = m.side == Side::Buy ? m.qty - fee_base : m.qty + fee_base;
      if (held.raw > 0) booked = held;
    } else if (m.fee_asset == FeeAsset::Other) {
      fee = Notional{};
    }
    if (booked.is_positive()) pos_.on_fill(id, m.side, m.price, booked, fee, inst);
  }

  // A position the account holds (a strategy's store, a venue's position record).
  void set_position(InstrumentId id, Qty qty, Price avg_px) noexcept {
    if (insts_->contains(id)) pos_.set(id, qty, avg_px, insts_->get(id));
  }

  // Market data: the book, and the mark when it is valid. Returns true when it marked.
  bool on_book(const BookDeltaMsg& d) noexcept {
    const InstrumentId id = d.hdr.instrument;
    if (id.value >= kMaxInstruments || books_[id.value] == nullptr) return false;
    Book& b = *books_[id.value];
    b.apply_delta(d);
    if (!b.is_valid()) return false;
    pos_.mark(id, b.mid(), insts_->get(id));
    return true;
  }
  // The book of an instrument of this venue; nullptr for another venue's.
  [[nodiscard]] const Book* book(InstrumentId id) const noexcept {
    return id.value < books_.size() ? books_[id.value].get() : nullptr;
  }
  // A market-data channel that is not live: its books start over, as the engine's do.
  void on_connection_state(const ConnectionStateMsg& m) noexcept {
    if (m.channel != 0 || m.state == ConnState::Live) return;
    for (auto& b : books_) {
      if (b != nullptr) b->clear();
    }
  }

 private:
  using Seen = RecentMap<std::uint64_t, std::uint8_t, kExecWindow>;
  PositionTracker pos_;
  const InstrumentTable* insts_;
  std::unique_ptr<Seen> seen_;
  std::vector<std::unique_ptr<Book>> books_;
};

// The account's exposure limits on one order, as RiskEngine checks [risk]'s: the order's notional
// on top of the positions marked now. An order that reduces its instrument's position always
// passes, and so does one that brings a net already over the cap towards zero. 0 = no limit.
[[nodiscard]] inline RejectReason check_exposure(const Position& pos,
                                                 Side side,
                                                 Notional notional,
                                                 Notional gross,
                                                 Notional net,
                                                 Notional max_gross,
                                                 Notional max_net) noexcept {
  const std::int64_t dir = sign(side);
  if (pos.qty.raw != 0 && (pos.qty.raw > 0) != (dir > 0)) return RejectReason::None;
  if (max_gross.is_positive() && gross + notional > max_gross)
    return RejectReason::GatewayGrossNotional;
  if (max_net.is_positive()) {
    const Notional after = net + (dir > 0 ? notional : Notional{} - notional);
    if (after.abs() > max_net && after.abs() > net.abs()) return RejectReason::GatewayNetNotional;
  }
  return RejectReason::None;
}

}  // namespace fastmm
