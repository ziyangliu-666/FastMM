#pragma once
// QuoteManager (5.6): diffs the strategy's DesiredQuotes against the orders resting at the
// venue and emits the minimal set of New / Cancel / Replace actions.
//
// Rules:
//   * level i of a side maps to slot i (one order per slot);
//   * hysteresis: keep an order whose price is within min_requote_ticks of the desired
//     price and whose leaves cover min_qty_bps of the desired quantity;
//   * never re-quote a slot within min_requote_interval of its last change;
//   * orders in a Pending* state are never touched: the slot records the target instead and applies
//     it when the order's ack or terminal update arrives via on_order_update() (dropped if the
//     instrument is pulled first);
//   * a slot only counts an order as its own if it is live and carries the slot's tag (order
//     slots are reused, and a replace changes the order's id);
//   * while an instrument is pulled, an order that becomes working late (its ack arrived after
//     the pull) is cancelled at once;
//   * a New or Replace refused by the venue (other than a post-only cross, or an order the venue
//     no longer has) blocks new orders and replaces on that side for reject_backoff, doubling on
//     each further refusal up to reject_backoff_max and reset by an ack on that side, so a side
//     the venue keeps refusing (no balance, a price outside its band during a move) is not resent
//     on every requote; a requote that meets a working order meanwhile cancels it, as on a venue
//     without replace, and the New waits for the backoff to end. A New or Replace the engine's own
//     checks refuse for its price (price collar, fat-finger band: QuoteAction::refused) backs off
//     the same way, since the next requote during a move asks for a price just as far out;
//   * an instrument whose venue supports replace (set_replace(), with supports_replace) gets a
//     single Replace; otherwise Cancel now and New only once the cancel's terminal update arrives
//     via on_order_update() (cancel-then-new keeps momentary exposure down and never double-quotes
//     a level, and the New is checked against the balance once the old order's hold is released);
//   * such a deferred New (QuoteAction::deferred) that the balance no longer covers -- the order it
//     replaced filled instead of cancelling -- is withheld (QuoteAction::withheld, kept_balance)
//     rather than refused by risk: the target predates the fill, the strategy's next quotes decide;
//   * pause_quotes() holds an instrument's orders where they rest (the engine during a
//     reconciliation): nothing is sent, moved or cancelled until resume(), which re-applies the
//     last desired quotes against them, so an order that still matches is kept rather than
//     cancelled and sent again; pull_quotes(keep_desired) does the same with the orders cancelled;
//   * a move towards the touch (a bid up, an ask down) that the placer holds (QuoteAction::held:
//     the account the order rests on has no more than QuoteParams::room_reserve orders left in its
//     venue order windows) keeps the resting order where it is: the move would spend one of the
//     last orders, and with cancel-then-new a New the window then refuses leaves the level empty.
//     Moves away from the touch and pulls still go. The strategy's next quotes try the move again;
//   * a New or a move the placer prices out (QuoteAction::priced_out: [capacity], its value is
//   below
//     the price of the order slot it takes, core/capacity_price.hpp) is neither a reject nor a
//     backoff: a move keeps the resting order where it is, a New leaves the level empty, and the
//     instrument joins a queue the engine re-applies (retry_priced()) when the price falls. A move
//     towards the touch buys its gain (the level's value less its keep value); a move away from it
//     cancels for free and buys its New as a new order;
//   * hold_side() (the engine's fill-cluster guard, core/fill_cluster.hpp) holds one side on every
//     instrument: pull_side() cancels what rests there, reconcile() and every retry treat the
//     side's levels as empty while the desired quotes keep them, a target recorded for a pending
//     order on the side is not placed, and reapply() puts the last desired quotes back when the
//     hold ends;
//   * a New or Replace the placer holds back for want of an order token (QuoteAction::rate_limited)
//     is neither a reject nor a backoff: the instrument joins a FIFO of starved instruments, and
//     the engine re-applies their last desired quotes (retry()) in that order as tokens come back,
//     so a burst of requotes on many instruments is served in turn instead of refused event by
//     event.
//
// Actions are executed by the caller through a Placer callable so the QuoteManager stays
// independent of risk/OMS/transport wiring:
//   bool place(QuoteAction& a);   // New: sets a.handle on success; returns false on reject
#include "fastmm/core/capacity_price.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/ring_buffer.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fastmm {

inline constexpr std::size_t kMaxQuoteLevels = 8;

