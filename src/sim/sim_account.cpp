#include "fastmm/sim/sim_account.hpp"

#include "fastmm/core/fx.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace fastmm::sim {

namespace {
// notional * rate, rounded up.
std::int64_t margin_of(std::int64_t n, std::int64_t rate_raw) noexcept {
  if (n <= 0 || rate_raw <= 0) return 0;
  return static_cast<std::int64_t>((static_cast<Int128>(n) * rate_raw + kFixedScale - 1) /
                                   kFixedScale);
}
std::int64_t abs64(std::int64_t v) noexcept {
  return v < 0 ? -v : v;
}
}  // namespace

SimAccounts::SimAccounts(const InstrumentTable& instruments,
                         std::span<const SimAccountConfig> accounts,
                         std::span<const Ratio> initial_margin,
                         const PoolPlan* pools)
    : instruments_(instruments), n_inst_(instruments.size()) {
  slot_of_venue_.fill(kNoSlot);
  for (const SimAccountConfig& a : accounts) {
    if (!a.venue.valid() || a.venue.value >= kMaxVenues)
      throw std::invalid_argument("sim account: venue id out of range");
    const VenueId primary = pools != nullptr ? pools->primary(a.venue) : a.venue;
    const bool traded = std::any_of(instruments.begin(),
                                    instruments.end(),
                                    [&](const Instrument& i) { return i.venue == primary; });
    if (!traded) {
      throw std::invalid_argument("sim account: no instrument trades on venue " +
                                  std::to_string(a.venue.value));
    }
    if (venue_on_[a.venue.value]) {
      throw std::invalid_argument("sim account: venue " + std::to_string(a.venue.value) +
                                  " has two accounts");
    }
    venue_on_[a.venue.value] = true;
    slot_of_venue_[a.venue.value] = static_cast<std::uint8_t>(n_slots_++);
  }
  inst_.resize(std::max<std::size_t>(n_slots_, 1) * n_inst_);
  rows_.reserve(2 * instruments.size() * std::max<std::size_t>(n_slots_, 1) + 16);
  for (std::uint8_t v = 0; v < kMaxVenues; ++v) {
    const std::uint8_t k = slot_of_venue_[v];
    if (k == kNoSlot) continue;
    const VenueId venue{v};
    const VenueId primary = pools != nullptr ? pools->primary(venue) : venue;
    for (const Instrument& i : instruments) {
      if (i.venue != primary) continue;
      Inst& in = at(k, i.id);
      in.on = true;
      in.derivative = i.is_derivative();
      if (i.id.value < initial_margin.size() && initial_margin[i.id.value].raw > 0)
        in.im_raw = initial_margin[i.id.value].raw;
      if (in.derivative) {
        in.settle = row_for(venue, i.settlement_ccy());
      } else {
        in.base = row_for(venue, i.base.view());
        in.quote = row_for(venue, i.quote.view());
      }
    }
  }
  for (const SimAccountConfig& a : accounts) {
    std::vector<std::string> seen;
    for (const SimBalance& b : a.balances) {
      const std::string name(b.asset.view());
      if (name.empty()) throw std::invalid_argument("sim account: an empty asset name");
      for (const std::string& s : seen) {
        if (same_currency(s, name)) {
          throw std::invalid_argument("sim account: asset " + name + " named twice on venue " +
                                      std::to_string(a.venue.value));
        }
      }
      seen.push_back(name);
      rows_[row_for(a.venue, name)].total = b.amount.raw;
    }
  }
}

std::uint16_t SimAccounts::find(VenueId venue, std::string_view asset) const noexcept {
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].venue == venue && same_currency(rows_[i].asset.view(), asset))
      return static_cast<std::uint16_t>(i);
  }
  return kNone;
}

std::uint16_t SimAccounts::row_for(VenueId venue, std::string_view asset) {
  if (asset.empty()) return kNone;
  if (const std::uint16_t r = find(venue, asset); r != kNone) return r;
  Row r;
  r.venue = venue;
  r.asset = FixedString<8>(asset);
  rows_.push_back(r);
  return static_cast<std::uint16_t>(rows_.size() - 1);
}

std::int64_t SimAccounts::hold_of(const Inst& in,
                                  const Instrument& i,
                                  Side side,
                                  Price px,
                                  Qty qty,
                                  bool reduces) const noexcept {
  if (!qty.is_positive()) return 0;
  if (!in.derivative) return side == Side::Sell ? qty.raw : i.notional(px, qty).raw;
  if (reduces) return 0;
  return margin_of(i.notional(px, qty).raw, in.im_raw);
}

