#include "fastmm/sim/matching_engine.hpp"

namespace fastmm::sim {

namespace {
struct NullSink final : MatchingSink {};
NullSink g_null_sink;
}  // namespace

MatchingEngine::MatchingEngine(std::size_t instrument_count, MatchingSink* sink)
    : instrument_count_(instrument_count == 0 ? 1 : instrument_count),
      sink_(sink == nullptr ? &g_null_sink : sink),
      books_(new SimBook[instrument_count_]) {
  pool_.warm_up();
}

void MatchingEngine::set_stp(AccountId account, StpMode mode) noexcept {
  if (account < kMaxAccounts) stp_[account] = mode;
}
StpMode MatchingEngine::stp(AccountId account) const noexcept {
  return account < kMaxAccounts ? stp_[account] : StpMode::None;
}

const SimOrder* MatchingEngine::find(AccountId account, ClientOrderId id) const noexcept {
  const Handle32* h = by_key_.find(key(account, id));
  return h == nullptr ? nullptr : &pool_.get(*h);
}

MatchingEngine::SideExposure MatchingEngine::exposure(AccountId account,
                                                      InstrumentId id,
                                                      Side side) const noexcept {
  SideExposure e;
  pool_.for_each([&](Handle32, const SimOrder& o) {
    if (o.account != account || o.instrument != id || o.side != side) return;
    ++e.orders;
    e.leaves += o.leaves();
    if (e.best.is_zero() || better(side, o.price, e.best)) e.best = o.price;
  });
  return e;
}

Qty MatchingEngine::account_qty_at(AccountId account,
                                   InstrumentId id,
                                   Side side,
                                   Price px) const noexcept {
  Qty sum{};
  if (id.value >= instrument_count_) return sum;
  const PriceLevel* l = books_[id.value].find(side, px);
  if (l == nullptr) return sum;
  for (std::uint32_t idx = l->head; idx != kNullHandle;) {
    const SimOrder& o = pool_.get(Handle32{idx});
    if (o.account == account) sum += o.leaves();
    idx = o.next;
  }
  return sum;
}

void MatchingEngine::reduce_account_qty(
    AccountId account, InstrumentId id, Side side, Price px, Qty by, Timestamp now) noexcept {
  if (id.value >= instrument_count_) return;
  SimBook& book = books_[id.value];
  while (by.is_positive()) {
    PriceLevel* l = book.find(side, px);
    if (l == nullptr) return;
    // newest matching order first
    std::uint32_t idx = l->tail;
    while (idx != kNullHandle && pool_.get(Handle32{idx}).account != account)
      idx = pool_.get(Handle32{idx}).prev;
    if (idx == kNullHandle) return;
    const Handle32 h{idx};
    SimOrder& o = pool_.get(h);
    if (o.leaves() <= by) {
      by -= o.leaves();
      ++stats_.cancels;
      remove_resting(h, o, CancelReason::Requested, now);  // may erase the level
    } else {
      o.qty -= by;
      l->qty -= by;
      book_changed(id, side, *l);
      return;
    }
  }
}

// ---- submit -------------------------------------------------------------------------------

SubmitResult MatchingEngine::submit(const NewOrder& o, Timestamp now) noexcept {
  ++stats_.submits;
  SubmitResult r;
  auto reject = [&](RejectReason why) {
    ++stats_.rejects;
    r.reason = why;
    sink_->on_reject(o, why, now);
    return r;
  };
  if (FASTMM_UNLIKELY(o.instrument.value >= instrument_count_))
    return reject(RejectReason::InstrumentDisabled);
  if (FASTMM_UNLIKELY(!o.qty.is_positive())) return reject(RejectReason::InvalidLot);
  const bool market = o.type == OrderType::Market;
  if (FASTMM_UNLIKELY(!market && !o.price.is_positive())) return reject(RejectReason::InvalidTick);
  if (FASTMM_UNLIKELY(by_key_.contains(key(o.account, o.cl_ord_id))))
    return reject(RejectReason::DuplicateId);
  SimBook& book = books_[o.instrument.value];
  const Side opp = opposite(o.side);
  if (o.type == OrderType::PostOnly) {
    const PriceLevel* best = book.best(opp);
    if (best != nullptr && crosses(o.side, o.price, best->price))
      return reject(RejectReason::PostOnlyWouldCross);
  }
  const Handle32 h = pool_.allocate();
  if (FASTMM_UNLIKELY(!h.valid())) {
    ++stats_.pool_exhausted;
    return reject(RejectReason::PoolExhausted);
  }
  SimOrder& ord = pool_.get(h);
  ord = SimOrder{};
  ord.order_id = next_order_id_++;
  ord.cl_ord_id = o.cl_ord_id;
  ord.price = market ? Price{} : o.price;
  ord.qty = o.qty;
  ord.created = now;
  ord.instrument = o.instrument;
  ord.next = ord.prev = kNullHandle;
  ord.account = o.account;
  ord.side = o.side;
  ord.type = o.type;
  ord.tif = market ? TimeInForce::Ioc : o.tif;
  by_key_.insert(key(o.account, o.cl_ord_id), h);
  r.order_id = ord.order_id;
  sink_->on_ack(ord, now);

  if (ord.tif == TimeInForce::Fok && book.qty_at_or_better(opp, ord.price, market) < ord.qty) {
    ++stats_.expired;
    sink_->on_cancel(ord, CancelReason::Fok, now);
    free_order(h, ord);
    return r;
  }
  if (o.type != OrderType::PostOnly && !match(h, ord, now, r.filled)) {
    return r;  // cancelled by STP during the sweep (r.filled set by match)
  }
  r.filled = ord.cum_qty;
  if (ord.leaves().is_zero()) {
    free_order(h, ord);
    return r;
  }
  if (market || ord.tif == TimeInForce::Ioc || ord.tif == TimeInForce::Fok) {
    ++stats_.expired;
    sink_->on_cancel(ord, market ? CancelReason::NoLiquidity : CancelReason::Ioc, now);
    free_order(h, ord);
    return r;
  }
  rest(h, ord);
  if (!pool_.is_live(h)) {  // level table full: order rejected after the ack
    r.reason = RejectReason::VenueReject;
    return r;
  }
  r.resting = ord.leaves();
  return r;
}

bool MatchingEngine::match(Handle32 th, SimOrder& taker, Timestamp now, Qty& filled) noexcept {
  SimBook& book = books_[taker.instrument.value];
  const Side maker_side = opposite(taker.side);
  const bool market = taker.type == OrderType::Market;
  const StpMode stp_mode = stp(taker.account);
  while (taker.leaves().is_positive()) {
    PriceLevel* level = book.best(maker_side);
    if (level == nullptr) break;
    if (!market && !crosses(taker.side, taker.price, level->price)) break;
    const Price px = level->price;
    bool taker_done = false;
    std::uint32_t idx = level->head;
    while (idx != kNullHandle && taker.leaves().is_positive()) {
      const Handle32 mh{idx};
      SimOrder& maker = pool_.get(mh);
      const std::uint32_t next = maker.next;
      if (stp_mode != StpMode::None && maker.account == taker.account) {
        ++stats_.stp_cancels;
        if (stp_mode == StpMode::CancelMaker || stp_mode == StpMode::CancelBoth) {
          level->qty -= maker.leaves();
          unlink(maker, *level);
          sink_->on_cancel(maker, CancelReason::Stp, now);
          free_order(mh, maker);
        }
        if (stp_mode == StpMode::CancelTaker || stp_mode == StpMode::CancelBoth) {
          taker_done = true;
          break;
        }
        idx = next;
        continue;
      }
      const Qty q = min(taker.leaves(), maker.leaves());
      maker.cum_qty += q;
      taker.cum_qty += q;
      level->qty -= q;
      record_fill(maker, taker, px, q);
      sink_->on_fill(maker, taker, px, q, now);
      sink_->on_trade(taker.instrument, px, q, taker.side, next_trade_id_++, now);
      if (maker.leaves().is_zero()) {
        unlink(maker, *level);
        free_order(mh, maker);
      }
      idx = next;
    }
    if (level->count == 0) {
      book_level_gone(taker.instrument, maker_side, px);
      book.erase(maker_side, level);
    } else {
      book_changed(taker.instrument, maker_side, *level);
    }
    if (taker_done) {
      // STP cancelled the taker: report the remainder as cancelled and drop it.
      filled = taker.cum_qty;
      sink_->on_cancel(taker, CancelReason::Stp, now);
      free_order(th, taker);
      return false;
    }
  }
  filled = taker.cum_qty;
  return true;
}

void MatchingEngine::rest(Handle32 h, SimOrder& o) noexcept {
  SimBook& book = books_[o.instrument.value];
  PriceLevel* level = book.find_or_insert(o.side, o.price);
  if (FASTMM_UNLIKELY(level == nullptr)) {
    ++stats_.level_full;
    NewOrder n{o.account, o.cl_ord_id, o.instrument, o.side, o.type, o.tif, o.price, o.qty};
    sink_->on_reject(n, RejectReason::VenueReject, o.created);
    free_order(h, o);
    return;
  }
  o.prev = level->tail;
  o.next = kNullHandle;
  if (level->tail != kNullHandle) {
    pool_.get(Handle32{level->tail}).next = h.idx;
  } else {
    level->head = h.idx;
  }
  level->tail = h.idx;
  ++level->count;
  level->qty += o.leaves();
  book_changed(o.instrument, o.side, *level);
}

void MatchingEngine::unlink(SimOrder& o, PriceLevel& l) noexcept {
  if (o.prev != kNullHandle) {
    pool_.get(Handle32{o.prev}).next = o.next;
  } else {
    l.head = o.next;
  }
  if (o.next != kNullHandle) {
    pool_.get(Handle32{o.next}).prev = o.prev;
  } else {
    l.tail = o.prev;
  }
  --l.count;
  o.next = o.prev = kNullHandle;
}

void MatchingEngine::remove_resting(Handle32 h,
                                    SimOrder& o,
                                    CancelReason why,
                                    Timestamp now) noexcept {
  SimBook& book = books_[o.instrument.value];
  PriceLevel* level = book.find(o.side, o.price);
  FASTMM_ASSERT(level != nullptr);
  level->qty -= o.leaves();
  unlink(o, *level);
  if (level->count == 0) {
    book_level_gone(o.instrument, o.side, o.price);
    book.erase(o.side, level);
  } else {
    book_changed(o.instrument, o.side, *level);
  }
  sink_->on_cancel(o, why, now);
  free_order(h, o);
}

void MatchingEngine::book_changed(InstrumentId id, Side side, const PriceLevel& l) noexcept {
  const std::uint64_t u = books_[id.value].bump_update_id();
  sink_->on_book_change(id, side, l.price, l.qty, u);
}
void MatchingEngine::book_level_gone(InstrumentId id, Side side, Price px) noexcept {
  const std::uint64_t u = books_[id.value].bump_update_id();
  sink_->on_book_change(id, side, px, Qty{}, u);
}

void MatchingEngine::record_fill(const SimOrder& maker,
                                 const SimOrder& taker,
                                 Price px,
                                 Qty qty) noexcept {
  ++stats_.fills;
  ++stats_.trades;
  const Notional n = mul(px, qty);
  for (const SimOrder* o : {&maker, &taker}) {
    AccountLedger& l = ledgers_[o->account < kMaxAccounts ? o->account : kMaxAccounts - 1];
    ++l.fills;
    if (o->side == Side::Buy) {
      l.bought += qty;
      l.buy_notional += n;
    } else {
      l.sold += qty;
      l.sell_notional += n;
    }
  }
}

void MatchingEngine::free_order(Handle32 h, const SimOrder& o) noexcept {
  by_key_.erase(key(o.account, o.cl_ord_id));
  pool_.free(h);
}

// ---- cancel / replace ---------------------------------------------------------------------

bool MatchingEngine::cancel(AccountId account, ClientOrderId id, Timestamp now) noexcept {
  const Handle32* hp = by_key_.find(key(account, id));
  if (hp == nullptr) {
    ++stats_.cancel_rejects;
    sink_->on_cancel_reject(account, id, InstrumentId{}, now);
    return false;
  }
  const Handle32 h = *hp;
  ++stats_.cancels;
  remove_resting(h, pool_.get(h), CancelReason::Requested, now);
  return true;
}

SubmitResult MatchingEngine::replace(AccountId account,
                                     ClientOrderId orig,
                                     ClientOrderId new_id,
                                     Price price,
                                     Qty qty,
                                     Timestamp now) noexcept {
  ++stats_.replaces;
  const Handle32* hp = by_key_.find(key(account, orig));
  if (hp == nullptr) {
    ++stats_.cancel_rejects;
    sink_->on_cancel_reject(account, orig, InstrumentId{}, now);
    NewOrder n{
        account, new_id, InstrumentId{}, Side::Buy, OrderType::Limit, TimeInForce::Gtc, price, qty};
    ++stats_.rejects;
    sink_->on_reject(n, RejectReason::VenueUnknownOrder, now);
    SubmitResult r;
    r.reason = RejectReason::VenueUnknownOrder;
    return r;
  }
  const Handle32 h = *hp;
  SimOrder& o = pool_.get(h);
  if (qty.is_positive() && price == o.price && qty <= o.leaves() &&
      !by_key_.contains(key(account, new_id))) {
    // Amend in place: the order keeps its FIFO slot, becomes a fresh venue order.
    SimBook& book = books_[o.instrument.value];
    PriceLevel* level = book.find(o.side, o.price);
    FASTMM_ASSERT(level != nullptr);
    const SimOrder old = o;
    sink_->on_cancel(old, CancelReason::Replaced, now);
    by_key_.erase(key(account, orig));
    const bool qty_changed = qty != o.leaves();
    level->qty -= (o.leaves() - qty);
    o.order_id = next_order_id_++;
    o.cl_ord_id = new_id;
    o.qty = qty;
    o.cum_qty = Qty{};
    o.created = now;
    by_key_.insert(key(account, new_id), h);
    if (qty_changed) book_changed(o.instrument, o.side, *level);
    sink_->on_ack(o, now);
    SubmitResult r;
    r.order_id = o.order_id;
    r.resting = qty;
    return r;
  }
  NewOrder n{account, new_id, o.instrument, o.side, o.type, o.tif, price, qty};
  remove_resting(h, o, CancelReason::Replaced, now);
  return submit(n, now);
}

}  // namespace fastmm::sim