struct DesiredQuotes {
  StaticVector<Level, kMaxQuoteLevels> bids;  // index 0 = closest to mid
  StaticVector<Level, kMaxQuoteLevels> asks;
  // The pool account a level's order goes from (NewOrderRequest::account); invalid = the engine's
  // automatic routing. A resting order on another account is cancelled and placed again.
  std::array<VenueId, kMaxQuoteLevels> bid_account{};
  std::array<VenueId, kMaxQuoteLevels> ask_account{};
  // What a level's order is worth, in the quote currency ([capacity], core/capacity_price.hpp):
  // `value` placed at the level's price, `keep` the order already resting in the slot where it
  // rests. kNoValue: not stated ([capacity] value decides).
  std::array<float, kMaxQuoteLevels> bid_value = no_values();
  std::array<float, kMaxQuoteLevels> ask_value = no_values();
  std::array<float, kMaxQuoteLevels> bid_keep = no_values();
  std::array<float, kMaxQuoteLevels> ask_keep = no_values();
  void clear() noexcept {
    bids.clear();
    asks.clear();
    bid_account = {};
    ask_account = {};
    bid_value = no_values();
    ask_value = no_values();
    bid_keep = no_values();
    ask_keep = no_values();
  }
  [[nodiscard]] VenueId account(Side s, std::uint32_t lvl) const noexcept {
    return lvl < kMaxQuoteLevels ? (s == Side::Buy ? bid_account : ask_account)[lvl] : VenueId{};
  }
  // Sets the value of level `lvl` of a side and, optionally, the keep value of its resting order.
  void set_value(Side s, std::uint32_t lvl, float value, float keep = kNoValue) noexcept {
    if (lvl >= kMaxQuoteLevels) return;
    (s == Side::Buy ? bid_value : ask_value)[lvl] = value;
    (s == Side::Buy ? bid_keep : ask_keep)[lvl] = keep;
  }
  [[nodiscard]] float value(Side s, std::uint32_t lvl) const noexcept {
    return lvl < kMaxQuoteLevels ? (s == Side::Buy ? bid_value : ask_value)[lvl] : kNoValue;
  }
  [[nodiscard]] float keep(Side s, std::uint32_t lvl) const noexcept {
    return lvl < kMaxQuoteLevels ? (s == Side::Buy ? bid_keep : ask_keep)[lvl] : kNoValue;
  }
  [[nodiscard]] const StaticVector<Level, kMaxQuoteLevels>& side(Side s) const noexcept {
    return s == Side::Buy ? bids : asks;
  }
  [[nodiscard]] bool empty() const noexcept { return bids.empty() && asks.empty(); }
  // Append the next level of a side (index 0 first). A non-positive price or quantity is dropped,
  // as is a level beyond kMaxQuoteLevels; returns whether the level was added.
  bool bid(Price px, Qty qty) noexcept {
    return px.is_positive() && qty.is_positive() && bids.push_back(Level{px, qty});
  }
  bool ask(Price px, Qty qty) noexcept {
    return px.is_positive() && qty.is_positive() && asks.push_back(Level{px, qty});
  }
  // ... with the level's value and its resting order's keep value ([capacity]).
  bool bid(Price px, Qty qty, float value, float keep = kNoValue) noexcept {
    if (!bid(px, qty)) return false;
    set_value(Side::Buy, static_cast<std::uint32_t>(bids.size() - 1), value, keep);
    return true;
  }
  bool ask(Price px, Qty qty, float value, float keep = kNoValue) noexcept {
    if (!ask(px, qty)) return false;
    set_value(Side::Sell, static_cast<std::uint32_t>(asks.size() - 1), value, keep);
    return true;
  }
  // Never cross ourselves at level 0: an ask at or below the best bid moves one tick above it.
  void uncross(Price tick) noexcept {
    if (!bids.empty() && !asks.empty() && bids[0].price >= asks[0].price)
      asks[0].price = bids[0].price + tick;
  }

 private:
  static constexpr std::array<float, kMaxQuoteLevels> no_values() noexcept {
    std::array<float, kMaxQuoteLevels> a{};
    for (float& v : a) v = kNoValue;
    return a;
  }
};

struct QuoteParams {
  std::int64_t min_requote_ticks = 1;  // |desired - resting| < this -> keep
  Duration min_requote_interval = milliseconds(50);
  std::int64_t min_qty_bps = 8000;  // leaves >= 80 % of desired -> qty ok
  bool supports_replace = false;    // off: never replace; on: where the instrument's venue can
  // Replace only if every instrument's venue can (the engine before replace was per venue; set to
  // replay journals recorded then).
  bool replace_all_venues = false;
  bool post_only = true;
  Duration reject_backoff = milliseconds(1000);  // 0 = no backoff after venue rejects
  Duration reject_backoff_max = milliseconds(60000);
  // Order tokens ([risk] orders_per_sec) a quote that does not reduce the position leaves for the
  // orders a strategy sends itself (hedges, unwinds) and for quotes that do. Read by the engine's
  // placer; 0 = quotes may take the last token.
  std::int64_t token_reserve = 0;
  // Order tokens every quote leaves for the orders a strategy sends itself (ctx.send, a hedge or an
  // unwind), on top of token_reserve for the quotes that add risk. A quote short of a token waits
  // for one; a sent order short of one is refused RateLimit, and the waiting quotes take each token
  // as it comes back, so without this a sent order can find none for as long as quotes wait.
  // 0 = quotes may take the last token.
  std::int64_t send_reserve = 0;
  // Orders an account's venue order windows (ctx.order_budget) keep for News into empty levels and
  // for moves away from the touch: with no more than this many left, a resting quote is not moved
  // towards the touch (held, QuoteStats::kept_room). Read by the engine's placer; 0 = only when
  // none is left; -1 = never held.
  std::int64_t room_reserve = 0;
};