bool SimAccounts::admit(ClientOrderId id,
                        InstrumentId inst,
                        Side side,
                        Price px,
                        Qty qty,
                        bool reduce_only,
                        ClientOrderId replaces,
                        VenueId venue) noexcept {
  if (inst.value >= n_inst_) return true;
  const Instrument& i = instruments_.get(inst);
  const std::uint8_t k = slot_for(venue.valid() ? venue : i.venue);
  if (k == kNoSlot || !at(k, inst).on) return true;
  Inst& in = at(k, inst);
  const Hold* old = replaces.valid() ? holds_.find(replaces.value) : nullptr;
  const std::int64_t old_amount = old != nullptr ? old->amount : 0;
  const Side old_side = old != nullptr ? old->side : side;
  // A derivative order that only takes the position towards zero needs no margin.
  const bool reduces = in.derivative && (reduce_only || (in.position != 0 &&
                                                         (side == Side::Buy) == (in.position < 0) &&
                                                         qty.raw <= abs64(in.position)));
  const std::int64_t amount = hold_of(in, i, side, px, qty, reduces);
  if (!in.derivative) {
    const std::uint16_t r = side == Side::Buy ? in.quote : in.base;
    if (r != kNone) {
      const std::int64_t need = amount - (old_side == side ? old_amount : 0);
      const Row& row = rows_[r];
      if (need > 0 && need > row.total - row.locked) return false;
    }
  } else if (!reduces && in.settle != kNone) {
    const Row& row = rows_[in.settle];
    const std::int64_t free = row.total + upnl(in.settle) - row.locked;
    if (in.im_raw == 0) {
      if (free < 0) return false;
    } else {
      std::array<std::int64_t, 2> sides = in.side_hold;
      sides[static_cast<std::size_t>(old_side)] -= old_amount;
      sides[static_cast<std::size_t>(side)] += amount;
      const std::int64_t need =
          std::max(sides[0], sides[1]) - std::max(in.side_hold[0], in.side_hold[1]);
      if (need >= 0 && (free < 0 || need > free)) return false;
    }
  }
  if (old != nullptr) close(replaces);
  Hold h;
  h.inst = inst;
  h.venue = venue.valid() ? venue : i.venue;
  h.slot = k;
  h.side = side;
  h.price = px;
  h.leaves = qty;
  h.amount = amount;
  h.reduce_only = reduces;
  if (holds_.insert(id.value, h).first == nullptr) return true;  // table full: nothing held
  if (!in.derivative) {
    const std::uint16_t r = side == Side::Buy ? in.quote : in.base;
    if (r != kNone && amount != 0) {
      rows_[r].locked += amount;
      touch(r);
    }
  } else if (amount != 0) {
    in.side_hold[static_cast<std::size_t>(side)] += amount;
    relock(in.settle);
  }
  return true;
}

bool SimAccounts::admit_replace(ClientOrderId orig, ClientOrderId id, Price px, Qty qty) noexcept {
  const Hold* o = holds_.find(orig.value);
  if (o == nullptr) return true;
  const Hold h = *o;
  return admit(id, h.inst, h.side, px, qty, h.reduce_only, orig, h.venue);
}

void SimAccounts::fill(ClientOrderId id, Price px, Qty qty, Qty leaves, Notional fee) noexcept {
  Hold* hp = holds_.find(id.value);
  if (hp == nullptr) return;
  Hold& o = *hp;
  Inst& in = at(o.slot, o.inst);
  const Instrument& i = instruments_.get(o.inst);
  const std::int64_t after =
      leaves.is_positive() ? hold_of(in, i, o.side, o.price, leaves, o.reduce_only) : 0;
  const std::int64_t release = o.amount - after;
  const std::int64_t n = i.notional(px, qty).raw;
  if (!in.derivative) {
    const bool buy = o.side == Side::Buy;
    const std::uint16_t held = buy ? in.quote : in.base;
    if (held != kNone) rows_[held].locked -= release;
    if (in.base != kNone) {
      rows_[in.base].total += buy ? qty.raw : -qty.raw;
      touch(in.base);
    }
    if (in.quote != kNone) {
      rows_[in.quote].total += (buy ? -n : n) - fee.raw;
      touch(in.quote);
    }
  } else {
    in.side_hold[static_cast<std::size_t>(o.side)] -= release;
    const std::int64_t signed_qty = o.side == Side::Buy ? qty.raw : -qty.raw;
    std::int64_t realized = 0;
    const std::int64_t pos = in.position;
    if (pos != 0 && (pos > 0) != (signed_qty > 0)) {
      // Closing up to |pos| at px: long profit is N(px) - N(entry) (linear) or N(entry) - N(px)
      // (inverse: the notional is in the base coin).
      const Qty closed = Qty::from_raw(std::min(abs64(pos), qty.raw));
      const std::int64_t at_px = i.notional(px, closed).raw;
      const std::int64_t at_entry = i.notional(in.entry, closed).raw;
      const std::int64_t long_pnl = i.inverse() ? at_entry - at_px : at_px - at_entry;
      realized = pos > 0 ? long_pnl : -long_pnl;
      const std::int64_t rest = qty.raw - closed.raw;
      in.position = pos + (pos > 0 ? -closed.raw : closed.raw);
      if (rest > 0) {
        in.position = signed_qty > 0 ? rest : -rest;
        in.entry = px;
      } else if (in.position == 0) {
        in.entry = Price{};
      }
    } else {
      const std::int64_t was = abs64(pos);
      const std::int64_t now = was + qty.raw;
      in.entry =
          was == 0
              ? px
              : Price::from_raw(static_cast<std::int64_t>((static_cast<Int128>(in.entry.raw) * was +
                                                           static_cast<Int128>(px.raw) * qty.raw) /
                                                          now));
      in.position = pos + signed_qty;
    }
    in.position_margin =
        margin_of(i.notional(in.entry, Qty::from_raw(abs64(in.position))).raw, in.im_raw);
    if (in.settle != kNone) {
      rows_[in.settle].total += realized - fee.raw;
      relock(in.settle);
    }
  }
  o.amount = after;
  o.leaves = leaves;
  if (!leaves.is_positive()) holds_.erase(id.value);
}

