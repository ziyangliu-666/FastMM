// MatchingEngine effects -> TotalView-ITCH 5.0 (see itch_publisher.hpp).
#include "fastmm/sim/itch/itch_publisher.hpp"

namespace fastmm::sim::itch {

ItchPublisher::ItchPublisher(ItchOutput& out,
                             ItchNumbering& numbering,
                             std::uint16_t locate,
                             std::string_view symbol,
                             const ItchPublisherOptions& opt)
    : out_(out), num_(numbering), locate_(locate), symbol_(symbol), opt_(opt) {
  pending_.reserve(1024);
}

void ItchPublisher::emit(std::size_t n) noexcept {
  if (n == 0) {
    ++stats_.encode_failures;
    return;
  }
  ++stats_.messages;
  out_.publish(std::span<const std::byte>(buf_.data(), n));
}

void ItchPublisher::flush_deferred_delete() noexcept {
  if (deferred_delete_ == 0) return;
  emit(enc_.order_delete(buf_, locate_, out_.itch_timestamp(), deferred_delete_));
  deferred_delete_ = 0;
}

void ItchPublisher::add(const SimOrder& o, std::uint64_t ref) noexcept {
  const bool mpid = opt_.mpid_every != 0 && ++adds_ % opt_.mpid_every == 0;
  const std::uint64_t ts = out_.itch_timestamp();
  emit(mpid ? enc_.add_order_mpid(
                  buf_, locate_, ts, ref, o.side, o.leaves(), symbol_, o.price, "FMMM")
            : enc_.add_order(buf_, locate_, ts, ref, o.side, o.leaves(), symbol_, o.price));
  resting_[o.order_id] = Resting{ref, o.leaves()};
}

// ---- engine calls ---------------------------------------------------------------------------

SubmitResult ItchPublisher::submit(MatchingEngine& eng, const NewOrder& o, Timestamp now) noexcept {
  const SubmitResult r = eng.submit(o, now);
  after_call(eng);
  return r;
}

bool ItchPublisher::cancel(MatchingEngine& eng,
                           AccountId account,
                           ClientOrderId id,
                           Timestamp now) noexcept {
  const bool ok = eng.cancel(account, id, now);
  after_call(eng);
  return ok;
}

SubmitResult ItchPublisher::replace(MatchingEngine& eng,
                                    AccountId account,
                                    ClientOrderId orig,
                                    ClientOrderId new_id,
                                    Price price,
                                    Qty qty,
                                    Timestamp now) noexcept {
  const SimOrder* old = eng.find(account, orig);
  if (old == nullptr) {  // cancel reject + reject, nothing on the feed
    const SubmitResult r = eng.replace(account, orig, new_id, price, qty, now);
    after_call(eng);
    return r;
  }
  const std::uint64_t old_id = old->order_id;
  const Qty old_leaves = old->leaves();
  // MatchingEngine::replace keeps the order in place under exactly these conditions.
  const bool in_place = qty.is_positive() && price == old->price && qty <= old_leaves &&
                        eng.find(account, new_id) == nullptr;
  if (in_place) {
    in_place_ = true;
    in_place_ref_ = ref_of(old_id);
    const SubmitResult r = eng.replace(account, orig, new_id, price, qty, now);
    in_place_ = false;
    const auto it = resting_.find(old_id);
    if (it != resting_.end()) {
      Resting rest = it->second;
      resting_.erase(it);
      if (qty < rest.leaves) {
        emit(enc_.order_cancel(buf_, locate_, out_.itch_timestamp(), rest.ref, rest.leaves - qty));
        rest.leaves = qty;
      }
      if (r.order_id != 0) resting_[r.order_id] = rest;
    }
    pending_.clear();  // the in-place leg is already on the book under its old reference
    return r;
  }
  reentering_ = true;
  const SubmitResult r = eng.replace(account, orig, new_id, price, qty, now);
  reentering_ = false;
  if (deferred_delete_ != 0) {
    // Nothing traded and nothing else was published since the old leg left: U when the new leg
    // rests whole.
    const SimOrder* now_o = eng.find(account, new_id);
    if (opt_.replace_messages && now_o != nullptr && now_o->cum_qty.is_zero() &&
        pending_.size() == 1 && pending_[0].cl_ord_id == new_id) {
      const std::uint64_t ref = pending_[0].ref;
      emit(enc_.order_replace(buf_,
                              locate_,
                              out_.itch_timestamp(),
                              deferred_delete_,
                              ref,
                              now_o->leaves(),
                              now_o->price));
      resting_[now_o->order_id] = Resting{ref, now_o->leaves()};
      deferred_delete_ = 0;
      pending_.clear();
      return r;
    }
  }
  after_call(eng);
  return r;
}

void ItchPublisher::after_call(const MatchingEngine& eng) noexcept {
  flush_deferred_delete();
  in_place_ = false;
  for (const Pending& p : pending_) {
    const SimOrder* o = eng.find(p.account, p.cl_ord_id);
    if (o == nullptr || resting_.contains(o->order_id)) continue;
    add(*o, p.ref);
  }
  pending_.clear();
}

// ---- MatchingSink ---------------------------------------------------------------------------

void ItchPublisher::on_ack(const SimOrder& o, Timestamp) {
  const std::uint64_t ref = in_place_ ? in_place_ref_ : ++num_.next_ref;
  pending_.push_back(Pending{o.account, o.cl_ord_id, ref});
  if (client_ != nullptr && o.account != kGeneratorAccount) client_->on_ack(o, ref);
}

void ItchPublisher::on_reject(const NewOrder& n, RejectReason why, Timestamp) {
  if (client_ != nullptr && n.account != kGeneratorAccount) client_->on_reject(n, why);
}

void ItchPublisher::on_cancel(const SimOrder& o, CancelReason why, Timestamp) {
  const bool replaced = why == CancelReason::Replaced;
  if (!(replaced && in_place_)) {
    const auto it = resting_.find(o.order_id);
    if (it != resting_.end()) {  // IOC / FOK / market remainders never rested
      if (replaced && reentering_) {
        deferred_delete_ = it->second.ref;  // D or U once the new leg is known
      } else {
        flush_deferred_delete();
        emit(enc_.order_delete(buf_, locate_, out_.itch_timestamp(), it->second.ref));
      }
      resting_.erase(it);
    }
  }
  if (client_ != nullptr && o.account != kGeneratorAccount) client_->on_cancel(o, why);
}

void ItchPublisher::on_fill(
    const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp) {
  flush_deferred_delete();
  const std::uint64_t match = ++num_.next_match;
  const auto it = resting_.find(maker.order_id);
  if (it != resting_.end()) {
    const std::uint64_t ts = out_.itch_timestamp();
    ++execs_;
    if (opt_.priced_exec_every != 0 && execs_ % opt_.priced_exec_every == 0) {
      const bool printable =
          opt_.non_printable_every == 0 || ++priced_ % opt_.non_printable_every != 0;
      emit(enc_.order_executed_with_price(
          buf_, locate_, ts, it->second.ref, qty, match, px, printable));
    } else {
      emit(enc_.order_executed(buf_, locate_, ts, it->second.ref, qty, match));
    }
    it->second.leaves -= qty;
    if (!it->second.leaves.is_positive()) resting_.erase(it);
    if (opt_.trade_print_every != 0 && execs_ % opt_.trade_print_every == 0)  // off-book print
      emit(enc_.trade(buf_,
                      locate_,
                      out_.itch_timestamp(),
                      Side::Buy,
                      qty,
                      symbol_,
                      px,
                      (std::uint64_t{1} << 40) + match));
  }
  if (client_ == nullptr) return;
  if (maker.account != kGeneratorAccount) client_->on_fill(maker, px, qty, 'A', match);
  if (taker.account != kGeneratorAccount) client_->on_fill(taker, px, qty, 'R', match);
}

}  // namespace fastmm::sim::itch