enum class QuoteActionKind : std::uint8_t { New, Cancel, Replace };

struct QuoteAction {
  QuoteActionKind kind;
  InstrumentId instrument;
  Side side;
  std::uint32_t level;
  Handle<Order> handle;     // Cancel/Replace input; New output
  ClientOrderId cl_ord_id;  // New output (the placer fills it)
  Price price;
  Qty qty;
  bool post_only;
  // New: the target was recorded while an earlier order held the slot and is placed now that it
  // ended (on_order_update), so it was decided against a balance that order may have changed.
  bool deferred;
  bool withheld;  // New output: not sent, the balance does not cover it (deferred only)
  // New / Replace output: not sent, no order token for it now (QuoteParams::token_reserve,
  // send_reserve); the instrument is retried when tokens come back.
  bool rate_limited;
  VenueId account{};  // New: the pool account to send it from (invalid = automatic)
  // New / Replace output: the pre-trade check that refused it (None: sent, or not refused by one).
  RejectReason refused{};
  // Cancel / Replace output: not sent, the move goes towards the touch and the order's account is
  // short of order room (QuoteParams::room_reserve); the order stays as it is. A Cancel that moves
  // a quote carries the target in `price` and `qty`; a pull carries none and is never held.
  bool held = false;
  // New / Replace / a Cancel that moves: the level's value and the resting order's keep value
  // (DesiredQuotes::value, keep; kNoValue: not stated).
  float value = kNoValue;
  float keep = kNoValue;
  // Output: not sent, the action is worth less than the order slot it takes ([capacity]).
  bool priced_out = false;
  // A Cancel that moves, output: the move was bought, so the New after it is not priced again.
  // A deferred New, input: the move that freed its slot was bought.
  bool paid = false;
};

struct QuoteStats {
  std::uint64_t news = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t kept_hysteresis = 0;
  std::uint64_t kept_interval = 0;
  std::uint64_t skipped_pending = 0;
  std::uint64_t rejected = 0;
  std::uint64_t reject_backoffs = 0;  // venue rejects that started or extended a side backoff
  std::uint64_t kept_backoff = 0;     // News withheld because their side was backing off
  std::uint64_t kept_balance = 0;     // deferred News withheld: the balance no longer covers them
  std::uint64_t kept_rate_limit = 0;  // News / Replaces held back for want of an order token
  std::uint64_t kept_room = 0;        // moves towards the touch held: the account is short of room
  std::uint64_t retries = 0;          // retry() calls that re-applied a starved instrument's quotes
  std::uint64_t priced_out = 0;  // News not sent: worth less than their order slot ([capacity])
  std::uint64_t priced_out_moves = 0;  // moves not made, the order left where it rests
};

class QuoteManager {
  static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

 public:
  explicit QuoteManager(const QuoteParams& p = {}) noexcept : params_(p) { resolve_replace(); }

  [[nodiscard]] const QuoteParams& params() const noexcept { return params_; }
  void set_params(const QuoteParams& p) noexcept {
    params_ = p;
    resolve_replace();
  }
  [[nodiscard]] const QuoteStats& stats() const noexcept { return stats_; }
  // Whether the instrument's venue amends in place (default true; supports_replace still applies).
  void set_replace(InstrumentId id, bool on) noexcept {
    InstState& st = state_[id.value];
    st.venue_no_replace = !on;
    st.replace = params_.supports_replace && on;
  }
  [[nodiscard]] bool replaces(InstrumentId id) const noexcept { return state_[id.value].replace; }

  // user_tag encoding so a terminal OmsUpdate can be mapped back to its slot.
  [[nodiscard]] static constexpr std::uint32_t make_tag(Side s, std::uint32_t level) noexcept {
    return 0x5100'0000U | (static_cast<std::uint32_t>(s) << 8) | level;
  }
  [[nodiscard]] static constexpr bool is_quote_tag(std::uint32_t tag) noexcept {
    return (tag & 0xFFFF'0000U) == 0x5100'0000U;
  }

  // Reconciles desired vs resting for one instrument. Returns the number of actions placed.
  template <class Placer>
  std::uint32_t reconcile(const Instrument& inst,
                          const DesiredQuotes& desired,
                          const Oms& oms,
                          Timestamp now,
                          Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    st.pulled = false;
    st.paused = false;
    st.resumable = false;
    st.desired = desired;
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      const auto s = static_cast<std::size_t>(side);
      const auto& want = st.desired.side(side);
      // A level the ladder does not ask for and whose slot is empty has nothing to reconcile.
      const std::uint32_t n = std::max(static_cast<std::uint32_t>(want.size()), st.used[s]);
      std::uint32_t used = 0;
      for (std::uint32_t lvl = 0; lvl < n; ++lvl) {
        Slot& slot = st.slots[s][lvl];
        const bool has_want = !side_held_[s] && lvl < want.size() && want[lvl].qty.is_positive();
        actions += reconcile_slot(inst,
                                  st,
                                  oms,
                                  side,
                                  lvl,
                                  slot,
                                  has_want ? want[lvl] : Level{},
                                  st.desired.account(side, lvl),
                                  st.desired.value(side, lvl),
                                  st.desired.keep(side, lvl),
                                  now,
                                  place);
        if (!slot.empty()) used = lvl + 1;
      }
      st.used[s] = used;
    }
    return actions;
  }