void SimAccounts::close(ClientOrderId id) noexcept {
  Hold* hp = holds_.find(id.value);
  if (hp == nullptr) return;
  const Hold o = *hp;
  holds_.erase(id.value);
  if (o.amount == 0) return;
  Inst& in = at(o.slot, o.inst);
  if (!in.derivative) {
    const std::uint16_t r = o.side == Side::Buy ? in.quote : in.base;
    if (r == kNone) return;
    rows_[r].locked -= o.amount;
    touch(r);
    return;
  }
  in.side_hold[static_cast<std::size_t>(o.side)] -= o.amount;
  relock(in.settle);
}

void SimAccounts::relock(std::uint16_t row) noexcept {
  if (row == kNone) return;
  std::int64_t locked = 0;
  for (const Inst& in : inst_) {
    if (!in.on || !in.derivative || in.settle != row) continue;
    locked += in.position_margin + std::max(in.side_hold[0], in.side_hold[1]);
  }
  rows_[row].locked = locked;
  touch(row);
}

std::int64_t SimAccounts::upnl(std::uint16_t row) const noexcept {
  std::int64_t sum = 0;
  for (std::size_t x = 0; x < inst_.size(); ++x) {
    const Inst& in = inst_[x];
    if (!in.on || !in.derivative || in.settle != row || in.position == 0 || !in.mark.is_positive())
      continue;
    const Instrument& i = instruments_.get(InstrumentId{static_cast<std::uint32_t>(x % n_inst_)});
    // As realised on closing (fill()): long N(mark) - N(entry), inverse N(entry) - N(mark).
    const Qty size = Qty::from_raw(abs64(in.position));
    const std::int64_t at_mark = i.notional(in.mark, size).raw;
    const std::int64_t at_entry = i.notional(in.entry, size).raw;
    const std::int64_t long_pnl = i.inverse() ? at_entry - at_mark : at_mark - at_entry;
    sum += in.position > 0 ? long_pnl : -long_pnl;
  }
  return sum;
}

void SimAccounts::touch(std::uint16_t row) noexcept {
  if (row != kNone) dirty_[rows_[row].venue.value] = true;
}

BalanceMsg SimAccounts::message(const Row& r, Timestamp ts) const noexcept {
  BalanceMsg m{};
  init_header(m, EventType::Balance, InstrumentId{}, r.venue);
  m.hdr.exch_ts = ts;
  const auto row = static_cast<std::uint16_t>(&r - rows_.data());
  const std::int64_t u = upnl(row);
  m.free = Notional::from_raw(r.total + u - r.locked);
  m.locked = Notional::from_raw(r.locked);
  m.total = Notional::from_raw(r.total);
  m.equity = Notional::from_raw(r.total + u);
  m.asset = r.asset;
  return m;
}

bool SimAccounts::transfer(VenueId from,
                           VenueId to,
                           std::string_view asset,
                           Notional amount) noexcept {
  if (from == to || amount.raw <= 0 || !enabled(from) || !enabled(to)) return false;
  const std::uint16_t a = find(from, asset);
  const std::uint16_t b = find(to, asset);
  if (a == kNone || b == kNone) return false;
  if (rows_[a].total + upnl(a) - rows_[a].locked < amount.raw) return false;
  rows_[a].total -= amount.raw;
  rows_[b].total += amount.raw;
  touch(a);
  touch(b);
  return true;
}

Notional SimAccounts::free(VenueId venue, std::string_view asset) const noexcept {
  const std::uint16_t r = find(venue, asset);
  return r == kNone ? Notional{} : Notional::from_raw(rows_[r].total + upnl(r) - rows_[r].locked);
}
Notional SimAccounts::unrealized(VenueId venue, std::string_view asset) const noexcept {
  const std::uint16_t r = find(venue, asset);
  return r == kNone ? Notional{} : Notional::from_raw(upnl(r));
}
Notional SimAccounts::locked(VenueId venue, std::string_view asset) const noexcept {
  const std::uint16_t r = find(venue, asset);
  return r == kNone ? Notional{} : Notional::from_raw(rows_[r].locked);
}
Notional SimAccounts::total(VenueId venue, std::string_view asset) const noexcept {
  const std::uint16_t r = find(venue, asset);
  return r == kNone ? Notional{} : Notional::from_raw(rows_[r].total);
}
Qty SimAccounts::position(InstrumentId id) const noexcept {
  if (id.value >= n_inst_) return Qty{};
  std::int64_t sum = 0;
  for (std::size_t k = 0; k < n_slots_; ++k) sum += at(k, id).position;
  return Qty::from_raw(sum);
}

}  // namespace fastmm::sim