  // Cancels every working quote on the instrument and suppresses re-quoting until the
  // next reconcile(). With keep_desired (a pause, not a decision to stop quoting) the last desired
  // quotes are kept for resume(), unless the instrument was already pulled.
  template <class Placer>
  std::uint32_t pull_quotes(const Instrument& inst,
                            const Oms& oms,
                            Placer&& place,
                            bool keep_desired = false) noexcept {
    InstState& st = state_[inst.id.value];
    if (keep_desired) {
      st.resumable = st.resumable || (!st.pulled && !st.desired.empty());
    } else {
      st.resumable = false;
      st.desired.clear();
    }
    st.pulled = true;
    st.paused = false;
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
        Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
        slot.apply_want = false;
        if (!owns(oms, inst, side, lvl, slot)) continue;
        const Order& o = oms.get(slot.handle);
        if (!o.is_working()) continue;  // pending: cannot touch; a later pull will catch it
        actions += cancel(inst, side, lvl, slot, place);
      }
    }
    return actions;
  }

  // Holds the instrument's orders as they are until resume(): no New, Cancel or Replace, and a
  // target recorded for a slot is dropped (resume() re-applies the whole ladder). The last desired
  // quotes are kept for resume() unless the instrument was already pulled. Sends nothing.
  void pause_quotes(InstrumentId id) noexcept {
    InstState& st = state_[id.value];
    st.resumable = st.resumable || (!st.pulled && !st.desired.empty());
    st.paused = true;
    for (auto& side : st.slots)
      for (Slot& slot : side) slot.apply_want = false;
  }
  [[nodiscard]] bool paused(InstrumentId id) const noexcept { return state_[id.value].paused; }

  // Ends a pause and re-applies the desired quotes kept by pause_quotes() or
  // pull_quotes(keep_desired) to the orders resting now. Returns the actions placed.
  template <class Placer>
  std::uint32_t resume(const Instrument& inst,
                       const Oms& oms,
                       Timestamp now,
                       Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    st.paused = false;
    if (!st.resumable) return 0;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }
  [[nodiscard]] bool resumable(InstrumentId id) const noexcept {
    return state_[id.value].resumable;
  }

  // Holds `side` on every instrument (on) or ends the hold (off). While it holds, the side's levels
  // count as empty wherever quotes are reconciled; the desired quotes keep them for reapply().
  void hold_side(Side side, bool on) noexcept { side_held_[static_cast<std::size_t>(side)] = on; }
  [[nodiscard]] bool side_held(Side side) const noexcept {
    return side_held_[static_cast<std::size_t>(side)];
  }
  // Cancels the instrument's working quotes on `side`, now; one still pending is cancelled when its
  // ack arrives. The other side and the desired quotes stay as they are. Returns the actions
  // placed.
  template <class Placer>
  std::uint32_t pull_side(
      const Instrument& inst, Side side, const Oms& oms, Timestamp now, Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    const auto s = static_cast<std::size_t>(side);
    std::uint32_t actions = 0;
    std::uint32_t used = 0;
    for (std::uint32_t lvl = 0; lvl < st.used[s]; ++lvl) {
      Slot& slot = st.slots[s][lvl];
      actions += reconcile_slot(
          inst, st, oms, side, lvl, slot, Level{}, VenueId{}, kNoValue, kNoValue, now, place);
      if (!slot.empty()) used = lvl + 1;
    }
    st.used[s] = used;
    return actions;
  }
  // Re-applies the instrument's last desired quotes (a hold ended); one pulled or paused stays as
  // it is.
  template <class Placer>
  std::uint32_t reapply(const Instrument& inst,
                        const Oms& oms,
                        Timestamp now,
                        Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    if (st.pulled || st.paused || st.desired.empty()) return 0;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }

  // Instruments whose quotes were held back for want of an order token, oldest first.
  [[nodiscard]] bool starved() const noexcept { return !starved_.empty(); }
  [[nodiscard]] std::size_t starved_count() const noexcept { return starved_.size(); }
  // Takes the oldest starved instrument off the queue (the caller checks starved() first).
  [[nodiscard]] InstrumentId pop_starved() noexcept {
    const InstrumentId id{starved_.front()};
    starved_.pop();
    state_[id.value].starved = false;
    return id;
  }
  // Re-applies the last desired quotes of an instrument popped from the starved queue; one that was
  // pulled meanwhile stays pulled. An action held back again queues it at the back.
  template <class Placer>
  std::uint32_t retry(const Instrument& inst,
                      const Oms& oms,
                      Timestamp now,
                      Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    if (st.pulled || st.paused) return 0;
    ++stats_.retries;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }

  // Instruments with a New held back by a side's reject backoff: once the backoff ends, the engine
  // re-applies their last desired quotes (retry_backed_off), so a side comes back by itself rather
  // than at the strategy's next requote, which a quiet market may not bring.
  [[nodiscard]] bool backed_off() const noexcept { return !backed_off_.empty(); }
  [[nodiscard]] bool backoff_due(Timestamp now) const noexcept { return now >= backoff_due_; }
  // Takes each instrument whose held-back side may quote at `now` off the queue and calls
  // f(InstrumentId); the others stay queued.
  template <class F>
  void take_backoff_due(Timestamp now, F&& f) noexcept {
    if (now < backoff_due_) return;
    backoff_due_ = Timestamp{kNever};
    for (std::size_t n = backed_off_.size(); n > 0; --n) {
      const std::uint16_t id = backed_off_.front();
      backed_off_.pop();
      InstState& st = state_[id];
      if (now < st.retry_at) {
        static_cast<void>(backed_off_.try_push(id));
        backoff_due_ = std::min(backoff_due_, st.retry_at);
        continue;
      }
      st.retry_at = Timestamp{kNever};
      f(InstrumentId{id});
    }
  }
  // Re-applies the last desired quotes of an instrument take_backoff_due() handed out; one pulled
  // meanwhile stays pulled. A side still backing off queues it again.
  template <class Placer>
  std::uint32_t retry_backed_off(const Instrument& inst,
                                 const Oms& oms,
                                 Timestamp now,
                                 Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    if (st.pulled || st.paused) return 0;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }

  // Instruments with a level priced out ([capacity]): the engine re-applies their last desired
  // quotes (retry_priced) when the price may have fallen, a window rolling over or a fill giving a
  // slot back.
  [[nodiscard]] bool priced_out() const noexcept { return !priced_.empty(); }
  // Takes every instrument queued now off the queue and calls f(InstrumentId); one priced out
  // again during f queues itself for the next time.
  template <class F>
  void take_priced(F&& f) noexcept {
    for (std::size_t n = priced_.size(); n > 0; --n) {
      const std::uint16_t id = priced_.front();
      priced_.pop();
      state_[id].priced = false;
      f(InstrumentId{id});
    }
  }
  template <class Placer>
  std::uint32_t retry_priced(const Instrument& inst,
                             const Oms& oms,
                             Timestamp now,
                             Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    if (st.pulled || st.paused) return 0;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }

  // Feed every OmsUpdate here. A terminal update frees the slot it belonged to and, if a target
  // was recorded for it (cancel-then-new, or a requote that met the order pending), places the New
  // now. The ack of an order that was pending at a requote applies the recorded target. While the
  // instrument is pulled, an order that has just become working is cancelled instead.
  template <class Placer>
  void on_order_update(const OmsUpdate& u,
                       const Instrument& inst,
                       const Oms& oms,
                       Timestamp now,
                       Placer&& place) noexcept {
    if (!is_quote_tag(u.order.user_tag)) return;
    const Side side = static_cast<Side>((u.order.user_tag >> 8) & 1U);
    const std::uint32_t lvl = u.order.user_tag & 0xFFU;
    if (lvl >= kMaxQuoteLevels || u.order.instrument != inst.id) return;
    InstState& st = state_[inst.id.value];
    Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
    if (u.refused != RejectReason::None) on_venue_reject(st, side, u.refused, now);
    if (!u.terminal) {
      if ((u.prev == OrderState::PendingNew || u.prev == OrderState::PendingReplace) &&
          u.refused == RejectReason::None && u.order.is_working())
        st.backoff[static_cast<std::size_t>(side)] = Duration{};  // the venue takes this side
      if (!u.handle.valid() || slot.handle.idx != u.handle.idx ||
          !owns(oms, inst, side, lvl, slot) || !u.order.is_working())
        return;
      if (st.pulled) {
        static_cast<void>(cancel(inst, side, lvl, slot, place));
        return;
      }
      if (st.paused) return;
      if (slot.apply_want &&
          (u.prev == OrderState::PendingNew || u.prev == OrderState::PendingReplace)) {
        slot.apply_want = false;
        static_cast<void>(
            reconcile_slot(inst,
                           st,
                           oms,
                           side,
                           lvl,
                           slot,
                           side_held_[static_cast<std::size_t>(side)] ? Level{} : slot.want,
                           slot.want_account,
                           slot.want_value,
                           slot.want_keep,
                           now,
                           place));
      }
      return;
    }
    // The terminal order was this slot's unless the slot already holds a newer live order. Ids
    // are not compared: a replace changes the order's id, and a stale id used to drop the update
    // and leave the slot waiting for a terminal state forever.
    if (owns(oms, inst, side, lvl, slot)) return;
    slot.handle = Handle<Order>{};
    slot.cl_ord_id = ClientOrderId{};
    slot.awaiting_terminal = false;
    if (st.pulled || st.paused || !slot.apply_want) return;
    slot.apply_want = false;
    if (!slot.want.qty.is_positive() || side_held_[static_cast<std::size_t>(side)]) return;
    if (backing_off(st, side, now)) {
      kept_backoff(st, inst.id, side);
      return;
    }
    submit_new(inst,
               side,
               lvl,
               slot,
               slot.want,
               slot.want_account,
               slot.want_value,
               now,
               place,
               /*deferred=*/true);
  }

  [[nodiscard]] Handle<Order> slot_handle(InstrumentId id,
                                          Side s,
                                          std::uint32_t level) const noexcept {
    return state_[id.value].slots[static_cast<std::size_t>(s)][level].handle;
  }
  [[nodiscard]] bool pulled(InstrumentId id) const noexcept { return state_[id.value].pulled; }
  // The last ladder reconcile() applied to the instrument (empty after a pull).
  [[nodiscard]] const DesiredQuotes& desired(InstrumentId id) const noexcept {
    return state_[id.value].desired;
  }
  // The instrument waits for an order token (a New or Replace was rate-limited).
  [[nodiscard]] bool starved(InstrumentId id) const noexcept { return state_[id.value].starved; }
  // The side is in its venue-reject backoff at `now`.
  [[nodiscard]] bool backing_off(InstrumentId id, Side s, Timestamp now) const noexcept {
    return backing_off(state_[id.value], s, now);
  }

 private:
  struct Slot {
    Handle<Order> handle{};
    ClientOrderId cl_ord_id{};
    Timestamp last_requote{};
    Level want{};                 // target to apply once the slot's pending order resolves
    VenueId want_account{};       // ... and its account (DesiredQuotes::account)
    float want_value = kNoValue;  // ... and its value and keep value ([capacity])
    float want_keep = kNoValue;
    bool paid = false;        // the move that is freeing the slot was bought: its New is not priced
    bool apply_want = false;  // `want` is pending (qty 0: no quote on this level)
    bool awaiting_terminal = false;
    // No order and nothing to apply: reconciling the slot against no level changes nothing.
    [[nodiscard]] bool empty() const noexcept {
      return !handle.valid() && !apply_want && !awaiting_terminal;
    }
  };
  struct InstState {
    Slot slots[2][kMaxQuoteLevels];
    // Per side, 1 + the highest level whose slot was not empty after the last reconcile(). A slot
    // only fills at a level the ladder asks for, so no slot above max(used, ladder) holds anything.
    std::uint32_t used[2] = {};
    DesiredQuotes desired;
    bool pulled = false;
    bool paused = false;     // pause_quotes(): orders held where they rest until resume()
    bool resumable = false;  // paused or pulled with keep_desired: resume() re-applies `desired`
    bool venue_no_replace = false;    // the venue cannot amend in place (set_replace)
    bool replace = false;             // supports_replace && !venue_no_replace
    Duration backoff[2] = {};         // current backoff per side (0 = none)
    Timestamp blocked_until[2] = {};  // no News or Replaces on the side before this
    bool starved = false;             // queued in starved_
    bool priced = false;              // queued in priced_
    Timestamp retry_at{kNever};       // queued in backed_off_: a held-back side's backoff ends
  };

  [[nodiscard]] static bool backing_off(const InstState& st, Side side, Timestamp now) noexcept {
    const auto i = static_cast<std::size_t>(side);
    return st.backoff[i].ns > 0 && now < st.blocked_until[i];
  }
  // A New on `side` waits for the side's backoff: queue the instrument to be re-applied when it
  // ends.
  void kept_backoff(InstState& st, InstrumentId id, Side side) noexcept {
    ++stats_.kept_backoff;
    const Timestamp until = st.blocked_until[static_cast<std::size_t>(side)];
    if (st.retry_at.ns == kNever) {
      if (!backed_off_.try_push(static_cast<std::uint16_t>(id.value))) return;
    }
    st.retry_at = std::min(st.retry_at, until);
    backoff_due_ = std::min(backoff_due_, until);
  }
  void on_venue_reject(InstState& st, Side side, RejectReason reason, Timestamp now) noexcept {
    // A post-only cross is a stale price, fixed by the next requote, and a replace of an order the
    // venue no longer has lost a race with its fill or cancel; other venue rejects (no balance,
    // filters, a price outside the venue's band, permissions) repeat until something changes.
    if (reason == RejectReason::PostOnlyWouldCross || reason == RejectReason::VenueUnknownOrder)
      return;
    back_off(st, side, now);
  }
  // Starts or doubles the side's backoff.
  void back_off(InstState& st, Side side, Timestamp now) noexcept {
    if (params_.reject_backoff.ns <= 0) return;
    const auto i = static_cast<std::size_t>(side);
    Duration next = st.backoff[i].ns <= 0 ? params_.reject_backoff : st.backoff[i] + st.backoff[i];
    if (params_.reject_backoff_max.ns > 0 && next.ns > params_.reject_backoff_max.ns)
      next = params_.reject_backoff_max;
    st.backoff[i] = next;
    st.blocked_until[i] = now + next;
    ++stats_.reject_backoffs;
  }

  // One slot of reconcile(): keep, cancel, replace or place so the slot converges on `target`
  // (qty 0: no quote).
  template <class Placer>
  std::uint32_t reconcile_slot(const Instrument& inst,
                               InstState& st,
                               const Oms& oms,
                               Side side,
                               std::uint32_t lvl,
                               Slot& slot,
                               Level target,
                               VenueId account,
                               float value,
                               float keep,
                               Timestamp now,
                               Placer& place) noexcept {
    const bool has_want = target.qty.is_positive();
    if (owns(oms, inst, side, lvl, slot)) {
      const Order& o = oms.get(slot.handle);
      if (is_pending(o.state)) {
        // Cannot touch it now; apply the target when its ack or terminal update arrives.
        ++stats_.skipped_pending;
        slot.want = target;
        slot.want_account = account;
        slot.want_value = value;
        slot.want_keep = keep;
        slot.apply_want = true;
        return 0;
      }
      slot.apply_want = false;
      if (!has_want) return cancel(inst, side, lvl, slot, place);
      // Asked for on another account: an order cannot move between accounts.
      if (account.valid() && o.venue != account) {
        slot.want = target;
        slot.want_account = account;
        slot.want_value = value;
        slot.want_keep = keep;
        slot.apply_want = true;
        slot.paid = false;
        return cancel(inst, side, lvl, slot, place);
      }
      const std::int64_t dpx = (target.price - o.price).abs().raw / inst.tick.raw;
      const bool qty_ok = static_cast<Int128>(o.leaves_qty().raw) * 10'000 >=
                          static_cast<Int128>(target.qty.raw) * params_.min_qty_bps;
      if (dpx < params_.min_requote_ticks && qty_ok) {
        ++stats_.kept_hysteresis;
        return 0;
      }
      if (now - slot.last_requote < params_.min_requote_interval) {
        ++stats_.kept_interval;
        return 0;
      }
      // A side backing off sends no replace either: the order is cancelled and the New waits.
      if (st.replace && !backing_off(st, side, now)) {
        QuoteAction a{QuoteActionKind::Replace,
                      inst.id,
                      side,
                      lvl,
                      slot.handle,
                      ClientOrderId{},
                      target.price,
                      target.qty,
                      params_.post_only,
                      false,
                      false,
                      false};
        a.value = value;
        a.keep = keep;
        if (!place(a)) {
          if (a.priced_out) {
            // Away from the touch the resting order is not wanted where it is: cancel it (free)
            // and buy its New as a new order. Towards the touch it stays.
            if (side == Side::Buy ? target.price < o.price : target.price > o.price)
              return move(inst, side, lvl, slot, target, account, value, keep, place);
            ++stats_.priced_out_moves;
            queue_priced(st, inst.id);
          } else if (a.held) {
            ++stats_.kept_room;
          } else {
            held_back(st, inst.id, a, now);
          }
          return 0;
        }
        ++stats_.replaces;
        slot.last_requote = now;
        return 1;
      }
      return move(inst, side, lvl, slot, target, account, value, keep, place);
    }
    // No resting order in this slot. A cancel still in flight keeps the order live (Pending*,
    // handled above), so any earlier order is final in the OMS and there is nothing left to
    // wait for, even if its terminal update never reached on_order_update().
    slot.handle = Handle<Order>{};
    slot.awaiting_terminal = false;
    slot.apply_want = false;
    if (!has_want) return 0;
    if (backing_off(st, side, now)) {
      kept_backoff(st, inst.id, side);
      return 0;
    }
    return submit_new(inst, side, lvl, slot, target, account, value, now, place);
  }

  // The slot's order is live and really the slot's: order slots are reused by later orders.
  [[nodiscard]] static bool owns(const Oms& oms,
                                 const Instrument& inst,
                                 Side side,
                                 std::uint32_t lvl,
                                 const Slot& slot) noexcept {
    if (!slot.handle.valid() || !oms.is_live(slot.handle)) return false;
    const Order& o = oms.get(slot.handle);
    return o.instrument == inst.id && o.user_tag == make_tag(side, lvl);
  }

  // Cancel-then-new of a resting quote to `target`: the cancel carries the target, so the placer
  // can hold a move towards the touch (QuoteAction::held) and leave the order where it is.
  template <class Placer>
  std::uint32_t move(const Instrument& inst,
                     Side side,
                     std::uint32_t lvl,
                     Slot& slot,
                     Level target,
                     VenueId account,
                     float value,
                     float keep,
                     Placer& place) noexcept {
    QuoteAction a{QuoteActionKind::Cancel,
                  inst.id,
                  side,
                  lvl,
                  slot.handle,
                  ClientOrderId{},
                  target.price,
                  target.qty,
                  false,
                  false,
                  false,
                  false};
    a.value = value;
    a.keep = keep;
    slot.want = target;
    slot.want_account = account;
    slot.want_value = value;
    slot.want_keep = keep;
    slot.apply_want = true;
    slot.paid = false;
    if (!place(a)) {
      if (a.priced_out) {
        slot.apply_want = false;
        ++stats_.priced_out_moves;
        queue_priced(state_[inst.id.value], inst.id);
      } else if (a.held) {
        slot.apply_want = false;
        ++stats_.kept_room;
      } else {
        ++stats_.rejected;
      }
      return 0;
    }
    slot.paid = a.paid;
    ++stats_.cancels;
    slot.awaiting_terminal = true;
    return 1;
  }

  template <class Placer>
  std::uint32_t cancel(
      const Instrument& inst, Side side, std::uint32_t lvl, Slot& slot, Placer& place) noexcept {
    QuoteAction a{QuoteActionKind::Cancel,
                  inst.id,
                  side,
                  lvl,
                  slot.handle,
                  ClientOrderId{},
                  Price{},
                  Qty{},
                  false,
                  false,
                  false,
                  false};
    if (!place(a)) {
      ++stats_.rejected;
      return 0;
    }
    ++stats_.cancels;
    slot.awaiting_terminal = true;
    return 1;
  }
  template <class Placer>
  std::uint32_t submit_new(const Instrument& inst,
                           Side side,
                           std::uint32_t lvl,
                           Slot& slot,
                           Level target,
                           VenueId account,
                           float value,
                           Timestamp now,
                           Placer& place,
                           bool deferred = false) noexcept {
    QuoteAction a{QuoteActionKind::New,
                  inst.id,
                  side,
                  lvl,
                  Handle<Order>{},
                  ClientOrderId{},
                  target.price,
                  target.qty,
                  params_.post_only,
                  deferred,
                  false,
                  false,
                  account};
    a.value = value;
    a.paid = deferred && slot.paid;
    slot.paid = false;
    if (!place(a) || !a.handle.valid()) {
      if (a.priced_out) {
        ++stats_.priced_out;
        queue_priced(state_[inst.id.value], inst.id);
      } else if (a.withheld) {
        ++stats_.kept_balance;
      } else {
        held_back(state_[inst.id.value], inst.id, a, now);
      }
      return 0;
    }
    ++stats_.news;
    slot.handle = a.handle;
    slot.cl_ord_id = a.cl_ord_id;
    slot.last_requote = now;
    slot.awaiting_terminal = false;
    return 1;
  }

  // A failed New or Replace: a rate-limited one waits in the starved queue, anything else was
  // refused; refused for its price, its side backs off.
  void held_back(InstState& st, InstrumentId id, const QuoteAction& a, Timestamp now) noexcept {
    if (!a.rate_limited) {
      ++stats_.rejected;
      if (a.refused == RejectReason::PriceCollar || a.refused == RejectReason::FatFinger)
        back_off(st, a.side, now);
      return;
    }
    ++stats_.kept_rate_limit;
    if (st.starved) return;
    st.starved = true;
    static_cast<void>(starved_.try_push(static_cast<std::uint16_t>(id.value)));
  }

  void queue_priced(InstState& st, InstrumentId id) noexcept {
    if (st.priced) return;
    st.priced = priced_.try_push(static_cast<std::uint16_t>(id.value));
  }

  void resolve_replace() noexcept {
    for (InstState& st : state_) st.replace = params_.supports_replace && !st.venue_no_replace;
  }

  QuoteParams params_;
  QuoteStats stats_{};
  InstState state_[kMaxInstruments] = {};
  RingBuffer<std::uint16_t, kMaxInstruments> starved_;  // each instrument at most once
  RingBuffer<std::uint16_t, kMaxInstruments> priced_;   // priced out, each at most once
  // Instruments with a New held back by a backoff (InstState::retry_at), each at most once, and the
  // earliest retry_at among them.
  RingBuffer<std::uint16_t, kMaxInstruments> backed_off_;
  Timestamp backoff_due_{kNever};
  bool side_held_[2] = {};  // hold_side()
};

}  // namespace fastmm
