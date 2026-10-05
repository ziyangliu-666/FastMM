// fastmm-gateway: the venue connections and their network threads, and the strategy processes
// attached over shared-memory rings (live/gateway.hpp).
#include "fastmm/live/gateway.hpp"

#include "fastmm/core/account_book.hpp"
#include "fastmm/core/balance_book.hpp"
#include "fastmm/core/book/book_snapshot.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/self_trade.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/underlying.hpp"
#include "fastmm/live/control_socket.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"

#include <fmt/format.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fastmm::live {

namespace {

volatile std::sig_atomic_t g_gw_signal = 0;
extern "C" void on_gw_signal(int sig) {
  g_gw_signal = sig;
}

struct GatewaySignals {
  struct sigaction old_int {};
  struct sigaction old_term {};
  GatewaySignals() {
    g_gw_signal = 0;
    struct sigaction sa {};
    sa.sa_handler = &on_gw_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
  }
  GatewaySignals(const GatewaySignals&) = delete;
  GatewaySignals& operator=(const GatewaySignals&) = delete;
  GatewaySignals(GatewaySignals&&) = delete;
  GatewaySignals& operator=(GatewaySignals&&) = delete;
  ~GatewaySignals() {
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
  }
};

// Client order ids (New or Replace) forwarded to one venue, newest last, to find where a
// reconciliation's sent watermark stands.
constexpr std::size_t kHistory = 1U << 14;
// Orders working at one venue, over every attachment.
constexpr std::size_t kOrderTable = 1U << 14;
// Pauses before an attachment's full order ring counts as an overflow (a few ms): the network
// thread serves every attachment, so it cannot wait on one for long.
constexpr std::uint32_t kOrderSpin = 100'000;
// The venue is asked for fresh books (the account's copies were lost) at most this often.
constexpr std::int64_t kResyncIntervalNs = 1'000'000'000;
// Executions no attached strategy has received yet, per venue (VenueRouter::parked).
constexpr std::size_t kMaxParked = 4096;

// An order of a strategy's earlier session, by the venue's id of it (gw::PastOrder).
struct PastOrder {
  InstrumentId inst;
  VenueOrderId venue_order_id;
  ClientOrderId id;
};

struct Attachment;

// The account over every venue ([gateway] max_loss, max_gross_notional, max_net_notional). Each
// venue's network thread keeps the positions of its own instruments (VenueRouter::book) and
// publishes their totals here, where the other threads' checks and the main thread read them.
struct Account {
  std::int64_t max_loss = 0;  // raw Notional; 0: off
  std::int64_t max_gross = 0;
  std::int64_t max_net = 0;
  // realized - fees of the earlier runs, from the kill file (minus this run's, after a clear-kill)
  std::atomic<std::int64_t> carry{0};
  std::size_t venue_count = 0;
  std::array<VenueSlot*, 8> slots{};  // their reactors are woken by a trip
  std::atomic<bool> tripped{false};
  std::atomic<std::int64_t> trip_net{0};                  // the net PnL that tripped it
  std::atomic<KillReason> trip_reason{KillReason::None};  // GatewayMaxLoss, GatewayOperator
  // A venue's totals in one currency, as its network thread publishes them.
  struct alignas(kCacheLine) Totals {
    std::atomic<std::int64_t> realized{0};
    std::atomic<std::int64_t> unrealized{0};
    std::atomic<std::int64_t> fees{0};
    std::atomic<std::int64_t> gross{0};
    std::atomic<std::int64_t> net{0};
  };
  // Raw Notionals: a venue's totals in the reporting currency, or its one currency without
  // [accounting].
  struct Sum {
    std::int64_t realized = 0;
    std::int64_t unrealized = 0;
    std::int64_t fees = 0;
    std::int64_t gross = 0;
    std::int64_t net = 0;
  };
  // [accounting]: each currency's rate is the mid of its FX source, published by the network thread
  // of the source's venue. `valid`: that book is valid now; `at_ns`: steady time of its last mark.
  // The mid stays when the book goes: PnL keeps the last rate, new exposure needs a current one.
  struct alignas(kCacheLine) Rate {
    std::atomic<std::int64_t> mid{0};  // raw Price; 0: never known
    std::atomic<std::int64_t> at_ns{0};
    std::atomic<bool> valid{false};
  };
  FxPlan fx;                  // set before the network threads start
  std::int64_t stale_ns = 0;  // [risk] stale_md_ms: a mark older than this is not current; 0: off
  std::array<Rate, kMaxCurrencies> rates;
  std::array<std::array<Totals, kMaxCurrencies>, 8> venues;      // per venue, per currency
  std::array<std::atomic<std::int64_t>, kMaxInstruments> qty{};  // raw Qty per instrument
  // [gateway.underlying]: the net position per base asset over every venue. Each network thread
  // publishes, for its own instruments of an underlying, the leaves of the orders working at the
  // venue per side (raw Qty) and the mid of a valid book with its steady time (raw Price; the
  // mark an inverse contract is converted at). Set before the network threads start.
  UnderlyingPlan und;
  std::array<std::array<std::atomic<std::int64_t>, 2>, kMaxInstruments> open{};
  std::array<std::atomic<std::int64_t>, kMaxInstruments> mark{};
  std::array<std::atomic<std::int64_t>, kMaxInstruments> mark_ns{};

  // [gateway.shared]: instruments several strategies trade at once. Each strategy has a slot, by
  // its [engine] name, for this gateway's life; an execution of a shared instrument counts towards
  // the slot of the strategy whose order it names (share), or towards nobody (unattributed). The
  // instrument's network thread publishes both with every change, and the account's position less
  // their sum (unexplained: 0 unless a venue's position record says otherwise). Set before the
  // network threads start, except epoch_slot, which the main thread fills in before an attachment
  // whose epochs it names is routed.
  static constexpr std::size_t kMaxSlots = 64;
  std::array<bool, kMaxInstruments> shared{};
  std::array<std::uint8_t, kMaxInstruments> primary{};        // its primary's slot + 1, 0 for none
  std::array<std::atomic<std::uint8_t>, 65536> epoch_slot{};  // a session epoch's slot + 1
  std::array<std::array<std::atomic<std::int64_t>, kMaxSlots>, kMaxInstruments> share{};  // raw Qty
  std::array<std::atomic<std::int64_t>, kMaxInstruments> unattributed{};
  std::array<std::atomic<std::int64_t>, kMaxInstruments> unexplained{};

  [[nodiscard]] bool exposure_limits() const noexcept { return max_gross > 0 || max_net > 0; }
  [[nodiscard]] std::size_t currencies() const noexcept { return fx.active() ? fx.count : 1; }
  // The rate PnL is converted at: the last one known.
  [[nodiscard]] FxRate rate(std::size_t c) const noexcept {
    if (c == 0) return FxRate::identity();
    return FxRate::from_mid(Price::from_raw(rates[c].mid.load(std::memory_order_relaxed)),
                            fx.sources[c].invert);
  }
  // The rate new exposure is measured at: unknown while the source's book is not valid, or its
  // last mark is older than fx.stale ([accounting] stale_fx_ms).
  [[nodiscard]] FxRate current_rate(std::size_t c, std::int64_t now_ns) const noexcept {
    if (c == 0) return FxRate::identity();
    const Rate& r = rates[c];
    if (!r.valid.load(std::memory_order_acquire)) return {};
    if (fx.stale.ns > 0 && now_ns - r.at_ns.load(std::memory_order_relaxed) > fx.stale.ns)
      return {};
    return rate(c);
  }
  // The mark an inverse contract of an underlying is converted at: the last mid of a valid book,
  // not older than stale_ns; zero when there is none.
  [[nodiscard]] Price current_mark(InstrumentId id, std::int64_t now_ns) const noexcept {
    const std::int64_t m = mark[id.value].load(std::memory_order_relaxed);
    if (m <= 0) return {};
    if (stale_ns > 0 && now_ns - mark_ns[id.value].load(std::memory_order_relaxed) > stale_ns)
      return {};
    return Price::from_raw(m);
  }
  // Underlying `u` in base units (raw): its net position and, with `side`, the leaves working on
  // that side, over every venue. False when an inverse contract with a position (or leaves) has no
  // current mark.
  bool underlying_position(std::size_t u,
                           const InstrumentTable& insts,
                           const Side* side,
                           std::int64_t now_ns,
                           std::int64_t& net,
                           std::int64_t& working) const noexcept {
    net = working = 0;
    bool known = true;
    for (const InstrumentId j : und.instruments(u)) {
      const Instrument& inst = insts.get(j);
      const Price m = current_mark(j, now_ns);
      std::int64_t b = 0;
      if (to_base_units(inst, qty[j.value].load(std::memory_order_relaxed), m, b)) {
        net += b;
      } else {
        known = false;
      }
      if (side == nullptr) continue;
      const std::int64_t leaves =
          open[j.value][static_cast<std::size_t>(*side)].load(std::memory_order_relaxed);
      if (to_base_units(inst, leaves, m, b)) {
        working += b;
      } else {
        known = false;
      }
    }
    return known;
  }
  // Venue `i`'s totals, converted.
  [[nodiscard]] Sum venue_sum(std::size_t i) const noexcept {
    Sum s;
    for (std::size_t c = 0; c < currencies(); ++c) {
      const Totals& t = venues[i][c];
      const FxRate r = rate(c);
      const auto conv = [&](const std::atomic<std::int64_t>& a) {
        return convert(Notional::from_raw(a.load(std::memory_order_relaxed)), r).raw;
      };
      s.realized += conv(t.realized);
      s.unrealized += conv(t.unrealized);
      s.fees += conv(t.fees);
      s.gross += conv(t.gross);
      s.net += conv(t.net);
    }
    return s;
  }
  // Net PnL of the account: the carry and every venue's realized + unrealized - fees.
  [[nodiscard]] std::int64_t net_pnl() const noexcept {
    std::int64_t n = carry.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < venue_count; ++i) {
      const Sum t = venue_sum(i);
      n += t.realized + t.unrealized - t.fees;
    }
    return n;
  }
};

// What one venue's network thread keeps about one attachment. It lives in the Attachment and is
// added to and removed from the venue's router by tasks posted to that thread; only the atomics
// are read elsewhere (the main thread's checks and logs).
struct Route {
  Attachment* att = nullptr;
  std::uint16_t epoch = 0;
  ShmRing* md = nullptr;
  ShmRing* order = nullptr;
  ShmRing* out = nullptr;
  Waker* waker = nullptr;         // the engine's, when it blocks while idle
  bool dirty = false;             // events routed here since the last notify
  bool snapshot_pending = false;  // it asked for a reconciliation that has not begun
  bool in_snapshot = false;       // it receives the reconciliation being routed
  bool md_gap = false;  // its md ring dropped: nothing more until its Resyncing and books went out
  std::atomic<std::uint64_t> md_dropped{0};
  // Its orders the gateway refused, in the order of kStatusGatewayRefusalReasons.
  std::array<std::atomic<std::uint64_t>, kStatusGatewayRefusals> refused{};
  std::atomic<bool> overflow{false};  // its order ring stayed full: it has to go
  // Where its own history starts on this venue (venue ms): the replay start it sent, or the venue's
  // time at its attach when it restored nothing. An execution naming no live order from before
  // that is its store's (or none of its business), and the trade ids its store listed in the
  // overlap before it are too.
  std::int64_t replay_from_ms = 0;
  const std::unordered_set<std::string>* known = nullptr;
  std::uint8_t slot = 0;                // its [engine] name's slot (Account::share)
  std::bitset<kMaxInstruments> claims;  // the instruments it trades
  // Executions it was given from VenueRouter::parked (fill_key): a replay's copy of one is not
  // given again.
  std::unordered_set<std::string> unparked;
};

// One order the gateway forwarded (or a reconciliation reported), for the account guards and for
// cancelling a detached strategy's orders.
struct GwOrder {
  static constexpr std::uint8_t kCancelSent = 1U << 0;
  static constexpr std::uint8_t kRests = 1U << 1;  // it can rest in the book (not IOC, FOK, market)
  static constexpr std::uint8_t kReduceOnly = 1U << 2;
  static constexpr std::uint8_t kAcked = 1U << 3;  // the venue acknowledged it (or filled it)
  InstrumentId inst{};
  std::uint16_t epoch = 0;
  std::uint8_t flags = 0;
  Side side = Side::Buy;
  Price price{};
  std::int64_t notional = 0;           // raw Notional of the working quantity
  std::int64_t replaced_notional = 0;  // of the order this one replaces, restored on a reject
  Qty leaves{};                        // working quantity ([gateway.underlying])
  Qty replaced_leaves{};               // of the order this one replaces, restored on a reject
  std::int64_t hold = 0;               // what it holds of the account's balance (BalanceBook::hold)
  std::uint64_t g = 0;                 // 1-based forward index; 1 when learned from a snapshot
  std::uint32_t gen = 0;               // the last reconciliation that reported it
  ClientOrderId replaces{};
  VenueOrderId venue_order_id{};
};

struct CancelReq {
  ClientOrderId id;
  InstrumentId inst;
  VenueOrderId venue_order_id;
};

// A venue's router: its network thread's view of the attachments and of the orders at the venue.
// Everything but the atomics belongs to that thread.
struct VenueRouter {
  VenueSlot* slot = nullptr;
  VenueId vid{};
  const InstrumentTable* insts = nullptr;
  StaticVector<Route*, gw::kMaxAttachments> routes;
  std::array<Route*, kMaxInstruments> owner{};

  std::vector<ClientOrderId> hist = std::vector<ClientOrderId>(kHistory);
  std::uint64_t forwarded = 0;
  OpenHashMap<ClientOrderId, GwOrder, kOrderTable> orders;
  std::int64_t open_notional = 0;
  TokenBucket rate;
  std::int64_t max_open_notional = 0;  // raw; 0: off

  // The reconciliation being routed.
  bool snap_bounded = false;
  bool snap_known = false;  // the sent watermark was found in hist
  std::uint64_t snap_p = 0;
  std::uint32_t gen = 0;
  bool sweep_pending = false;   // the gateway asked for one (a detach): nobody receives it
  bool quiet_md_state = false;  // inside a resync_books() the gateway called
  bool want_resync = false;     // the account's books were lost: the venue resnapshots them
  std::int64_t last_resync_ns = 0;
  std::vector<CancelReq> cancels;  // the gateway's own, sent by the hook
  bool dry_run = false;

  // The account's positions on this venue. A replayed execution of an instrument older than the
  // history its account position was seeded from (the seeding strategy's replay start and the
  // trade ids its store listed) is in that seed already.
  Account* acct = nullptr;
  std::unique_ptr<AccountBook> book;
  // The book events for the account's marks, copied out in the md drain and applied by the hook
  // after the engines have been woken: the books are not on the way to a strategy. Brought up to
  // date, they are what an attachment that received every md event so far holds, which is what
  // an attaching or lagging one is given (push_books).
  std::unique_ptr<MsgRing> acct_md;
  bool acct_md_lost = false;  // it was full: the books start over from a resync
  std::array<std::int64_t, kMaxInstruments> seed_from_ms{};
  std::array<std::shared_ptr<const std::unordered_set<std::string>>, kMaxInstruments> seed_known{};
  bool killed = false;  // it acted on the account's trip

  // The account's balances on this venue (core/balance_book.hpp): the venue's reports moved by the
  // orders and fills of every attached strategy, for the balance guard. Published for the status
  // file every kBalancePubNs while the venue reports.
  std::unique_ptr<BalanceBook> balances = std::make_unique<BalanceBook>();
  struct BalancePub {
    std::uint32_t count = 0;
    std::array<LiveBalance, kMaxLiveBalances> rows{};
  };
  Seqlocked<BalancePub> balance_pub;
  std::int64_t balance_pub_ns = 0;
  // The account's perp table rows of this venue (AccountBook::perps), for the status file.
  struct PerpPub {
    std::uint32_t count = 0;
    std::array<LivePerp, kMaxLivePerps> rows{};
  };
  Seqlocked<PerpPub> perp_pub;
  std::int64_t perp_pub_ns = 0;

  // Shared instruments. The live attachment of each slot. Per instrument: each slot that seeded
  // the account's position with its store, and the history that seed holds (its replay start and
  // the trade ids its store listed); the sum of every slot's share and the unattributed part; per
  // side, the orders of every attachment that can rest, for the self-trade check.
  std::array<Route*, Account::kMaxSlots> by_slot{};
  struct ShareSeed {
    std::uint8_t slot = 0;
    std::int64_t from_ms = 0;
    std::shared_ptr<const std::unordered_set<std::string>> known;
  };
  std::array<std::vector<ShareSeed>, kMaxInstruments> share_seeds;
  std::array<std::int64_t, kMaxInstruments> explained{};
  std::array<RestingOrders, kMaxInstruments> resting;
  std::int64_t start_ms = 0;  // the gateway's start: older executions are nobody's business here

  // Owned instruments whose account position a first owner seeded. Before that the account books
  // none of their executions: they wait in `parked` for the seed.
  std::bitset<kMaxInstruments> seeded;
  // The orders of earlier sessions the attached strategies' stores listed, by instrument and the
  // venue's id of the order. An execution naming no client order id (a venue's trade history can
  // give the venue's id alone; Binance's does) that names one of them is that order's: after a
  // gateway restart nothing else says whose it is.
  std::map<std::pair<std::uint32_t, std::string>, ClientOrderId> past_orders;
  // Executions no attached strategy received: of an order of a session whose strategy is not
  // attached, naming an order nobody has claimed, or naming none while the instrument has no
  // owner (or primary) attached. Each attach looks through them for its own, so an execution
  // reaches its strategy whether the replay that found it ran before or after the attach. Oldest
  // first, at most kMaxParked (the oldest goes).
  std::deque<OrderFillMsg> parked;
  std::atomic<std::uint64_t> parked_count{0};
  std::atomic<std::uint64_t> parked_dropped{0};

  std::atomic<std::uint64_t> md_discarded{0};
  std::atomic<std::uint64_t> order_discarded{0};
  std::atomic<std::uint64_t> unrouted{0};
  std::atomic<std::uint64_t> gateway_cancels{0};
  std::atomic<std::uint64_t> refused_rate{0};
  std::atomic<std::uint64_t> refused_notional{0};
  std::atomic<std::uint64_t> refused_owner{0};
  std::atomic<std::uint64_t> refused_killed{0};
  std::atomic<std::uint64_t> refused_gross{0};
  std::atomic<std::uint64_t> refused_net{0};
  std::atomic<std::uint64_t> refused_fx{0};
  std::atomic<std::uint64_t> refused_underlying{0};
  std::atomic<std::uint64_t> refused_underlying_mark{0};
  std::atomic<std::uint64_t> refused_self_trade{0};
  std::atomic<std::uint64_t> refused_balance{0};
  std::atomic<std::uint64_t> account_skipped{0};  // replayed fills / funding its seed holds
  std::atomic<std::uint64_t> account_md_lost{0};  // times acct_md was full
  std::atomic<std::uint64_t> untracked{0};        // the order table was full
  std::atomic<std::uint64_t> stale_replays{0};    // replayed fills older than their owner's history

  [[nodiscard]] Route* by_epoch(ClientOrderId id) const noexcept {
    const std::uint16_t e = cl_ord_id_epoch(id);
    if (e == 0) return nullptr;
    for (Route* r : routes) {
      if (r->epoch == e) return r;
    }
    return nullptr;
  }
  [[nodiscard]] Route* owner_of(InstrumentId id) const noexcept {
    return id.value < owner.size() ? owner[id.value] : nullptr;
  }
  [[nodiscard]] bool shared(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && acct->shared[id.value];
  }
};

// ---- network thread: the account --------------------------------------------------------------

// A trip: every network thread refuses orders from here on and acts on it in its next hook.
// False when the account was tripped already.
bool trip_account(Account& a, std::int64_t net, KillReason why) noexcept {
  // The reason first: a thread that sees the trip reads it.
  KillReason none = KillReason::None;
  if (!a.trip_reason.compare_exchange_strong(none, why, std::memory_order_acq_rel)) return false;
  a.trip_net.store(net, std::memory_order_relaxed);
  a.tripped.store(true, std::memory_order_release);
  for (std::size_t i = 0; i < a.venue_count; ++i) a.slots[i]->reactor->wake();
  return true;
}

// Publishes this venue's totals, per currency with [accounting], and checks the account's loss.
void publish_account(VenueRouter& v) noexcept {
  Account& a = *v.acct;
  const PositionTracker& p = v.book->positions();
  const auto store = [](Account::Totals& t, const PositionTracker::Totals& n) {
    t.realized.store(n.realized.raw, std::memory_order_relaxed);
    t.unrealized.store(n.unrealized.raw, std::memory_order_relaxed);
    t.fees.store(n.fees.raw, std::memory_order_relaxed);
    t.gross.store(n.gross.raw, std::memory_order_relaxed);
    t.net.store(n.net.raw, std::memory_order_relaxed);
  };
  if (p.converting()) {
    for (std::size_t c = 0; c < a.currencies(); ++c) store(a.venues[v.vid.value][c], p.native(c));
  } else {
    store(a.venues[v.vid.value][0],
          PositionTracker::Totals{p.total_realized(),
                                  p.total_unrealized(),
                                  p.total_fees(),
                                  p.gross_exposure(),
                                  p.net_exposure()});
  }
  if (a.max_loss > 0 && !a.tripped.load(std::memory_order_relaxed)) {
    const std::int64_t net = a.net_pnl();
    if (net <= -a.max_loss) static_cast<void>(trip_account(a, net, KillReason::GatewayMaxLoss));
  }
}

// A shared instrument: the account's position less every strategy's share and the unattributed
// part.
void publish_unexplained(VenueRouter& v, InstrumentId id) noexcept {
  v.acct->unexplained[id.value].store(v.book->positions().get(id).qty.raw - v.explained[id.value],
                                      std::memory_order_relaxed);
}

// A shared instrument's position changes by `d` (raw Qty) for `tag`: slot + 1, 0 for nobody.
void add_share(VenueRouter& v, InstrumentId id, std::uint8_t tag, std::int64_t d) noexcept {
  Account& a = *v.acct;
  std::atomic<std::int64_t>& to = tag != 0 ? a.share[id.value][tag - 1U] : a.unattributed[id.value];
  to.store(to.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);  // one writer
  v.explained[id.value] += d;
}

void set_account_position(VenueRouter& v, InstrumentId id, Qty qty, Price avg) noexcept {
  v.book->set_position(id, qty, avg);
  v.acct->qty[id.value].store(v.book->positions().get(id).qty.raw, std::memory_order_relaxed);
  if (v.shared(id)) publish_unexplained(v, id);
  publish_account(v);
}

// An execution the account books for the first time moves the account's balances.
void balance_fill(VenueRouter& v,
                  const OrderFillMsg& m,
                  std::int64_t before,
                  std::int64_t after) noexcept {
  const FeeAsset fee_asset = m.fee_asset > FeeAsset::Other ? FeeAsset::Quote : m.fee_asset;
  v.balances->on_fill(m.hdr.instrument,
                      v.insts->get(m.hdr.instrument),
                      m.side,
                      m.price,
                      m.qty,
                      m.fee,
                      fee_asset,
                      before,
                      after,
                      m.hdr.exch_ts);
}

// A strategy's store joins a shared instrument's account position, as its share.
void seed_share(VenueRouter& v, InstrumentId id, std::uint8_t slot, Qty qty, Price avg) noexcept {
  v.book->add_position(id, qty, avg);
  v.acct->qty[id.value].store(v.book->positions().get(id).qty.raw, std::memory_order_relaxed);
  add_share(v, id, static_cast<std::uint8_t>(slot + 1U), qty.raw);
  publish_unexplained(v, id);
  publish_account(v);
}

// A replayed execution that the history of an attachment holds already: older than where its
// replay started, or among the trade ids its store listed.
bool held_by(const Route& r, const OrderFillMsg& m) {
  return m.hdr.exch_ts.ns / 1'000'000 < r.replay_from_ms ||
         (r.known != nullptr && r.known->contains(std::string(m.exec_id.view())));
}

// A shared instrument's execution, booked once and counted towards `tag` (slot + 1; 0 nobody). A
// replayed one that the seed of its slot holds (older than that strategy's replay start or among
// its store's trade ids), or, without such a seed, older than the gateway (in the venue's clock,
// which the execution's time is), is not booked. One the account booked for nobody that now
// reaches a strategy (`delivered`) becomes that strategy's.
void account_fill_shared(VenueRouter& v, const OrderFillMsg& m, std::uint8_t tag, bool delivered) {
  const InstrumentId id = m.hdr.instrument;
  if (!v.insts->contains(id)) return;
  if ((m.flags & OrderFillMsg::kReplayed) != 0 && m.hdr.exch_ts.ns > 0) {
    const std::int64_t ms = m.hdr.exch_ts.ns / 1'000'000;
    const VenueRouter::ShareSeed* seed = nullptr;
    for (const VenueRouter::ShareSeed& s : v.share_seeds[id.value]) {
      if (tag != 0 && s.slot + 1U == tag) seed = &s;
    }
    const bool held = seed != nullptr ? ms < seed->from_ms ||
                                            (seed->known != nullptr &&
                                             seed->known->contains(std::string(m.exec_id.view())))
                                      : ms < v.start_ms + v.slot->venue->status().clock_offset_ms;
    if (held) {
      v.account_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  if (const std::uint8_t* was = v.book->booked_as(m)) {
    if (*was == 0 && tag != 0 && delivered) {
      v.book->retag(m, tag);
      const std::int64_t d = AccountBook::position_change(m);
      add_share(v, id, 0, -d);
      add_share(v, id, tag, d);
    }
    return;
  }
  const std::int64_t before = v.book->positions().get(id).qty.raw;
  if (!v.book->first_time(m, tag)) return;
  v.book->book(m);
  const std::int64_t after = v.book->positions().get(id).qty.raw;
  balance_fill(v, m, before, after);
  add_share(v, id, tag, after - before);
  v.acct->qty[id.value].store(after, std::memory_order_relaxed);
  publish_unexplained(v, id);
  publish_account(v);
}

// Every execution once, streamed or replayed, whichever strategy it is routed to (or none).
void account_fill(VenueRouter& v, const OrderFillMsg& m) {
  const InstrumentId id = m.hdr.instrument;
  if (!v.insts->contains(id)) return;
  if ((m.flags & OrderFillMsg::kReplayed) != 0 && m.hdr.exch_ts.ns > 0) {
    const auto& known = v.seed_known[id.value];
    if (m.hdr.exch_ts.ns / 1'000'000 < v.seed_from_ms[id.value] ||
        (known != nullptr && known->contains(std::string(m.exec_id.view())))) {
      v.account_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  const std::int64_t before = v.book->positions().get(id).qty.raw;
  if (!v.book->first_time(m)) return;
  v.book->book(m);
  const std::int64_t after = v.book->positions().get(id).qty.raw;
  balance_fill(v, m, before, after);
  v.acct->qty[id.value].store(after, std::memory_order_relaxed);
  publish_account(v);
}

// A funding payment's id as the known-id lists (a store's resume, the account's seed) hold it.
std::string known_funding_id(const FundingMsg& m) {
  std::string id(kFundingIdPrefix);
  id += m.funding_id.view();
  return id;
}

// Every funding payment once, as account_fill books executions: one from the venue's history that
// the account's seed holds already (older than it, or listed by the seeding store) is skipped.
void account_funding(VenueRouter& v, const FundingMsg& m) {
  const InstrumentId id = m.hdr.instrument;
  if (!v.insts->contains(id)) return;
  if ((m.flags & FundingMsg::kReplayed) != 0 && m.hdr.exch_ts.ns > 0) {
    const auto& known = v.seed_known[id.value];
    if (m.hdr.exch_ts.ns / 1'000'000 < v.seed_from_ms[id.value] ||
        (known != nullptr && known->contains(known_funding_id(m)))) {
      v.account_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  if (!v.book->first_time(m)) return;
  if (!v.book->book(m)) {
    FASTMM_LOG_ERROR("gateway: funding {} in {} on instrument {} is not in its settlement currency",
                     m.funding_id.view(),
                     m.asset.view(),
                     id.value);
    return;
  }
  publish_account(v);
}

// [accounting]: the FX sources of this venue whose books are gone are not current any more.
void publish_rates(VenueRouter& v) noexcept {
  Account& a = *v.acct;
  for (std::size_t c = 1; c < a.fx.count; ++c) {
    const InstrumentId id = a.fx.sources[c].instrument;
    if (!id.valid() || !v.insts->contains(id) || v.insts->get(id).venue != v.vid) continue;
    const AccountBook::Book* b = v.book->book(id);
    if (b == nullptr || !b->is_valid()) a.rates[c].valid.store(false, std::memory_order_release);
  }
}

// The account's marks: the book events the md drain set aside. A set-aside ring that overflowed
// loses deltas, so the account's books start over from the snapshots of a resync.
std::size_t mark_account(VenueRouter& v) noexcept {
  std::size_t n = 0;
  bool marked = false;
  MsgRing& ring = *v.acct_md;
  Account& a = *v.acct;
  // The clock the venue marks are aged on, read once per batch.
  Timestamp now{};
  while (const std::byte* p = ring.try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    if (!now.valid()) now = steady_now();
    if (h.type == EventType::ConnectionState) {
      v.book->on_connection_state(msg_cast<ConnectionStateMsg>(&h));
      if (a.fx.active()) publish_rates(v);
    } else if (h.type == EventType::PerpState) {
      marked = v.book->on_perp_state(msg_cast<PerpStateMsg>(&h), now) || marked;
    } else {
      const bool valid = v.book->on_book(msg_cast<BookDeltaMsg>(&h), now);
      marked = valid || marked;
      if (valid && a.und.underlying_of(h.instrument) >= 0) {
        a.mark[h.instrument.value].store(v.book->book(h.instrument)->mid().raw,
                                         std::memory_order_relaxed);
        a.mark_ns[h.instrument.value].store(steady_now().ns, std::memory_order_relaxed);
      }
      if (const int c = a.fx.priced_by(h.instrument); FASTMM_UNLIKELY(c > 0)) {
        Account::Rate& r = a.rates[static_cast<std::size_t>(c)];
        if (valid) {
          r.mid.store(v.book->book(h.instrument)->mid().raw, std::memory_order_relaxed);
          r.at_ns.store(steady_now().ns, std::memory_order_relaxed);
        }
        r.valid.store(valid, std::memory_order_release);
      }
    }
    ring.release();
    ++n;
  }
  if (FASTMM_UNLIKELY(v.acct_md_lost)) {
    v.acct_md_lost = false;
    v.account_md_lost.fetch_add(1, std::memory_order_relaxed);
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, InstrumentId::invalid(), v.vid);
    m.state = ConnState::Resyncing;
    m.channel = 0;
    v.book->on_connection_state(m);
    if (a.fx.active()) publish_rates(v);
    v.want_resync = true;
  }
  if (marked) publish_account(v);
  return n;
}

// The account's exposure limits for one order on this venue, `notional` in the reporting currency:
// the totals every venue published (this one's are its positions: it publishes every change).
RejectReason check_account(const VenueRouter& v, InstrumentId inst, Side side, Notional notional) {
  const Account& a = *v.acct;
  const PositionTracker& p = v.book->positions();
  Notional gross{};
  Notional net{};
  for (std::size_t i = 0; i < a.venue_count; ++i) {
    const Account::Sum t = a.venue_sum(i);
    gross += Notional::from_raw(t.gross);
    net += Notional::from_raw(t.net);
  }
  return check_exposure(p.get(inst),
                        side,
                        notional,
                        gross,
                        net,
                        Notional::from_raw(a.max_gross),
                        Notional::from_raw(a.max_net));
}

// [gateway.underlying]: the account's net position in the order's base asset, over every venue,
// with the leaves working on the order's side and the order itself (the leaves of the order a
// replace takes over excluded), as RiskEngine checks [risk.underlying] for one strategy.
RejectReason check_underlying(
    const VenueRouter& v, InstrumentId inst, Side side, Qty qty, Qty replaced_leaves) {
  const Account& a = *v.acct;
  const int u = a.und.underlying_of(inst);
  if (u < 0 || !a.und.max_net[static_cast<std::size_t>(u)].is_positive()) return RejectReason::None;
  const std::int64_t now_ns = steady_now().ns;
  std::int64_t net = 0;
  std::int64_t working = 0;
  std::int64_t add = 0;
  if (!a.underlying_position(static_cast<std::size_t>(u), *v.insts, &side, now_ns, net, working) ||
      !to_base_units(
          v.insts->get(inst), qty.raw - replaced_leaves.raw, a.current_mark(inst, now_ns), add))
    return RejectReason::GatewayUnderlyingMarkUnknown;
  const std::int64_t worst = net + sign(side) * (working + add);
  return underlying_exceeds(a.und.max_net[static_cast<std::size_t>(u)].raw, net, worst)
             ? RejectReason::GatewayUnderlyingNet
             : RejectReason::None;
}

// ---- network thread: inbound ------------------------------------------------------------------

void push_md(Route& r, const EventHeader& h) noexcept {
  if (r.md_gap) {
    r.md_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!r.md->try_push(&h, h.len)) {
    r.md_gap = true;
    r.md_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  r.dirty = true;
}

// Order events are never dropped: a strategy that stops reading its ring is detached.
void push_order(Route& r, const EventHeader& h) noexcept {
  if (r.overflow.load(std::memory_order_relaxed)) return;
  for (std::uint32_t i = 0; !r.order->try_push(&h, h.len); ++i) {
    if (i == kOrderSpin) {
      r.overflow.store(true, std::memory_order_release);
      return;
    }
    _mm_pause();
  }
  r.dirty = true;
}

// EventSink drain hook of the md sink: every event to every attachment.
void drain_md(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  MsgRing& ring = *v.slot->md_ring;
  while (const std::byte* p = ring.try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    if (v.routes.empty()) {
      v.md_discarded.fetch_add(1, std::memory_order_relaxed);
    } else if (h.type != EventType::ConnectionState || !v.quiet_md_state) {
      for (Route* r : v.routes) push_md(*r, h);
    }
    if (h.type == EventType::BookDelta || h.type == EventType::BookSnapshot ||
        h.type == EventType::ConnectionState || h.type == EventType::PerpState) {
      if (!v.acct_md_lost && !v.acct_md->try_push(&h, h.len)) v.acct_md_lost = true;
    }
    ring.release();
  }
}

void queue_cancel(VenueRouter& v, ClientOrderId id, InstrumentId inst, const VenueOrderId& voi) {
  if (GwOrder* o = v.orders.find(id)) {
    o->flags |= GwOrder::kCancelSent;
  }
  v.cancels.push_back(CancelReq{id, inst, voi});
}

// A cancel the gateway has not sent for this order yet (an order it does not track always).
void cancel_once(VenueRouter& v, ClientOrderId id, InstrumentId inst, const VenueOrderId& voi) {
  if (const GwOrder* o = v.orders.find(id); o != nullptr && (o->flags & GwOrder::kCancelSent) != 0)
    return;
  queue_cancel(v, id, inst, voi);
}

// [gateway.underlying]: the leaves working at the venue per instrument and side change by `d`.
void add_leaves(VenueRouter& v, const GwOrder& o, std::int64_t d) noexcept {
  if (d == 0 || !v.acct->und.active() || v.acct->und.of[o.inst.value] == 0) return;
  v.acct->open[o.inst.value][static_cast<std::size_t>(o.side)].fetch_add(d,
                                                                         std::memory_order_relaxed);
}

// An order's hold on the account's balance follows its working quantity, at the venue time `at` of
// the event that changed it (invalid: the gateway's own bookkeeping, always applied).
void sync_hold(VenueRouter& v, GwOrder& o, Timestamp at) noexcept {
  if (!v.insts->contains(o.inst)) return;
  Price px = o.price;
  if (!px.is_positive()) {
    const AccountBook::Book* b = v.book->book(o.inst);
    if (b != nullptr && b->is_valid()) px = b->mid();
  }
  const std::int64_t h = v.balances->hold(
      o.inst, v.insts->get(o.inst), o.side, px, o.leaves, (o.flags & GwOrder::kReduceOnly) != 0);
  if (h == o.hold) return;
  v.balances->move_hold(o.inst, o.side, o.hold, h, at, (o.flags & GwOrder::kAcked) == 0);
  o.hold = h;
}

// The venue has taken the order: the balances stop keeping its hold on top of their reports.
void acknowledge(VenueRouter& v, GwOrder& o, Timestamp at) noexcept {
  if ((o.flags & GwOrder::kAcked) != 0) return;
  o.flags |= GwOrder::kAcked;
  v.balances->acknowledge(o.inst, o.side, o.hold, at);
}

void set_leaves(VenueRouter& v, GwOrder& o, Qty leaves, Timestamp at = {}) noexcept {
  add_leaves(v, o, leaves.raw - o.leaves.raw);
  o.leaves = leaves;
  sync_hold(v, o, at);
}

// A shared instrument's orders that can rest, for the self-trade check: `id` leaves the list of its
// side, and is in it while it rests with a working quantity.
void drop_resting(VenueRouter& v, ClientOrderId id, const GwOrder& o) noexcept {
  if (v.shared(o.inst)) v.resting[o.inst.value].remove(id, o.side);
}
void sync_resting(VenueRouter& v, ClientOrderId id, const GwOrder& o) {
  if (!v.shared(o.inst)) return;
  if ((o.flags & GwOrder::kRests) != 0 && o.leaves.is_positive()) {
    v.resting[o.inst.value].set(id, o.side, o.price, o.epoch);
  } else {
    v.resting[o.inst.value].remove(id, o.side);
  }
}

void untrack(VenueRouter& v, ClientOrderId id, Timestamp at = {}) noexcept {
  if (const GwOrder* o = v.orders.find(id)) {
    drop_resting(v, id, *o);
    v.open_notional -= o->notional;
    add_leaves(v, *o, -o->leaves.raw);
    if (o->hold != 0)
      v.balances->move_hold(o->inst, o->side, o->hold, 0, at, (o->flags & GwOrder::kAcked) == 0);
    v.orders.erase(id);
  }
}

// A reconciliation: Begin, rows, End. Each recipient gets its own rows under a Begin carrying its
// own watermark; rows of an epoch nobody holds are cancelled.
void route_reconcile(VenueRouter& v, const ReconcileMsg& m) {
  switch (m.kind) {
    case ReconcileMsg::Kind::Begin: {
      ++v.gen;
      v.snap_bounded = (m.flags & ReconcileMsg::kSentWatermark) != 0;
      v.snap_known = false;
      v.snap_p = 0;
      if (v.snap_bounded) {
        if (!m.sent_watermark.valid()) {
          v.snap_known = true;  // nothing had been taken
        } else {
          const std::uint64_t lo = v.forwarded > kHistory ? v.forwarded - kHistory : 0;
          for (std::uint64_t j = v.forwarded; j > lo; --j) {
            if (v.hist[(j - 1) % kHistory] == m.sent_watermark) {
              v.snap_known = true;
              v.snap_p = j;
              break;
            }
          }
        }
      }
      bool any = false;
      for (Route* r : v.routes) any = any || r->snapshot_pending;
      const bool all = !any && !v.sweep_pending;
      v.sweep_pending = false;
      for (Route* r : v.routes) {
        if (!all && !r->snapshot_pending) continue;
        r->snapshot_pending = false;
        r->in_snapshot = true;
        ReconcileMsg b = m;
        if (v.snap_bounded) {
          // Its last order the venue had taken when it asked; none (or not findable): nothing of
          // its can be concluded from the snapshot, which is the safe side.
          ClientOrderId wm{};
          if (v.snap_known) {
            const std::uint64_t lo = v.forwarded > kHistory ? v.forwarded - kHistory : 0;
            for (std::uint64_t j = v.snap_p; j > lo; --j) {
              const ClientOrderId id = v.hist[(j - 1) % kHistory];
              if (cl_ord_id_epoch(id) == r->epoch) {
                wm = id;
                break;
              }
            }
          }
          b.sent_watermark = wm;
        }
        push_order(*r, b.hdr);
      }
      return;
    }
    case ReconcileMsg::Kind::OpenOrder: {
      const Qty leaves = m.orig_qty - m.cum_qty;
      if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
        o->gen = v.gen;
        if (!m.venue_order_id.empty()) o->venue_order_id = m.venue_order_id;
        if (v.insts->contains(m.hdr.instrument) && leaves.is_positive()) {
          const std::int64_t n = v.insts->get(m.hdr.instrument).notional(m.price, leaves).raw;
          v.open_notional += n - o->notional;
          o->notional = n;
          set_leaves(v, *o, leaves);
        }
      }
      Route* r = v.by_epoch(m.cl_ord_id);
      if (r != nullptr) {
        if (v.orders.find(m.cl_ord_id) == nullptr && v.insts->contains(m.hdr.instrument)) {
          GwOrder o;
          o.inst = m.hdr.instrument;
          o.epoch = r->epoch;
          o.side = m.side;
          o.price = m.price;
          o.notional = leaves.is_positive()
                           ? v.insts->get(m.hdr.instrument).notional(m.price, leaves).raw
                           : 0;
          o.leaves = leaves.is_positive() ? leaves : Qty{};
          o.g = 1;
          o.gen = v.gen;
          o.venue_order_id = m.venue_order_id;
          o.flags = GwOrder::kRests | GwOrder::kAcked;
          if (v.orders.insert(m.cl_ord_id, o).second) {
            v.open_notional += o.notional;
            add_leaves(v, o, o.leaves.raw);
            sync_resting(v, m.cl_ord_id, o);
            sync_hold(v, *v.orders.find(m.cl_ord_id), Timestamp{});
          }
        }
        if (r->in_snapshot) push_order(*r, m.hdr);
        if (v.killed) cancel_once(v, m.cl_ord_id, m.hdr.instrument, m.venue_order_id);
        return;
      }
      // Nobody holds its epoch: a dead session's order, or one placed outside FastMM.
      queue_cancel(v, m.cl_ord_id, m.hdr.instrument, m.venue_order_id);
      return;
    }
    case ReconcileMsg::Kind::Position:
      if (v.insts->contains(m.hdr.instrument))
        set_account_position(v, m.hdr.instrument, m.position_qty, m.avg_px);
      // A shared instrument's venue position is the account's, no strategy's: it is compared with
      // theirs (Account::unexplained) instead.
      if (v.shared(m.hdr.instrument)) return;
      if (Route* o = v.owner_of(m.hdr.instrument)) {
        push_order(*o, m.hdr);
      } else {
        v.unrouted.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    case ReconcileMsg::Kind::End: {
      for (Route* r : v.routes) {
        if (!r->in_snapshot) continue;
        r->in_snapshot = false;
        push_order(*r, m.hdr);
      }
      // Orders the snapshot no longer holds and that had reached the venue when it was asked.
      if (!v.snap_bounded || v.snap_known) {
        std::vector<ClientOrderId> gone;
        v.orders.for_each_key([&](ClientOrderId id) {
          const GwOrder* o = v.orders.find(id);
          if (o->gen != v.gen && (!v.snap_bounded || o->g <= v.snap_p)) gone.push_back(id);
        });
        for (const ClientOrderId id : gone) untrack(v, id);
      }
      return;
    }
  }
}

// A shared instrument's execution goes to the attachment whose order it names; one naming an order
// of an earlier session to the live attachment of that session's strategy (by slot: the gateway's
// own epochs of this run and those the strategy's store listed); one naming no order to the
// instrument's primary. The account counts it towards that strategy, attached or not, or towards
// nobody: an order of a session no strategy claims, or no order and no primary. A replayed one that
// the receiving strategy's history holds is not routed, as for an owner. The attachment it goes
// to, or nullptr; `held`: the strategy's store holds it (it is not kept for a later attach).
Route* route_shared_fill(VenueRouter& v, const OrderFillMsg& m, bool replayed, bool& held) {
  const InstrumentId id = m.hdr.instrument;
  const Account& a = *v.acct;
  Route* r = v.by_epoch(m.cl_ord_id);
  std::uint8_t tag = 0;
  if (r != nullptr) {
    tag = static_cast<std::uint8_t>(r->slot + 1U);
  } else {
    const std::uint16_t e = cl_ord_id_epoch(m.cl_ord_id);
    tag = e != 0 ? a.epoch_slot[e].load(std::memory_order_relaxed) : a.primary[id.value];
    r = tag != 0 ? v.by_slot[tag - 1U] : nullptr;
    if (r != nullptr && replayed && m.hdr.exch_ts.ns > 0 && held_by(*r, m)) {
      v.stale_replays.fetch_add(1, std::memory_order_relaxed);
      r = nullptr;
      held = true;
    }
  }
  account_fill_shared(v, m, tag, r != nullptr);
  return r;
}

// The key an execution given from VenueRouter::parked is remembered by (Route::unparked): as the
// account's dedupe, its trade id, instrument and side.
std::string fill_key(const OrderFillMsg& m) {
  std::string k(m.exec_id.view());
  k += '/';
  k += std::to_string(m.hdr.instrument.value);
  k += m.side == Side::Buy ? 'B' : 'S';
  return k;
}

// An execution naming no client order id, but the venue's id of an order a strategy's store listed
// (VenueRouter::past_orders), names that order: `buf` holds the copy that says so.
const OrderFillMsg& named(const VenueRouter& v, const OrderFillMsg& m, OrderFillMsg& buf) {
  if (m.cl_ord_id.valid() || m.venue_order_id.empty() || v.past_orders.empty()) return m;
  const auto it =
      v.past_orders.find({m.hdr.instrument.value, std::string(m.venue_order_id.view())});
  if (it == v.past_orders.end()) return m;
  buf = m;
  buf.cl_ord_id = it->second;
  return buf;
}

// An execution to its strategy and into the account (see route_fill). One no attached strategy
// receives is kept in VenueRouter::parked for the attach that claims it, unless the store of the
// strategy it belongs to holds it. `unparking`: it comes from there, at an attach.
void deliver_fill(VenueRouter& v, const OrderFillMsg& raw, bool unparking) {
  OrderFillMsg buf;
  const OrderFillMsg& m = named(v, raw, buf);
  const InstrumentId id = m.hdr.instrument;
  const bool replayed = (m.flags & OrderFillMsg::kReplayed) != 0;
  const bool unresolved = replayed && !m.cl_ord_id.valid() && !m.exec_id.empty() &&
                          (m.flags & OrderFillMsg::kUnresolved) != 0;
  const auto same = [&](const OrderFillMsg& p) {
    return p.hdr.instrument == id && p.side == m.side && p.exec_id.view() == m.exec_id.view();
  };
  // The copy that settles an execution sent naming no order before: one naming its order (the
  // connector has looked it up) replaces every parked copy naming none; a last one without
  // kUnresolved (the connector gave up) replaces the copy held back for it.
  if (replayed && !unresolved && !unparking && !m.exec_id.empty() && !v.parked.empty()) {
    std::erase_if(v.parked, [&](const OrderFillMsg& p) {
      return !p.cl_ord_id.valid() &&
             (m.cl_ord_id.valid() || (p.flags & OrderFillMsg::kUnresolved) != 0) && same(p);
    });
  }
  // One the connector will send again (kUnresolved): no strategy receives it, not the primary nor
  // the owner, until the copy that settles it; the account books it now, for nobody (a later copy
  // naming its order moves it to that strategy's share).
  if (unresolved) {
    if (!unparking) {
      if (v.shared(id)) {
        account_fill_shared(v, m, 0, false);
      } else if (v.insts->contains(id) && id.value < kMaxInstruments && v.seeded[id.value]) {
        account_fill(v, m);
      }
      if (std::any_of(v.parked.begin(), v.parked.end(), [&](const OrderFillMsg& p) {
            return (p.flags & OrderFillMsg::kUnresolved) != 0 && same(p);
          }))
        return;  // a replay read it again
    }
    if (v.parked.size() == kMaxParked) {
      v.parked.pop_front();
      v.parked_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    v.parked.push_back(raw);
    return;
  }
  Route* r = nullptr;
  bool held = false;
  if (v.shared(id)) {
    r = route_shared_fill(v, m, replayed, held);
  } else if (v.insts->contains(id) && id.value < kMaxInstruments && !v.seeded[id.value]) {
    // Nobody has seeded the account's position yet: it is booked, and routed, once the first
    // owner's store has (unparked at that attach).
  } else {
    account_fill(v, m);
    // Streamed or replayed alike: to the order's epoch, else to the instrument's owner. A replay
    // one attachment's attach started also names the others' executions; each books only those
    // its OMS has not (it dedupes by venue execution id, instrument and side), and among them is
    // a fill its private stream missed.
    r = v.by_epoch(m.cl_ord_id);
    if (r == nullptr) {
      r = v.owner_of(id);
      // Naming no live order, it goes to the owner only if the owner's store cannot hold it
      // already: a replay another attach started can reach back before this owner's history
      // began, and its engine never saw those executions, so it would book them a second time.
      // (A connector that gives no trade time, exch_ts 0, is not filtered.)
      if (r != nullptr && replayed && m.hdr.exch_ts.ns > 0 && held_by(*r, m)) {
        v.stale_replays.fetch_add(1, std::memory_order_relaxed);
        FASTMM_LOG_INFO(
            "gateway: replayed execution {} ({} ms) is older than its owner's history ({} ms) or "
            "in its store: not routed",
            m.exec_id.view(),
            m.hdr.exch_ts.ns / 1'000'000,
            r->replay_from_ms);
        return;
      }
    }
  }
  if (r != nullptr) {
    if (unparking) {
      if (!m.exec_id.empty()) r->unparked.insert(fill_key(m));
    } else if (replayed && !r->unparked.empty() && !m.exec_id.empty() &&
               r->unparked.contains(fill_key(m))) {
      return;  // given to it from `parked` at its attach
    }
    push_order(*r, m.hdr);
    return;
  }
  if (!unparking) v.unrouted.fetch_add(1, std::memory_order_relaxed);
  if (held || !v.insts->contains(id)) return;
  if (v.parked.size() == kMaxParked) {
    v.parked.pop_front();
    v.parked_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  v.parked.push_back(raw);
}

// At an attach, once its routes, seeds and past orders are in place: every parked execution is
// routed again, and what nobody receives stays.
void unpark(VenueRouter& v) {
  std::deque<OrderFillMsg> was;
  was.swap(v.parked);
  for (const OrderFillMsg& m : was) deliver_fill(v, m, true);
  v.parked_count.store(v.parked.size(), std::memory_order_relaxed);
}

void route_fill(VenueRouter& v, const OrderFillMsg& m) {
  const bool replayed = (m.flags & OrderFillMsg::kReplayed) != 0;
  if (!replayed) {
    if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
      acknowledge(v, *o, m.hdr.exch_ts);
      if (m.leaves_qty.is_zero()) {
        untrack(v, m.cl_ord_id, m.hdr.exch_ts);
      } else if (v.insts->contains(o->inst)) {
        const std::int64_t n = v.insts->get(o->inst).notional(o->price, m.leaves_qty).raw;
        v.open_notional += n - o->notional;
        o->notional = n;
        set_leaves(v, *o, m.leaves_qty, m.hdr.exch_ts);
      }
    }
  }
  deliver_fill(v, m, false);
  v.parked_count.store(v.parked.size(), std::memory_order_relaxed);
}

// A funding payment goes to the instrument's owner, which books it (its engine dedupes by id). One
// from the venue's history that is older than the owner's history or in its store is not routed,
// as with a replayed execution naming no live order.
void route_funding(VenueRouter& v, const FundingMsg& m) {
  account_funding(v, m);
  // A shared instrument's funding is its primary's, or the account's alone.
  Route* r = v.owner_of(m.hdr.instrument);
  if (v.shared(m.hdr.instrument)) {
    const std::uint8_t p = v.acct->primary[m.hdr.instrument.value];
    r = p != 0 ? v.by_slot[p - 1U] : nullptr;
  }
  if (r == nullptr) {
    v.unrouted.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if ((m.flags & FundingMsg::kReplayed) != 0 && m.hdr.exch_ts.ns > 0 &&
      (m.hdr.exch_ts.ns / 1'000'000 < r->replay_from_ms ||
       (r->known != nullptr && r->known->contains(known_funding_id(m))))) {
    v.stale_replays.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  push_order(*r, m.hdr);
}

// An event about one order goes to the attachment its epoch names; a dead session's is dropped.
template <class M>
void route_by_id(VenueRouter& v, const M& m) {
  if (Route* r = v.by_epoch(m.cl_ord_id)) {
    push_order(*r, m.hdr);
  } else {
    v.unrouted.fetch_add(1, std::memory_order_relaxed);
  }
}

void route_order(VenueRouter& v, const EventHeader& h) {
  switch (h.type) {
    case EventType::OrderAck: {
      const auto& m = msg_cast<OrderAckMsg>(&h);
      bool cancel_sent = false;
      if (GwOrder* o = v.orders.find(m.cl_ord_id)) {
        o->venue_order_id = m.venue_order_id;
        acknowledge(v, *o, m.hdr.exch_ts);
        cancel_sent = (o->flags & GwOrder::kCancelSent) != 0;
        if (o->replaces.valid()) {
          untrack(v, o->replaces, m.hdr.exch_ts);
          if (GwOrder* again = v.orders.find(m.cl_ord_id)) again->replaces = ClientOrderId{};
        }
      }
      Route* r = v.by_epoch(m.cl_ord_id);
      if (r != nullptr) push_order(*r, h);
      // Sent by a strategy that has gone since, it rests unmanaged; after an account trip nothing
      // may rest. Either way it goes.
      if ((r == nullptr || v.killed) && !cancel_sent)
        queue_cancel(v, m.cl_ord_id, h.instrument, m.venue_order_id);
      return;
    }
    case EventType::OrderReject: {
      const auto& m = msg_cast<OrderRejectMsg>(&h);
      if (const GwOrder* o = v.orders.find(m.cl_ord_id)) {
        if (o->replaces.valid()) {
          if (GwOrder* orig = v.orders.find(o->replaces)) {
            orig->notional = o->replaced_notional;
            v.open_notional += o->replaced_notional;
            set_leaves(v, *orig, o->replaced_leaves);
            sync_resting(v, o->replaces, *orig);
          }
        }
      }
      untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderCancelAck: {
      const auto& m = msg_cast<OrderCancelAckMsg>(&h);
      untrack(v, m.cl_ord_id, m.hdr.exch_ts);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderCancelReject: {
      const auto& m = msg_cast<OrderCancelRejectMsg>(&h);
      if (m.reason == RejectReason::VenueUnknownOrder) untrack(v, m.cl_ord_id);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderExpired: {
      const auto& m = msg_cast<OrderExpiredMsg>(&h);
      untrack(v, m.cl_ord_id, m.hdr.exch_ts);
      route_by_id(v, m);
      return;
    }
    case EventType::OrderFill:
      route_fill(v, msg_cast<OrderFillMsg>(&h));
      return;
    case EventType::Funding:
      route_funding(v, msg_cast<FundingMsg>(&h));
      return;
    case EventType::PositionUpdate: {
      const auto& m = msg_cast<PositionUpdateMsg>(&h);
      if (v.insts->contains(h.instrument)) set_account_position(v, h.instrument, m.qty, m.avg_px);
      if (v.shared(h.instrument)) return;  // the account's, as a reconciliation's Position row
      if (Route* o = v.owner_of(h.instrument)) {
        push_order(*o, h);
      } else {
        v.unrouted.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    case EventType::Reconcile:
      route_reconcile(v, msg_cast<ReconcileMsg>(&h));
      return;
    case EventType::Balance:
      // The account's, and every attachment's: each engine keeps the venue's balances too.
      v.balances->on_report(msg_cast<BalanceMsg>(&h));
      for (Route* r : v.routes) push_order(*r, h);
      return;
    default:
      // Connection states, a venue kill, latency samples: every attachment.
      for (Route* r : v.routes) push_order(*r, h);
      return;
  }
}

// EventSink drain hook of the order sink.
void drain_orders(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  MsgRing& ring = *v.slot->order_ring;
  while (const std::byte* p = ring.try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    // With nothing attached the gateway still keeps its order table and cancels dead sessions'.
    if (v.routes.empty()) v.order_discarded.fetch_add(1, std::memory_order_relaxed);
    route_order(v, h);
    ring.release();
  }
}

// ---- network thread: outbound -----------------------------------------------------------------

void refuse(VenueRouter& v, Route& r, const EventHeader& h, ClientOrderId id, RejectReason why) {
  OrderRejectMsg m{};
  init_header(m, EventType::OrderReject, h.instrument, h.venue);
  m.cl_ord_id = id;
  m.reason = why;
  m.text.assign("fastmm-gateway");
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  push_order(r, m.hdr);
  std::size_t slot = 0;  // GatewayNotOwner, which also counts any other reason
  for (std::size_t i = 0; i < kStatusGatewayRefusals; ++i) {
    if (kStatusGatewayRefusalReasons[i] == why) slot = i;
  }
  r.refused[slot].fetch_add(1, std::memory_order_relaxed);
  switch (why) {
    case RejectReason::GatewayRateLimit:
      v.refused_rate.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayOpenNotional:
      v.refused_notional.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayAccountKilled:
      v.refused_killed.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayGrossNotional:
      v.refused_gross.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayNetNotional:
      v.refused_net.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayFxRateUnknown:
      v.refused_fx.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayUnderlyingNet:
      v.refused_underlying.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayUnderlyingMarkUnknown:
      v.refused_underlying_mark.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewaySelfTrade:
      v.refused_self_trade.fetch_add(1, std::memory_order_relaxed);
      break;
    case RejectReason::GatewayBalanceShort:
      v.refused_balance.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      v.refused_owner.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

// The account guards: the sender trades the instrument, the account's kill switch, on a shared
// instrument no trade with another attachment's resting order, the notional working at the venue,
// a current rate for the order's currency ([accounting], when it adds to exposure; an unknown side
// counts as adding), the account's exposure (when the side is known), the account's balance on the
// venue ([gateway] check_balance, once the venue reports; when the side is known), the venue's
// order rate. `replaced` is the working notional of the order a replace takes over,
// `replaced_leaves` its working quantity and `replaced_hold` what it holds of the balance.
RejectReason check_order(VenueRouter& v,
                         const Route& r,
                         InstrumentId inst,
                         const Side* side,
                         Price px,
                         Qty qty,
                         bool market,
                         bool reduce_only,
                         std::int64_t replaced,
                         Qty replaced_leaves,
                         std::int64_t replaced_hold,
                         std::int64_t* notional) {
  if (inst.value >= kMaxInstruments || !r.claims[inst.value] || !v.insts->contains(inst))
    return RejectReason::GatewayNotOwner;
  if (FASTMM_UNLIKELY(v.acct->tripped.load(std::memory_order_relaxed)))
    return RejectReason::GatewayAccountKilled;
  if (v.acct->shared[inst.value]) {
    // A replace of an order the gateway does not track has no known side: neither may cross.
    for (const Side s : {Side::Buy, Side::Sell}) {
      if (side != nullptr && s != *side) continue;
      if (v.resting[inst.value].crosses(r.epoch, s, px, market))
        return RejectReason::GatewaySelfTrade;
    }
  }
  const Notional n = v.insts->get(inst).notional(px, qty);
  *notional = n.raw;
  if (v.max_open_notional > 0 && v.open_notional - replaced + *notional > v.max_open_notional)
    return RejectReason::GatewayOpenNotional;
  const Account& a = *v.acct;
  Notional exposure = n;
  if (FASTMM_UNLIKELY(a.fx.active()) && (a.max_loss > 0 || a.exposure_limits())) {
    const std::uint8_t c = a.fx.ccy[inst.value];
    const std::int64_t q = v.book->positions().get(inst).qty.raw;
    const bool adds = side == nullptr || q == 0 || (q > 0) == (*side == Side::Buy);
    if (c != 0 && adds) {
      const FxRate rate = a.current_rate(c, steady_now().ns);
      if (!rate.known()) return RejectReason::GatewayFxRateUnknown;
      exposure = convert(n, rate);
    }
  }
  if (a.exposure_limits() && side != nullptr) {
    if (const RejectReason why = check_account(v, inst, *side, exposure); why != RejectReason::None)
      return why;
  }
  if (FASTMM_UNLIKELY(a.und.active())) {
    // A replace of an order the gateway does not track has no known side: it must pass both ways.
    for (const Side s : {Side::Buy, Side::Sell}) {
      if (side != nullptr && s != *side) continue;
      if (const RejectReason why = check_underlying(v, inst, s, qty, replaced_leaves);
          why != RejectReason::None)
        return why;
    }
  }
  if (side != nullptr && v.balances->live() && v.balances->check_enabled()) {
    Price hold_px = px;
    if (market) {
      const AccountBook::Book* b = v.book->book(inst);
      hold_px = b != nullptr && b->is_valid() ? b->mid() : Price{};
    }
    const std::int64_t q = v.book->positions().get(inst).qty.raw;
    const bool reduces = q != 0 && (q > 0) != (*side == Side::Buy) && qty.raw <= (q < 0 ? -q : q);
    if (!v.balances->covers(
            inst, v.insts->get(inst), *side, hold_px, qty, replaced_hold, reduce_only, reduces))
      return RejectReason::GatewayBalanceShort;
  }
  if (!v.rate.try_take(steady_now())) return RejectReason::GatewayRateLimit;
  return RejectReason::None;
}

void note_forwarded(VenueRouter& v, ClientOrderId id) noexcept {
  v.hist[v.forwarded % kHistory] = id;
  ++v.forwarded;
}

void track(VenueRouter& v, ClientOrderId id, const GwOrder& o) noexcept {
  if (v.orders.insert(id, o).second) {
    v.open_notional += o.notional;
    add_leaves(v, o, o.leaves.raw);
    sync_resting(v, id, o);
    sync_hold(v, *v.orders.find(id), Timestamp{});
  } else {
    v.untracked.fetch_add(1, std::memory_order_relaxed);
  }
}

// Moves one attachment's outbound messages into the venue's ring. Returns how many.
std::size_t forward(VenueRouter& v, Route& r) {
  MsgRing& dst = *v.slot->outbound;
  std::size_t n = 0;
  while (const std::byte* p = r.out->try_peek()) {
    const auto& h = *reinterpret_cast<const EventHeader*>(p);
    std::byte* slot = dst.try_reserve(h.len);
    if (slot == nullptr) break;  // the venue catches up first
    bool send = true;
    switch (h.type) {
      case EventType::OutNewOrder: {
        const auto& m = msg_cast<OutNewOrderMsg>(&h);
        std::int64_t notional = 0;
        const bool market = m.type == OrderType::Market;
        if (const RejectReason why = check_order(v,
                                                 r,
                                                 h.instrument,
                                                 &m.side,
                                                 m.price,
                                                 m.qty,
                                                 market,
                                                 m.reduce_only != 0,
                                                 0,
                                                 Qty{},
                                                 0,
                                                 &notional);
            why != RejectReason::None) {
          refuse(v, r, h, m.cl_ord_id, why);
          send = false;
          break;
        }
        GwOrder o;
        o.inst = h.instrument;
        o.epoch = r.epoch;
        o.side = m.side;
        o.price = m.price;
        o.notional = notional;
        o.leaves = m.qty;
        if (!market && (m.tif == TimeInForce::Gtc || m.tif == TimeInForce::Day))
          o.flags = GwOrder::kRests;
        if (m.reduce_only != 0) o.flags |= GwOrder::kReduceOnly;
        note_forwarded(v, m.cl_ord_id);
        o.g = v.forwarded;
        track(v, m.cl_ord_id, o);
        break;
      }
      case EventType::OutReplace: {
        const auto& m = msg_cast<OutReplaceMsg>(&h);
        GwOrder* orig = v.orders.find(m.orig_cl_ord_id);
        const std::int64_t replaced = orig != nullptr ? orig->notional : 0;
        const Qty replaced_leaves = orig != nullptr ? orig->leaves : Qty{};
        const std::int64_t replaced_hold = orig != nullptr ? orig->hold : 0;
        // A replace keeps its order's side; an order the gateway does not track has none here.
        const Side side = orig != nullptr ? orig->side : Side::Buy;
        std::int64_t notional = 0;
        if (const RejectReason why =
                check_order(v,
                            r,
                            h.instrument,
                            orig != nullptr ? &side : nullptr,
                            m.price,
                            m.qty,
                            /*market=*/false,
                            orig != nullptr && (orig->flags & GwOrder::kReduceOnly) != 0,
                            replaced,
                            replaced_leaves,
                            replaced_hold,
                            &notional);
            why != RejectReason::None) {
          refuse(v, r, h, m.cl_ord_id, why);
          send = false;
          break;
        }
        const std::uint8_t rests =
            orig != nullptr
                ? static_cast<std::uint8_t>(orig->flags & (GwOrder::kRests | GwOrder::kReduceOnly))
                : GwOrder::kRests;
        if (orig != nullptr) {
          v.open_notional -= orig->notional;
          orig->notional = 0;
          set_leaves(v, *orig, Qty{});
          drop_resting(v, m.orig_cl_ord_id, *orig);
        }
        GwOrder o;
        o.flags = rests;
        o.inst = h.instrument;
        o.epoch = r.epoch;
        o.side = side;
        o.price = m.price;
        o.notional = notional;
        o.replaces = m.orig_cl_ord_id;
        o.replaced_notional = replaced;
        o.leaves = m.qty;
        o.replaced_leaves = replaced_leaves;
        o.venue_order_id = m.venue_order_id;
        note_forwarded(v, m.cl_ord_id);
        o.g = v.forwarded;
        track(v, m.cl_ord_id, o);
        break;
      }
      case EventType::Control:
        if (msg_cast<ControlMsg>(&h).command == ControlCommand::Reconcile)
          r.snapshot_pending = true;
        break;
      default:
        break;  // cancels always go
    }
    if (send) {
      std::memcpy(slot, p, h.len);
      dst.commit();
    }
    r.out->release();
    ++n;
  }
  return n;
}

// The gateway's own cancels (a detached strategy's orders, a dead session's rows and acks).
std::size_t send_cancels(VenueRouter& v) {
  if (v.cancels.empty()) return 0;
  std::vector<CancelReq> todo;
  todo.swap(v.cancels);
  std::size_t sent = 0;
  for (const CancelReq& c : todo) {
    if (sent == todo.size()) break;
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, c.inst, v.vid);
    m.cl_ord_id = c.id;
    m.venue_order_id = c.venue_order_id;
    if (!v.slot->outbound->try_push(&m, m.hdr.len)) {
      v.cancels.push_back(c);  // next round
      continue;
    }
    ++sent;
  }
  v.gateway_cancels.fetch_add(sent, std::memory_order_relaxed);
  return sent;
}

// The gateway's copy of each book of this venue into one attachment's md ring, as the connectors'
// BookSnapshot. Everything runs on the venue's network thread, and the md drain routes each event
// to every attachment and to acct_md in one step: once acct_md is applied, the copies are the
// books after exactly the events routed so far, and the drain routes the ones after them. A book
// the gateway does not hold now (the venue is resyncing it, or its channel is down) gets none:
// the venue's next snapshot reaches every attachment. False when the ring had no room.
bool push_books(VenueRouter& v, Route& r) noexcept {
  mark_account(v);
  const Timestamp now = wall_now();
  for (const Instrument& inst : *v.insts) {
    if (inst.venue != v.vid) continue;
    const AccountBook::Book* b = v.book->book(inst.id);
    if (b == nullptr || !b->has_snapshot()) continue;
    std::byte* p = r.md->try_reserve(book_snapshot_size(*b));
    if (p == nullptr) return false;
    write_book_snapshot(*b, inst.id, v.vid, now, p);
    r.md->commit();
    r.dirty = true;
  }
  // The venue's marks, indices and funding that are fresh: a strategy that attaches between two
  // funding-rate pushes (OKX: a minute apart) has them at once.
  const Timestamp steady = steady_now();
  for (const Instrument& inst : *v.insts) {
    if (inst.venue != v.vid) continue;
    PerpStateMsg m{};
    if (!v.book->perp_snapshot(inst.id, v.vid, steady, m)) continue;
    m.hdr.recv_ts = now;
    if (!r.md->try_push(&m, m.hdr.len)) return false;
    r.dirty = true;
  }
  return true;
}

// An attachment whose md ring dropped gets a Resyncing state of its own once its ring has room
// (its engine clears the venue's books and pulls their quotes), then the gateway's books. The
// venue and the other attachments see nothing of it.
std::size_t recover_md(VenueRouter& v) {
  std::size_t n = 0;
  for (Route* r : v.routes) {
    if (!r->md_gap || r->md->bytes_used_approx() > r->md->capacity() / 2) continue;
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, InstrumentId::invalid(), v.vid);
    m.state = ConnState::Resyncing;
    m.channel = 0;
    m.hdr.recv_ts = wall_now();
    if (!r->md->try_push(&m, m.hdr.len)) continue;
    r->dirty = true;
    r->md_gap = !push_books(v, *r);  // else again from a Resyncing
    ++n;
  }
  if (v.want_resync) {
    const std::int64_t now = steady_now().ns;
    if (now - v.last_resync_ns >= kResyncIntervalNs) {
      v.want_resync = false;
      v.last_resync_ns = now;
      // The states it emits are not routed: the attachments' books are whole.
      v.quiet_md_state = true;
      v.slot->venue->resync_books();
      v.quiet_md_state = false;
      ++n;
    }
  }
  return n;
}

// The account's kill switch tripped: every attachment's engine trips this venue's kill (and so
// stops quoting and cancels its orders), and the gateway cancels every order it knows at the venue
// and, once the venue has listed what it holds, every other one.
void kill_venue(VenueRouter& v) {
  v.killed = true;
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId::invalid(), v.vid);
  m.command = ControlCommand::TripVenueKill;
  m.arg = static_cast<std::uint64_t>(v.acct->trip_reason.load(std::memory_order_acquire));
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  for (Route* r : v.routes) push_order(*r, m.hdr);
  if (v.dry_run) return;
  std::vector<ClientOrderId> all;
  v.orders.for_each_key([&](ClientOrderId id) { all.push_back(id); });
  for (const ClientOrderId id : all) {
    const GwOrder o = *v.orders.find(id);
    cancel_once(v, id, o.inst, o.venue_order_id);
  }
  v.sweep_pending = true;
  v.slot->venue->request_open_orders();
}

// VenueSlot::Hook, after every reactor iteration: the account's trip, the gateway's cancels, the
// attachments' orders through the account guards into the venue, md recovery, then the engines'
// wake-ups.
// The venue's rows of the account's balance table, for the status file (every 200 ms at most).
void publish_balances(VenueRouter& v) noexcept {
  if (!v.balances->live()) return;
  const std::int64_t now = steady_now().ns;
  if (now - v.balance_pub_ns < 200'000'000) return;
  v.balance_pub_ns = now;
  VenueRouter::BalancePub p;
  for (std::size_t i = 0; i < v.balances->size() && p.count < kMaxLiveBalances; ++i) {
    const BalanceBook::Row& r = v.balances->row(i);
    if (r.venue != v.vid) continue;
    LiveBalance& b = p.rows[p.count++];
    b.venue = r.venue.value;
    b.account = r.account ? 1 : 0;
    b.known = r.reported ? 1 : 0;
    std::memcpy(b.asset, r.asset.data(), r.asset.size());
    b.free_raw = r.free;
    b.locked_raw = r.locked;
    b.total_raw = r.total;
    b.equity_raw = r.equity;
    b.maintenance_raw = r.maintenance;
    b.as_of_ns = r.as_of.ns;
  }
  v.balance_pub.store(p);
}

// The venue's rows of the account's perp table, for the status file (every 200 ms at most).
void publish_perps(VenueRouter& v) noexcept {
  const PerpBook& pb = v.book->perps();
  if (pb.instruments() == 0) return;
  const Timestamp now = steady_now();
  if (now.ns - v.perp_pub_ns < 200'000'000) return;
  v.perp_pub_ns = now.ns;
  VenueRouter::PerpPub p;
  for (const Instrument& inst : *v.insts) {
    if (inst.venue != v.vid || pb.row(inst.id).reports == 0) continue;
    if (p.count == kMaxLivePerps) break;
    fill_live_perp(p.rows[p.count++], pb, inst.id, v.vid, now);
  }
  v.perp_pub.store(p);
}

std::size_t gateway_hook(void* ctx) noexcept {
  auto& v = *static_cast<VenueRouter*>(ctx);
  if (FASTMM_UNLIKELY(v.acct->tripped.load(std::memory_order_relaxed)) && !v.killed) kill_venue(v);
  std::size_t moved = send_cancels(v);
  for (Route* r : v.routes) moved += forward(v, *r);
  if (moved != 0) v.slot->venue->on_wake();
  std::size_t n = moved;
  n += recover_md(v);
  for (Route* r : v.routes) {
    if (!r->dirty) continue;
    r->dirty = false;
    if (r->waker != nullptr) r->waker->notify();
    ++n;
  }
  n += mark_account(v);
  publish_balances(v);
  publish_perps(v);
  return n;
}

// VenueSlot::Pending: the recheck of an adaptive network thread about to block. An engine takes
// the thread's flag in the gateway's wake page after it pushed an order
// (GatewayClient::wake_venue).
bool gateway_pending(void* ctx) noexcept {
  const auto& v = *static_cast<const VenueRouter*>(ctx);
  if (!v.cancels.empty() || !v.acct_md->empty_approx()) return true;
  for (const Route* r : v.routes) {
    if (!r->out->empty_approx()) return true;
  }
  return false;
}

// ---- main thread ------------------------------------------------------------------------------

// Runs fn(i) on every venue's network thread and waits until all have run. The tasks reference
// the caller's frame, so this waits for as long as it takes (a wedged thread is logged).
void on_net_threads(VenueSlots& slots, const std::function<void(std::size_t)>& fn) {
  std::atomic<std::size_t> done{0};
  for (std::size_t i = 0; i < slots.size(); ++i) {
    slots[i]->reactor->post([&fn, &done, i] {
      fn(i);
      done.fetch_add(1, std::memory_order_release);
    });
    slots[i]->reactor->wake();
  }
  std::int64_t next_warn = steady_now().ns + 5'000'000'000;
  while (done.load(std::memory_order_acquire) != slots.size()) {
    sleep_for(microseconds(50));
    if (steady_now().ns >= next_warn) {
      next_warn += 5'000'000'000;
      FASTMM_LOG_ERROR("gateway: a network thread has not run a posted task for 5 s");
    }
  }
}

// "1.5", "-0.001": a fixed-point value for a formatted line.
template <class Tag>
std::string dec(Fixed<Tag> v) {
  char buf[kMaxDecimalChars];
  return std::string(buf, v.to_decimal(buf));
}

template <std::size_t N>
void to_field(char (&f)[N], std::string_view s) {
  std::memset(f, 0, N);
  std::memcpy(f, s.data(), std::min(s.size(), N - 1));
}

template <std::size_t N>
std::string from_field(const char (&f)[N]) {
  return std::string(f, ::strnlen(f, N));
}

void send_error(int fd, std::string_view why) {
  gw::AttachReply r{};
  r.hdr =
      gw::Header{gw::kMagic, gw::kVersion, gw::MsgType::AttachReply, sizeof(gw::AttachReply), 0};
  r.status = 1;
  r.gateway_pid = static_cast<std::uint32_t>(::getpid());
  to_field(r.error, why);
  static_cast<void>(::send(fd, &r, sizeof r, MSG_NOSIGNAL));
}

// One attached strategy.
struct Attachment {
  int fd = -1;
  std::uint32_t id = 0;
  std::string engine;
  std::uint32_t pid = 0;
  std::int64_t since_ns = 0;
  std::int64_t since_wall_ns = 0;
  bool blocks = false;  // its engine blocks when idle
  std::uint16_t epoch = 0;
  std::uint8_t slot = 0;  // its [engine] name's (Account::share)
  std::vector<InstrumentId> owned;
  struct Rings {
    std::unique_ptr<ShmRing> md;
    std::unique_ptr<ShmRing> order;
    std::unique_ptr<ShmRing> outbound;
    std::array<std::string, 3> paths;
  };
  std::vector<Rings> rings;         // per venue
  std::unique_ptr<Route[]> routes;  // per venue
  std::vector<std::uint64_t> md_dropped_logged;
  // Per venue, the trade ids its store listed (Route::known; an account position it seeds keeps
  // them).
  std::vector<std::shared_ptr<const std::unordered_set<std::string>>> known;
  // The wake page (live/gateway.hpp) and this process's Waker over the engine's flag in it.
  gw::WakePage* page = nullptr;
  std::unique_ptr<Waker> engine_waker;

  [[nodiscard]] std::string who() const {
    return engine + " (pid " + std::to_string(pid) + ", attachment " + std::to_string(id) +
           ", epoch " + std::to_string(epoch) + ")";
  }
};

void remove_rings(const std::vector<Attachment::Rings>& rings) {
  std::error_code ec;
  for (const Attachment::Rings& r : rings)
    for (const std::string& p : r.paths)
      if (!p.empty()) std::filesystem::remove(p, ec);
}

class Gateway {
 public:
  // A position a strategy restored from its store (gw::PositionSeed).
  struct Seed {
    InstrumentId inst;
    Qty qty;
    Price avg_px;
  };
  // Where one venue's execution replay starts for a strategy (gw::VenueResume).
  struct Resume {
    bool set = false;
    std::int64_t since_ms = 0;  // venue time
    std::vector<std::string> known;
    std::vector<PastOrder> orders;  // of its earlier sessions (gw::PastOrder)
  };

  Gateway(const Config& cfg,
          const GatewayOptions& opts,
          VenueSlots& slots,
          InstrumentTable& insts,
          gw::WakePage* net_page,
          int net_page_fd,
          Account& acct,
          std::string kill_path,
          const KillState& kill,
          const std::vector<std::pair<InstrumentId, std::string>>& primaries)
      : cfg_(cfg),
        opts_(opts),
        slots_(slots),
        instruments_(insts),
        net_page_fd_(net_page_fd),
        acct_(acct),
        kill_path_(std::move(kill_path)),
        kill_(kill) {
    const std::int64_t start_ms = wall_now().ns / 1'000'000;
    std::vector<std::string> venue_names;
    for (const auto& s : slots) venue_names.emplace_back(s->venue->name());
    const FeeTable fees = fee_table(cfg, &insts, &venue_names);
    BalanceConfig balance = balance_config(cfg, &insts, &venue_names);
    balance.check = cfg.gateway.check_balance;
    std::int64_t max_notional = 0;
    if (!cfg.gateway.max_open_notional.empty()) {
      if (const auto n = Notional::from_decimal(cfg.gateway.max_open_notional))
        max_notional = n->raw;
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
      auto v = std::make_unique<VenueRouter>();
      v->slot = slots[i].get();
      v->vid = VenueId{static_cast<std::uint8_t>(i)};
      v->insts = &insts;
      v->rate.configure(static_cast<std::uint32_t>(cfg.gateway.orders_per_sec),
                        static_cast<std::uint32_t>(cfg.gateway.burst),
                        steady_now());
      v->max_open_notional = max_notional;
      v->dry_run = opts.dry_run;
      v->acct = &acct;
      v->book = std::make_unique<AccountBook>(insts, v->vid, acct.fx, perp_config(cfg));
      v->balances->build(insts, fees, balance);
      v->acct_md = std::make_unique<MsgRing>(ring_size(cfg.engine.md_ring_bytes));
      // Before any strategy seeded an instrument, a replayed execution from before the gateway
      // started is none of the account's business: the positions start with the strategies'.
      v->seed_from_ms.fill(start_ms);
      v->start_ms = start_ms;
      for (const Instrument& inst : insts) {
        if (inst.venue != v->vid || !acct.shared[inst.id.value]) continue;
        v->resting[inst.id.value].reserve(64);
      }
      acct.slots[i] = slots[i].get();
      VenueSlot& s = *slots[i];
      s.hook = &gateway_hook;
      s.pending = &gateway_pending;
      s.hook_ctx = v.get();
      s.blocked = &net_page->net[i].flag;
      s.consumer = nullptr;  // the hook notifies each attachment's engine
      s.md_sink.set_drain_hook(&drain_md, v.get(), /*on_commit=*/true);
      s.order_sink.set_drain_hook(&drain_orders, v.get(), /*on_commit=*/true);
      routers_.push_back(std::move(v));
    }
    // A primary has its slot before it attaches: what is its is counted towards it meanwhile.
    for (const auto& [inst, name] : primaries) {
      if (name.empty()) continue;
      const std::optional<std::uint8_t> slot = slot_of(name);
      if (!slot) {
        FASTMM_LOG_ERROR(
            "gateway: no slot for primary {} of instrument {}: more than {} [engine] "
            "names; its unattributed events go to the account",
            name,
            inst.value,
            Account::kMaxSlots);
        continue;
      }
      acct.primary[inst.value] = static_cast<std::uint8_t>(*slot + 1U);
    }
  }

  [[nodiscard]] std::size_t attachments() const noexcept { return atts_.size(); }

  // The slot of an [engine] name (Account::share), given at its first attach (or as a primary) and
  // kept for the gateway's life; nullopt when every slot is taken.
  std::optional<std::uint8_t> slot_of(const std::string& engine) {
    for (std::size_t k = 0; k < slot_names_.size(); ++k) {
      if (slot_names_[k] == engine) return static_cast<std::uint8_t>(k);
    }
    if (slot_names_.size() == Account::kMaxSlots) return std::nullopt;
    slot_names_.push_back(engine);
    return static_cast<std::uint8_t>(slot_names_.size() - 1);
  }
  // Attachments trading `inst`.
  [[nodiscard]] std::size_t traders(InstrumentId inst) const {
    std::size_t n = 0;
    for (const auto& a : atts_)
      n += static_cast<std::size_t>(std::find(a->owned.begin(), a->owned.end(), inst) !=
                                    a->owned.end());
    return n;
  }

  // pollfds for the attachments' connections, in atts_ order.
  void poll_fds(std::vector<pollfd>& fds) const {
    for (const auto& a : atts_) fds.push_back(pollfd{a->fd, POLLIN | POLLRDHUP, 0});
  }
  // The connection of atts_[k] has something to say (it closed, as a rule).
  void on_readable(std::size_t k) {
    Attachment& a = *atts_[k];
    char b[64];
    const ssize_t n = ::recv(a.fd, b, sizeof b, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
      detach(a, n == 0 ? "its connection closed" : std::strerror(errno));
    } else if (n > 0) {
      FASTMM_LOG_WARN("gateway: ignored {} byte(s) from {}", n, std::string_view(a.who()));
    }
  }

  // A new connection on the listener. Answers it and keeps it as an attachment, or refuses it.
  void on_connection(int fd) {
    // The request follows the connect at once; a client that sends nothing is dropped.
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, 2000) <= 0) {
      FASTMM_LOG_WARN("gateway: a connection sent no attach request");
      ::close(fd);
      return;
    }
    std::vector<std::byte> buf(gw::kMaxRequest);
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    gw::AttachRequest req{};
    if (n < static_cast<ssize_t>(sizeof req)) {
      send_error(fd, "short or missing attach request");
      ::close(fd);
      return;
    }
    std::memcpy(&req, buf.data(), sizeof req);
    if (req.hdr.magic != gw::kMagic || req.hdr.type != gw::MsgType::AttachRequest) {
      send_error(fd, "not an attach request");
      ::close(fd);
      return;
    }
    if (req.hdr.version != gw::kVersion) {
      send_error(fd,
                 "gateway protocol version " + std::to_string(gw::kVersion) + ", request has " +
                     std::to_string(req.hdr.version));
      ::close(fd);
      return;
    }
    if (req.known_count > gw::kMaxKnownExecIds || req.claim_count > kMaxInstruments ||
        req.position_count > kMaxInstruments || req.resume_count > kMaxVenuesConfig ||
        req.epoch_count > gw::kMaxPastEpochs || req.order_count > gw::kMaxPastOrders ||
        static_cast<std::size_t>(n) != sizeof req + req.known_count * sizeof(gw::ExecId) +
                                           req.claim_count * sizeof(gw::InstrumentClaim) +
                                           req.position_count * sizeof(gw::PositionSeed) +
                                           req.resume_count * sizeof(gw::VenueResume) +
                                           req.epoch_count * sizeof(std::uint16_t) +
                                           req.order_count * sizeof(gw::PastOrder)) {
      send_error(fd, "malformed attach request");
      ::close(fd);
      return;
    }
    const std::string engine = from_field(req.engine);
    const auto refuse_attach = [&](const std::string& why) {
      FASTMM_LOG_WARN("gateway: refused {} (pid {}): {}",
                      std::string_view(engine),
                      req.pid,
                      std::string_view(why));
      send_error(fd, why);
      ::close(fd);
    };
    if (acct_.tripped.load(std::memory_order_acquire)) {
      refuse_attach(latched_message());
      return;
    }
    // Its kill file would be the gateway's.
    if (acct_.max_loss > 0 && engine == cfg_.engine.name) {
      refuse_attach("the strategy has the gateway's [engine] name '" + engine +
                    "', and with [gateway] max_loss the gateway keeps the account's loss in " +
                    kill_path_ + ": give the strategy a name of its own");
      return;
    }
    if (atts_.size() >= gw::kMaxAttachments) {
      refuse_attach("the gateway has " + std::to_string(gw::kMaxAttachments) +
                    " strategies attached already");
      return;
    }
    std::vector<std::string> known;
    known.reserve(req.known_count);
    for (std::uint32_t i = 0; i < req.known_count; ++i) {
      gw::ExecId id{};
      std::memcpy(&id, buf.data() + sizeof req + i * sizeof id, sizeof id);
      known.push_back(from_field(id.id));
    }
    // The instruments it trades: each in the gateway's table and owned by no one else.
    std::vector<InstrumentId> owned;
    const std::byte* claims = buf.data() + sizeof req + req.known_count * sizeof(gw::ExecId);
    for (std::uint32_t i = 0; i < req.claim_count; ++i) {
      gw::InstrumentClaim cl{};
      std::memcpy(&cl, claims + i * sizeof cl, sizeof cl);
      const std::string venue = from_field(cl.venue);
      const std::string symbol = from_field(cl.symbol);
      const VenueId vid = cfg_.venue_id(venue);
      const Instrument* inst = vid.valid() ? instruments_.find(vid, symbol) : nullptr;
      if (inst == nullptr) {
        refuse_attach(fmt::format(
            "instrument {} on venue '{}' is not in the gateway's configuration", symbol, venue));
        return;
      }
      if (const Attachment* other = owner_[inst->id.value]) {
        refuse_attach(
            fmt::format("instrument {} on venue '{}' is traded by {} (several strategies trade one "
                        "instrument only when [gateway.shared] names it)",
                        symbol,
                        venue,
                        other->who()));
        return;
      }
      if (std::find(owned.begin(), owned.end(), inst->id) == owned.end()) owned.push_back(inst->id);
    }
    if (owned.empty()) {
      refuse_attach("the attach request names no instrument to trade");
      return;
    }
    // An [engine] name is one strategy: its store, and its share of shared instruments.
    for (const auto& other : atts_) {
      if (other->engine != engine) continue;
      refuse_attach("a strategy with [engine] name '" + engine +
                    "' is attached already: " + other->who());
      return;
    }
    std::vector<std::uint16_t> past;
    const std::byte* epochs_at = buf.data() + sizeof req + req.known_count * sizeof(gw::ExecId) +
                                 req.claim_count * sizeof(gw::InstrumentClaim) +
                                 req.position_count * sizeof(gw::PositionSeed) +
                                 req.resume_count * sizeof(gw::VenueResume);
    for (std::uint32_t i = 0; i < req.epoch_count; ++i) {
      std::uint16_t e = 0;
      std::memcpy(&e, epochs_at + i * sizeof e, sizeof e);
      past.push_back(e);
    }
    // The positions it restored, of the instruments it claimed.
    std::vector<Seed> seeds;
    const std::byte* positions = claims + req.claim_count * sizeof(gw::InstrumentClaim);
    for (std::uint32_t i = 0; i < req.position_count; ++i) {
      gw::PositionSeed ps{};
      std::memcpy(&ps, positions + i * sizeof ps, sizeof ps);
      const VenueId vid = cfg_.venue_id(from_field(ps.venue));
      const Instrument* inst =
          vid.valid() ? instruments_.find(vid, from_field(ps.symbol)) : nullptr;
      if (inst == nullptr || std::find(owned.begin(), owned.end(), inst->id) == owned.end()) {
        FASTMM_LOG_WARN("gateway: {} reports a position in {} on '{}', which it does not claim",
                        std::string_view(engine),
                        from_field(ps.symbol),
                        from_field(ps.venue));
        continue;
      }
      seeds.push_back(Seed{inst->id, Qty::from_raw(ps.qty), Price::from_raw(ps.avg_px)});
    }
    // Where each venue's execution replay starts for it, and the trade ids its store holds there.
    std::vector<Resume> resumes(slots_.size());
    const std::byte* resume_at = positions + req.position_count * sizeof(gw::PositionSeed);
    for (std::uint32_t i = 0; i < req.resume_count; ++i) {
      gw::VenueResume vr{};
      std::memcpy(&vr, resume_at + i * sizeof vr, sizeof vr);
      const std::string venue = from_field(vr.venue);
      const VenueId vid = cfg_.venue_id(venue);
      if (vr.first_known > req.known_count || vr.known_count > req.known_count - vr.first_known) {
        refuse_attach("malformed attach request: the trade ids of venue '" + venue +
                      "' are out of range");
        return;
      }
      if (!vid.valid() || vid.value >= resumes.size()) {
        FASTMM_LOG_WARN("gateway: {} resumes executions on '{}', which is not a venue here",
                        std::string_view(engine),
                        std::string_view(venue));
        continue;
      }
      Resume& r = resumes[vid.value];
      r.set = vr.since_ms > 0;
      r.since_ms = vr.since_ms;
      r.known.assign(known.begin() + vr.first_known,
                     known.begin() + vr.first_known + vr.known_count);
    }
    // Its earlier sessions' orders by the venue's id of them, on the instruments it claims.
    const std::byte* orders_at = epochs_at + req.epoch_count * sizeof(std::uint16_t);
    for (std::uint32_t i = 0; i < req.order_count; ++i) {
      gw::PastOrder po{};
      std::memcpy(&po, orders_at + i * sizeof po, sizeof po);
      const VenueId vid = cfg_.venue_id(from_field(po.venue));
      const Instrument* inst =
          vid.valid() ? instruments_.find(vid, from_field(po.symbol)) : nullptr;
      const ClientOrderId id{po.cl_ord_id};
      if (inst == nullptr || vid.value >= resumes.size() || !id.valid() ||
          std::find(owned.begin(), owned.end(), inst->id) == owned.end())
        continue;
      resumes[vid.value].orders.push_back(
          PastOrder{inst->id, VenueOrderId(from_field(po.venue_order_id)), id});
    }
    attach(fd, req, engine, resumes, std::move(owned), seeds, past);
  }

  // The attachment's connection closed (or misbehaved): the strategy is gone.
  void detach(Attachment& a, std::string_view why) {
    const std::int64_t t0 = steady_now().ns;
    std::vector<std::size_t> cancelled(slots_.size(), 0);
    // Out of the routers first, so nothing of its reaches the wire any more; then its orders go.
    on_net_threads(slots_, [&](std::size_t i) {
      VenueRouter& v = *routers_[i];
      Route* r = &a.routes[i];
      for (std::size_t k = 0; k < v.routes.size(); ++k) {
        if (v.routes[k] == r) {
          v.routes.erase_at(k);
          break;
        }
      }
      for (Route*& o : v.owner) {
        if (o == r) o = nullptr;
      }
      if (v.by_slot[r->slot] == r) v.by_slot[r->slot] = nullptr;
      if (opts_.dry_run) return;
      std::vector<ClientOrderId> mine;
      v.orders.for_each_key([&](ClientOrderId id) {
        const GwOrder* o = v.orders.find(id);
        if (o->epoch == a.epoch && (o->flags & GwOrder::kCancelSent) == 0) mine.push_back(id);
      });
      for (const ClientOrderId id : mine) {
        const GwOrder o = *v.orders.find(id);
        queue_cancel(v, id, o.inst, o.venue_order_id);
      }
      cancelled[i] = mine.size();
      if (send_cancels(v) != 0) v.slot->venue->on_wake();
      // And ask the venue what it holds: a row of this epoch that the gateway did not know is
      // cancelled when the snapshot comes back.
      v.sweep_pending = true;
      v.slot->venue->request_open_orders();
    });
    gw::unmap_wake_page(a.page);
    a.page = nullptr;
    a.engine_waker.reset();
    remove_rings(a.rings);
    a.rings.clear();
    ::close(a.fd);
    a.fd = -1;
    for (const InstrumentId id : a.owned) {
      if (owner_[id.value] == &a) owner_[id.value] = nullptr;
    }
    // Its positions are the account's, not the process's: they stay, and the fills of its orders
    // still in flight are booked into them.
    std::string kept;
    for (const InstrumentId id : a.owned) {
      const Qty q = Qty::from_raw(acct_.qty[id.value].load(std::memory_order_relaxed));
      kept += fmt::format(" {}={}", instruments_.get(id).symbol.view(), dec(q));
    }
    FASTMM_LOG_INFO("gateway: the account keeps the positions of {}:{}",
                    std::string_view(a.who()),
                    std::string_view(kept));
    std::size_t total = 0;
    for (const std::size_t c : cancelled) total += c;
    const std::int64_t ms = (steady_now().ns - t0) / 1'000'000;
    const double held_s = static_cast<double>(steady_now().ns - a.since_ns) / 1e9;
    FASTMM_LOG_WARN(
        "gateway: {} detached after {:.1f} s ({}): {} order(s) of its epoch cancelled, sent in {} "
        "ms; its {} instrument(s) are free",
        std::string_view(a.who()),
        held_s,
        why,
        total,
        ms,
        a.owned.size());
    for (std::size_t k = 0; k < atts_.size(); ++k) {
      if (atts_[k].get() == &a) {
        atts_.erase(atts_.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
    }
  }

  void detach_all(std::string_view why) {
    while (!atts_.empty()) detach(*atts_.back(), why);
  }

  // An attachment stopped reading its order ring: the venue could not hand it an order event.
  void check_overflows() {
    for (std::size_t k = 0; k < atts_.size(); ++k) {
      Attachment& a = *atts_[k];
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!a.routes[i].overflow.load(std::memory_order_acquire)) continue;
        FASTMM_LOG_ERROR("gateway: [{}] the order ring of {} is full",
                         slots_[i]->venue->name(),
                         std::string_view(a.who()));
        detach(a, "its order ring overflowed");
        return;  // atts_ changed
      }
    }
  }

  // Once a second: what was dropped, refused or had nowhere to go.
  void log_counters() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      const VenueRouter& v = *routers_[i];
      const std::uint64_t md = v.md_discarded.load(std::memory_order_relaxed);
      const std::uint64_t order = v.order_discarded.load(std::memory_order_relaxed);
      const std::uint64_t unrouted = v.unrouted.load(std::memory_order_relaxed);
      const std::uint64_t cancels = v.gateway_cancels.load(std::memory_order_relaxed);
      const std::uint64_t rate = v.refused_rate.load(std::memory_order_relaxed);
      const std::uint64_t notional = v.refused_notional.load(std::memory_order_relaxed);
      const std::uint64_t owner = v.refused_owner.load(std::memory_order_relaxed);
      const std::uint64_t killed = v.refused_killed.load(std::memory_order_relaxed);
      const std::uint64_t gross = v.refused_gross.load(std::memory_order_relaxed);
      const std::uint64_t net = v.refused_net.load(std::memory_order_relaxed);
      const std::uint64_t fx = v.refused_fx.load(std::memory_order_relaxed);
      const std::uint64_t und = v.refused_underlying.load(std::memory_order_relaxed) +
                                v.refused_underlying_mark.load(std::memory_order_relaxed);
      const std::uint64_t self = v.refused_self_trade.load(std::memory_order_relaxed);
      const std::uint64_t skipped = v.account_skipped.load(std::memory_order_relaxed);
      const std::uint64_t md_lost = v.account_md_lost.load(std::memory_order_relaxed);
      const std::uint64_t untracked = v.untracked.load(std::memory_order_relaxed);
      const std::uint64_t stale = v.stale_replays.load(std::memory_order_relaxed);
      const std::uint64_t parked = v.parked_count.load(std::memory_order_relaxed);
      const std::uint64_t parked_dropped = v.parked_dropped.load(std::memory_order_relaxed);
      Logged& l = logged_[i];
      if (md != l.md || order != l.order || unrouted != l.unrouted || cancels != l.cancels ||
          rate != l.rate || notional != l.notional || owner != l.owner || killed != l.killed ||
          gross != l.gross || net != l.net || fx != l.fx || und != l.und || self != l.self ||
          untracked != l.untracked || stale != l.stale || skipped != l.skipped ||
          md_lost != l.md_lost || parked != l.parked || parked_dropped != l.parked_dropped) {
        FASTMM_LOG_INFO(
            "gateway: [{}] discarded with nothing attached: md={} order={}; order events for no "
            "attachment: {}; gateway cancels: {}; refused: rate={} open_notional={} not_owner={} "
            "account_killed={} gross_notional={} net_notional={} fx_rate={} underlying={} "
            "self_trade={}; "
            "untracked: {}; "
            "replayed fills "
            "older than their owner's history: {}, than the account's: {}; account books lost: {}; "
            "executions kept for a strategy to claim: {} ({} dropped)",
            slots_[i]->venue->name(),
            md,
            order,
            unrouted,
            cancels,
            rate,
            notional,
            owner,
            killed,
            gross,
            net,
            fx,
            und,
            self,
            untracked,
            stale,
            skipped,
            md_lost,
            parked,
            parked_dropped);
        l = Logged{md,
                   order,
                   unrouted,
                   cancels,
                   rate,
                   notional,
                   owner,
                   killed,
                   gross,
                   net,
                   fx,
                   und,
                   self,
                   untracked,
                   stale,
                   skipped,
                   md_lost,
                   parked,
                   parked_dropped};
      }
      for (const auto& a : atts_) {
        const std::uint64_t d = a->routes[i].md_dropped.load(std::memory_order_relaxed);
        if (d == a->md_dropped_logged[i]) continue;
        FASTMM_LOG_WARN("gateway: [{}] {} could not keep up: {} market-data event(s) dropped",
                        slots_[i]->venue->name(),
                        std::string_view(a->who()),
                        d);
        a->md_dropped_logged[i] = d;
      }
    }
  }

  // The account's kill switch tripped (a network thread saw the loss): latch it in the kill file
  // and cancel everything the venues hold. The network threads have refused orders since, told
  // every strategy and cancelled what they know.
  void check_trip() {
    if (trip_handled_ || !acct_.tripped.load(std::memory_order_acquire)) return;
    trip_handled_ = true;
    persist_kill();
    const std::string latch =
        acct_.max_loss > 0 ? "latched in " + kill_path_ : std::string("latched until clear-kill");
    if (acct_.trip_reason.load(std::memory_order_acquire) == KillReason::GatewayOperator) {
      FASTMM_LOG_ERROR(
          "gateway: account kill switch tripped by the operator (net PnL {}); orders refused, "
          "every strategy's venues killed, every open order cancelled; {}",
          Notional::from_raw(acct_.trip_net.load(std::memory_order_relaxed)),
          std::string_view(latch));
    } else {
      FASTMM_LOG_ERROR(
          "gateway: account kill switch tripped: net PnL {} <= -[gateway] max_loss {} over every "
          "strategy; orders refused, every strategy's venues killed, every open order cancelled; "
          "{}",
          Notional::from_raw(acct_.trip_net.load(std::memory_order_relaxed)),
          Notional::from_raw(acct_.max_loss),
          std::string_view(latch));
    }
    if (opts_.dry_run) return;
    for (auto& s : slots_) {
      if (!s->venue->cancel_all())
        FASTMM_LOG_ERROR("gateway: [{}] cancel_all after the account trip FAILED",
                         s->venue->name());
    }
  }

  // Writes the account's cumulative PnL (and a trip) to the kill file when it changed.
  void persist_kill() {
    if (acct_.max_loss <= 0) return;
    KillState st = kill_;
    for (std::size_t i = 0; i < acct_.venue_count; ++i) {
      const Account::Sum t = acct_.venue_sum(i);
      st.realized += Notional::from_raw(t.realized);
      st.fees += Notional::from_raw(t.fees);
    }
    if (acct_.tripped.load(std::memory_order_acquire)) {
      st.latched = true;
      st.reason = acct_.trip_reason.load(std::memory_order_acquire);
    }
    if (kill_written_ && st.realized == kill_last_.realized && st.fees == kill_last_.fees &&
        st.latched == kill_last_.latched)
      return;
    st.updated_ns = wall_now().ns;
    kill_last_ = st;
    kill_written_ = true;
    if (auto r = KillStateStore::store(kill_path_, st); !r && !kill_write_failed_) {
      kill_write_failed_ = true;
      FASTMM_LOG_ERROR("gateway: cannot persist the account's kill state: {}",
                       std::string_view(r.error()));
    }
  }

  // Once a second (when it changed) and at the end: the account over every strategy, then each
  // instrument whose position changed.
  void log_account(bool force = false) {
    std::array<std::int64_t, 5> now{};  // realized, unrealized, fees, gross, net
    for (std::size_t i = 0; i < acct_.venue_count; ++i) {
      const Account::Sum t = acct_.venue_sum(i);
      now[0] += t.realized;
      now[1] += t.unrealized;
      now[2] += t.fees;
      now[3] += t.gross;
      now[4] += t.net;
    }
    const bool tripped = acct_.tripped.load(std::memory_order_acquire);
    if (force || now != account_logged_ || tripped != account_logged_tripped_) {
      std::string_view kill = "off";
      if (tripped) {
        kill = "LATCHED";
      } else if (acct_.max_loss > 0) {
        kill = "armed";
      }
      FASTMM_LOG_INFO(
          "gateway: account net_pnl={} realized={} unrealized={} fees={} carried={} "
          "gross_exposure={} net_exposure={} kill={} max_loss={}{}",
          Notional::from_raw(acct_.carry.load(std::memory_order_relaxed) + now[0] + now[1] -
                             now[2]),
          Notional::from_raw(now[0]),
          Notional::from_raw(now[1]),
          Notional::from_raw(now[2]),
          Notional::from_raw(acct_.carry.load(std::memory_order_relaxed)),
          Notional::from_raw(now[3]),
          Notional::from_raw(now[4]),
          kill,
          Notional::from_raw(acct_.max_loss),
          acct_.fx.active() ? fmt::format(" in={}", acct_.fx.reporting()) : std::string());
      account_logged_ = now;
      account_logged_tripped_ = tripped;
    }
    for (std::size_t u = 0; u < acct_.und.count; ++u) {
      std::int64_t net = 0;
      std::int64_t working = 0;
      const bool known =
          acct_.underlying_position(u, instruments_, nullptr, steady_now().ns, net, working);
      const std::int64_t shown = known ? net : std::numeric_limits<std::int64_t>::min();
      if (shown == und_logged_[u] && !force) continue;
      und_logged_[u] = shown;
      FASTMM_LOG_INFO("gateway: account underlying {} net={}{}",
                      acct_.und.names[u],
                      Qty::from_raw(net),
                      known ? "" : " (an inverse contract has no current mark)");
    }
    for (const Instrument& inst : instruments_) {
      const std::size_t i = inst.id.value;
      const std::int64_t q = acct_.qty[i].load(std::memory_order_relaxed);
      if (!acct_.shared[i]) {
        if (q == qty_logged_[i] && (!force || q == 0)) continue;
        qty_logged_[i] = q;
        FASTMM_LOG_INFO("gateway: account position {}:{} {}",
                        slots_[inst.venue.value]->venue->name(),
                        inst.symbol,
                        Qty::from_raw(q));
        continue;
      }
      // A shared instrument: the account's position, each strategy's share and what nobody holds;
      // the account (the venue's position when it reports one) should be their sum. Logged when
      // any of them changed.
      std::string parts;
      for (std::size_t k = 0; k < slot_names_.size(); ++k) {
        if (!share_seeded_[i].test(k) && acct_.primary[i] != k + 1) continue;
        parts += fmt::format(" {}={}",
                             slot_names_[k],
                             dec(Qty::from_raw(acct_.share[i][k].load(std::memory_order_relaxed))));
      }
      const std::int64_t off = acct_.unexplained[i].load(std::memory_order_relaxed);
      parts +=
          fmt::format(" unattributed={}",
                      dec(Qty::from_raw(acct_.unattributed[i].load(std::memory_order_relaxed))));
      if (parts == shared_logged_[i] && q == qty_logged_[i] && off == unexplained_logged_[i] &&
          (!force || share_seeded_[i].none()))
        continue;
      shared_logged_[i] = parts;
      qty_logged_[i] = q;
      unexplained_logged_[i] = off;
      if (off == 0) {
        FASTMM_LOG_INFO("gateway: account position {}:{} {} (shared:{})",
                        slots_[inst.venue.value]->venue->name(),
                        inst.symbol,
                        Qty::from_raw(q),
                        std::string_view(parts));
      } else {
        FASTMM_LOG_WARN(
            "gateway: account position {}:{} {} does not match its strategies' (shared:{}): off "
            "by {}",
            slots_[inst.venue.value]->venue->name(),
            inst.symbol,
            Qty::from_raw(q),
            std::string_view(parts),
            Qty::from_raw(off));
      }
    }
  }

  [[nodiscard]] std::string latched_message() const {
    const bool operator_kill =
        acct_.trip_reason.load(std::memory_order_acquire) == KillReason::GatewayOperator;
    return fmt::format(
        "the account kill switch is latched ({}, net PnL {}){}. Check the positions, then clear "
        "it with fastmm-ctl --gateway {} clear-kill, or restart the gateway with "
        "fastmm-gateway --clear-kill",
        operator_kill ? "an operator's kill" : "[gateway] max_loss",
        dec(Notional::from_raw(acct_.trip_net.load(std::memory_order_relaxed))),
        acct_.max_loss > 0 ? " in " + kill_path_ : std::string(),
        cfg_.engine.name);
  }

  // ---- status segment (fastmm-top) and control socket (fastmm-ctl), main thread ---------------

  [[nodiscard]] bool open_status(const std::string& path, std::string* error) {
    started_ns_ = wall_now().ns;
    return status_.open(path, error);
  }

  // The snapshot, from what the network threads publish (their atomics) and what this thread
  // owns (the attachments, the owners): no network thread is asked for anything.
  [[nodiscard]] StatusSnapshot snapshot(StatusRunState state) const {
    StatusSnapshot s;
    s.kind = StatusKind::Gateway;
    s.state = state;
    s.pid = static_cast<std::uint32_t>(::getpid());
    s.started_ns = started_ns_;
    s.updated_ns = wall_now().ns;
    s.dry_run = opts_.dry_run ? 1 : 0;
    set_status_name(s.engine_name, cfg_.engine.name);
    set_status_name(s.strategy, "fastmm-gateway");
    StatusGateway& g = s.gateway;
    const bool tripped = acct_.tripped.load(std::memory_order_acquire);
    const KillReason why = acct_.trip_reason.load(std::memory_order_acquire);
    if (tripped) {
      g.kill_active = 1;
      s.kill_flags = 1;
      s.kill_reason = static_cast<std::uint8_t>(why);
      g.trip_net_raw = acct_.trip_net.load(std::memory_order_relaxed);
    }
    s.kill_latched = tripped && acct_.max_loss > 0 ? 1 : 0;
    s.pnl_carry_raw = acct_.carry.load(std::memory_order_relaxed);
    g.max_loss_raw = acct_.max_loss;
    g.max_gross_raw = acct_.max_gross;
    g.max_net_raw = acct_.max_net;
    g.max_open_notional_raw = routers_.empty() ? 0 : routers_[0]->max_open_notional;
    s.venue_count = static_cast<std::uint8_t>(std::min(slots_.size(), kStatusMaxVenues));
    for (std::size_t i = 0; i < s.venue_count; ++i) {
      StatusVenue& sv = s.venues[i];
      set_status_name(sv.name, slots_[i]->venue->name());
      fill_status_venue(slots_[i]->venue->status(), sv);
      sv.killed = tripped ? 1 : 0;
      sv.kill_reason = tripped ? static_cast<std::uint8_t>(why) : 0;
      const VenueRouter& v = *routers_[i];
      StatusGatewayVenue& gv = g.venues[i];
      gv.md_discarded = v.md_discarded.load(std::memory_order_relaxed);
      gv.order_discarded = v.order_discarded.load(std::memory_order_relaxed);
      gv.unrouted = v.unrouted.load(std::memory_order_relaxed);
      gv.gateway_cancels = v.gateway_cancels.load(std::memory_order_relaxed);
      gv.untracked = v.untracked.load(std::memory_order_relaxed);
      gv.stale_replays = v.stale_replays.load(std::memory_order_relaxed);
      gv.account_skipped = v.account_skipped.load(std::memory_order_relaxed);
      gv.account_md_lost = v.account_md_lost.load(std::memory_order_relaxed);
      // kStatusGatewayRefusalReasons order.
      gv.refused[0] = v.refused_owner.load(std::memory_order_relaxed);
      gv.refused[1] = v.refused_killed.load(std::memory_order_relaxed);
      gv.refused[2] = v.refused_notional.load(std::memory_order_relaxed);
      gv.refused[3] = v.refused_gross.load(std::memory_order_relaxed);
      gv.refused[4] = v.refused_net.load(std::memory_order_relaxed);
      gv.refused[5] = v.refused_rate.load(std::memory_order_relaxed);
      gv.refused[6] = v.refused_fx.load(std::memory_order_relaxed);
      gv.refused[7] = v.refused_underlying.load(std::memory_order_relaxed);
      gv.refused[8] = v.refused_underlying_mark.load(std::memory_order_relaxed);
      gv.refused[9] = v.refused_self_trade.load(std::memory_order_relaxed);
      gv.refused[10] = v.refused_balance.load(std::memory_order_relaxed);
      const VenueRouter::BalancePub bp = v.balance_pub.load();
      for (std::uint32_t k = 0; k < bp.count && s.balance_count < kStatusMaxBalances; ++k) {
        const LiveBalance& lb = bp.rows[k];
        StatusBalance& sb = s.balances[s.balance_count++];
        set_status_name(sb.asset, std::string_view(lb.asset, ::strnlen(lb.asset, sizeof lb.asset)));
        sb.venue = lb.venue;
        sb.account = lb.account;
        sb.known = lb.known;
        sb.free_raw = lb.free_raw;
        sb.locked_raw = lb.locked_raw;
        sb.total_raw = lb.total_raw;
        sb.equity_raw = lb.equity_raw;
        sb.maintenance_raw = lb.maintenance_raw;
        sb.as_of_ns = lb.as_of_ns;
      }
      const VenueRouter::PerpPub pp = v.perp_pub.load();
      for (std::uint32_t k = 0; k < pp.count && s.perp_count < kStatusMaxPerps; ++k) {
        const LivePerp& lp = pp.rows[k];
        const InstrumentId id{lp.instrument};
        to_status_perp(s.perps[s.perp_count++],
                       lp,
                       instruments_.contains(id) ? instruments_.get(id).symbol.view() : "?");
      }
      const Account::Sum t = acct_.venue_sum(i);
      gv.realized_raw = t.realized;
      gv.unrealized_raw = t.unrealized;
      gv.fees_raw = t.fees;
      gv.gross_raw = t.gross;
      gv.net_raw = t.net;
      s.realized_pnl_raw += gv.realized_raw;
      s.unrealized_pnl_raw += gv.unrealized_raw;
      s.fees_raw += gv.fees_raw;
      g.gross_raw += gv.gross_raw;
      g.net_raw += gv.net_raw;
    }
    g.net_pnl_raw = s.pnl_carry_raw + s.realized_pnl_raw + s.unrealized_pnl_raw - s.fees_raw;
    g.attachment_count = static_cast<std::uint32_t>(std::min(atts_.size(), kStatusMaxAttachments));
    for (std::size_t k = 0; k < g.attachment_count; ++k) {
      const Attachment& a = *atts_[k];
      StatusAttachment& sa = g.attachments[k];
      set_status_name(sa.engine, a.engine);
      sa.pid = a.pid;
      sa.id = a.id;
      sa.epoch = a.epoch;
      sa.blocks = a.blocks ? 1 : 0;
      sa.attached_ns = a.since_wall_ns;
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Route& r = a.routes[i];
        sa.md_dropped += r.md_dropped.load(std::memory_order_relaxed);
        for (std::size_t n = 0; n < kStatusGatewayRefusals; ++n)
          sa.refused[n] += r.refused[n].load(std::memory_order_relaxed);
      }
    }
    for (const Instrument& inst : instruments_) {
      if (g.position_count == kStatusMaxPositions) break;
      const std::size_t k = g.position_count++;
      StatusPosition& p = g.positions[k];
      set_status_name(p.symbol, inst.symbol.view());
      p.venue = inst.venue.value;
      p.qty_raw = acct_.qty[inst.id.value].load(std::memory_order_relaxed);
      if (const Attachment* o = owner_[inst.id.value]) p.owner_epoch = o->epoch;
      p.shared = acct_.shared[inst.id.value] ? 1 : 0;
      if (p.shared != 0) {
        p.unattributed_raw = acct_.unattributed[inst.id.value].load(std::memory_order_relaxed);
        p.unexplained_raw = acct_.unexplained[inst.id.value].load(std::memory_order_relaxed);
      }
      for (std::size_t n = 0; n < g.attachment_count; ++n) {
        const auto& owned = atts_[n]->owned;
        if (std::find(owned.begin(), owned.end(), inst.id) == owned.end()) continue;
        ++p.traders;
        g.attachments[n].instruments[k / 64] |= std::uint64_t{1} << (k % 64);
      }
    }
    for (std::size_t u = 0; u < acct_.und.count && u < kStatusMaxUnderlyings; ++u) {
      StatusUnderlying& su = g.underlyings[u];
      set_status_name(su.name, acct_.und.names[u].view());
      std::int64_t working = 0;
      su.known =
          acct_.underlying_position(u, instruments_, nullptr, steady_now().ns, su.net_raw, working)
              ? 1
              : 0;
      su.max_net_raw = acct_.und.max_net[u].raw;
    }
    return s;
  }

  void publish_status(StatusRunState state) {
    if (status_.is_open()) status_.publish(snapshot(state));
  }

  // One request on the control socket, its reply.
  [[nodiscard]] std::string command(std::string_view request) {
    std::vector<std::string_view> words;
    for (std::size_t i = 0; i < request.size();) {
      while (i < request.size() && std::isspace(static_cast<unsigned char>(request[i])) != 0) ++i;
      const std::size_t start = i;
      while (i < request.size() && std::isspace(static_cast<unsigned char>(request[i])) == 0) ++i;
      if (i > start) words.push_back(request.substr(start, i - start));
    }
    if (words.empty()) return "error empty command; `help` lists them\n";
    const std::string_view verb = words[0];
    if (verb == "help") return std::string(gateway_control_usage());
    if (verb == "status") {
      if (words.size() > 1) return "error status takes no arguments\n";
      return format_status(snapshot(StatusRunState::Running), wall_now().ns, /*color=*/false);
    }
    if (verb == "attachments") {
      if (words.size() > 1) return "error attachments takes no arguments\n";
      return attachments_text();
    }
    if (verb == "pull" || verb == "resume") return pull_or_resume(request);
    if (verb == "kill") {
      if (words.size() > 1) return "error kill takes no arguments\n";
      return operator_kill();
    }
    if (verb == "clear-kill") {
      if (words.size() > 1) return "error clear-kill takes no arguments\n";
      return clear_kill();
    }
    return "error unknown command '" + std::string(verb) + "'; `help` lists them\n";
  }

 private:
  // One line per attachment: id, epoch, engine, pid, what it trades, its drops and refusals.
  [[nodiscard]] std::string attachments_text() const {
    if (atts_.empty()) return "ok no strategy attached\n";
    std::string out;
    for (const auto& ap : atts_) {
      const Attachment& a = *ap;
      std::string symbols;
      std::string shares;  // its share of each shared instrument
      for (const InstrumentId inst : a.owned) {
        const Instrument& i = instruments_.get(inst);
        symbols += fmt::format("{}{}:{}",
                               symbols.empty() ? "" : ",",
                               slots_[i.venue.value]->venue->name(),
                               i.symbol.view());
        if (acct_.shared[inst.value])
          shares += fmt::format(
              "{}{}:{}={}",
              shares.empty() ? " share=" : ",",
              slots_[i.venue.value]->venue->name(),
              i.symbol.view(),
              dec(Qty::from_raw(acct_.share[inst.value][a.slot].load(std::memory_order_relaxed))));
      }
      std::uint64_t dropped = 0;
      std::uint64_t refused = 0;
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        dropped += a.routes[i].md_dropped.load(std::memory_order_relaxed);
        for (const auto& n : a.routes[i].refused) refused += n.load(std::memory_order_relaxed);
      }
      fmt::format_to(std::back_inserter(out),
                     "attachment={} epoch={} engine={} pid={} up={:.0f}s instruments={} "
                     "md_dropped={} refused={}{}\n",
                     a.id,
                     a.epoch,
                     a.engine,
                     a.pid,
                     static_cast<double>(steady_now().ns - a.since_ns) / 1e9,
                     symbols,
                     dropped,
                     refused,
                     shares);
    }
    return out;
  }

  // `pull` / `resume` with the engine's scope flags, parsed by control_command() into the engine's
  // ControlMsg, which goes to the strategies the scope reaches: an instrument's owner, every
  // attachment for a venue or for everything.
  [[nodiscard]] std::string pull_or_resume(std::string_view request) {
    std::size_t sent = 0;
    std::string refusal;
    ControlPlane plane;
    plane.instruments = &instruments_;
    plane.venue = [&](std::string_view name, VenueId& out) {
      for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i]->venue->name() != name) continue;
        out = VenueId{static_cast<std::uint8_t>(i)};
        return true;
      }
      return false;
    };
    plane.submit = [&](const EventHeader& h) {
      ControlMsg m = msg_cast<ControlMsg>(&h);
      m.hdr.t0_cycles = rdtscp();
      if (h.instrument.valid() && traders(h.instrument) == 0) {
        refusal =
            fmt::format("no strategy trades {}", instruments_.get(h.instrument).symbol.view());
        return true;
      }
      if (atts_.empty()) {
        refusal = "no strategy attached";
        return true;
      }
      sent = send_control(m);
      return true;
    };
    std::string reply = control_command(request, plane);
    if (!refusal.empty()) return "error " + refusal + "\n";
    if (!reply.starts_with("ok")) return reply;
    FASTMM_LOG_WARN("gateway: control socket: {} sent to {} strategies",
                    std::string_view(reply).substr(0, reply.size() - 1),
                    sent);
    return reply.substr(0, reply.size() - 1) + fmt::format(", sent to {} strategies\n", sent);
  }

  // Puts `m` on the order rings of the attachments its scope reaches, on the network threads that
  // own those rings. An instrument goes to its owner on its venue; a venue to every attachment on
  // that venue; everything to every attachment once, on the first venue. Returns how many.
  std::size_t send_control(const ControlMsg& m) {
    std::atomic<std::size_t> sent{0};
    VenueId target = m.hdr.venue;
    if (m.hdr.instrument.valid()) target = instruments_.get(m.hdr.instrument).venue;
    if (!target.valid()) target = VenueId{0};
    on_net_threads(slots_, [&](std::size_t i) {
      if (i != target.value) return;
      VenueRouter& v = *routers_[i];
      if (m.hdr.instrument.valid()) {
        for (Route* r : v.routes) {
          if (!r->claims[m.hdr.instrument.value]) continue;
          push_order(*r, m.hdr);
          sent.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }
      for (Route* r : v.routes) {
        push_order(*r, m.hdr);
        sent.fetch_add(1, std::memory_order_relaxed);
      }
    });
    return sent.load(std::memory_order_relaxed);
  }

  // `kill`: the account's kill switch, as a max_loss trip does it (KillReason::GatewayOperator).
  [[nodiscard]] std::string operator_kill() {
    if (!trip_account(acct_, acct_.net_pnl(), KillReason::GatewayOperator))
      return "error the account kill switch is tripped already (" +
             std::string(to_string(acct_.trip_reason.load(std::memory_order_acquire))) + ")\n";
    FASTMM_LOG_WARN("gateway: control socket: kill");
    check_trip();
    return fmt::format(
        "ok account kill switch tripped (GatewayOperator): orders refused, every strategy's venues "
        "killed, every open order cancelled, attaches refused until clear-kill ({} strategies "
        "attached)\n",
        atts_.size());
  }

  // `clear-kill`: the account's kill switch cleared and its loss budget armed again, as
  // fastmm-gateway --clear-kill does on a restart. Refused while a strategy is attached: every one
  // attached at the trip was killed by it and stays killed in its own engine, so it has to go
  // first (the next attach starts clean).
  [[nodiscard]] std::string clear_kill() {
    if (!acct_.tripped.load(std::memory_order_acquire)) return "error the account is not killed\n";
    if (!atts_.empty()) {
      std::string who;
      for (const auto& a : atts_) who += (who.empty() ? "" : ", ") + a->who();
      return fmt::format(
          "error {} strategies are still attached, killed by the trip: stop them first ({})\n",
          atts_.size(),
          who);
    }
    check_trip();
    // The budget starts again from here: this run's realized PnL and fees so far are carried as
    // their negative, so the account's net PnL is its unrealized PnL.
    std::int64_t realized = 0;
    std::int64_t fees = 0;
    for (std::size_t i = 0; i < acct_.venue_count; ++i) {
      const Account::Sum t = acct_.venue_sum(i);
      realized += t.realized;
      fees += t.fees;
    }
    KillState fresh;
    fresh.sessions = kill_.sessions;
    fresh.realized = Notional::from_raw(-realized);
    fresh.fees = Notional::from_raw(-fees);
    kill_ = fresh;
    acct_.carry.store(kill_.carry().raw, std::memory_order_relaxed);
    if (acct_.max_loss > 0) {
      if (auto r = KillStateStore::clear(kill_path_); !r)
        return "error cannot clear the kill file: " + r.error() + "\n";
    }
    kill_written_ = false;
    // Orders flow again once `tripped` is false; the reason is cleared after it, so a network
    // thread that trips in between finds the reason set and trips on its next check instead.
    acct_.tripped.store(false, std::memory_order_release);
    acct_.trip_net.store(0, std::memory_order_relaxed);
    acct_.trip_reason.store(KillReason::None, std::memory_order_release);
    on_net_threads(slots_, [&](std::size_t i) { routers_[i]->killed = false; });
    trip_handled_ = false;
    persist_kill();
    FASTMM_LOG_WARN(
        "gateway: control socket: clear-kill: the account kill switch is cleared and the whole "
        "[gateway] max_loss budget is armed again; strategies may attach");
    return "ok account kill switch cleared, the loss budget armed again; strategies may attach\n";
  }

  struct Logged {
    std::uint64_t md = 0;
    std::uint64_t order = 0;
    std::uint64_t unrouted = 0;
    std::uint64_t cancels = 0;
    std::uint64_t rate = 0;
    std::uint64_t notional = 0;
    std::uint64_t owner = 0;
    std::uint64_t killed = 0;
    std::uint64_t gross = 0;
    std::uint64_t net = 0;
    std::uint64_t fx = 0;
    std::uint64_t und = 0;
    std::uint64_t self = 0;
    std::uint64_t untracked = 0;
    std::uint64_t stale = 0;
    std::uint64_t skipped = 0;
    std::uint64_t md_lost = 0;
    std::uint64_t parked = 0;
    std::uint64_t parked_dropped = 0;
  };

  // A session epoch no live attachment has, from the gateway's epoch file (fail closed).
  [[nodiscard]] Result<std::uint16_t, std::string> next_epoch() const {
    if (const std::filesystem::path p(cfg_.engine.epoch_file); p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    for (std::size_t tries = 0; tries <= gw::kMaxAttachments; ++tries) {
      bool wrapped = false;
      auto e = SessionEpochStore::next_epoch(cfg_.engine.epoch_file, &wrapped);
      if (!e) return e;
      if (wrapped)
        FASTMM_LOG_WARN(
            "gateway: the session epoch has cycled past 65535: client order ids of sessions that "
            "long ago can repeat");
      bool taken = false;
      for (const auto& a : atts_) taken = taken || a->epoch == *e;
      if (!taken) return *e;
    }
    return fail(std::string("every session epoch tried is held by a live attachment"));
  }

  void attach(int fd,
              const gw::AttachRequest& req,
              const std::string& engine,
              const std::vector<Resume>& resumes,
              std::vector<InstrumentId> owned,
              const std::vector<Seed>& seeds,
              const std::vector<std::uint16_t>& past_epochs) {
    const auto epoch = next_epoch();
    if (!epoch) {
      FASTMM_LOG_ERROR("gateway: cannot give {} a session epoch: {}",
                       std::string_view(engine),
                       std::string_view(epoch.error()));
      send_error(fd, "cannot assign a session epoch: " + epoch.error());
      ::close(fd);
      return;
    }
    const std::optional<std::uint8_t> slot = slot_of(engine);
    if (!slot) {
      FASTMM_LOG_ERROR("gateway: refused {} (pid {}): {} strategies have attached already",
                       std::string_view(engine),
                       req.pid,
                       Account::kMaxSlots);
      send_error(fd,
                 fmt::format("the gateway has seen {} [engine] names already; restart it to "
                             "attach another",
                             Account::kMaxSlots));
      ::close(fd);
      return;
    }
    const auto tag = static_cast<std::uint8_t>(*slot + 1U);
    // Executions of its orders are its own: those of this session, of its earlier sessions this
    // gateway gave an epoch (kept since), and of those its store lists. An epoch another strategy
    // holds stays theirs.
    acct_.epoch_slot[*epoch].store(tag, std::memory_order_relaxed);
    for (const std::uint16_t e : past_epochs) {
      if (e == 0 || e == *epoch) continue;
      const std::uint8_t had = acct_.epoch_slot[e].load(std::memory_order_relaxed);
      if (had != 0 && had != tag) {
        FASTMM_LOG_WARN("gateway: {}'s store lists session epoch {}, which is {}'s here: ignored",
                        std::string_view(engine),
                        e,
                        std::string_view(slot_names_[had - 1U]));
        continue;
      }
      acct_.epoch_slot[e].store(tag, std::memory_order_relaxed);
    }
    const std::uint32_t id = ++next_id_;
    auto ap = std::make_unique<Attachment>();
    Attachment& a = *ap;
    a.id = id;
    a.engine = engine;
    a.pid = req.pid;
    a.epoch = *epoch;
    a.owned = std::move(owned);
    for (const Resume& r : resumes)
      a.known.push_back(
          std::make_shared<const std::unordered_set<std::string>>(r.known.begin(), r.known.end()));
    // Closed once the reply is sent (or on failure); the mappings keep the page.
    struct Fd {
      int fd;
      ~Fd() {
        if (fd >= 0) ::close(fd);
      }
    } page_fd{gw::create_wake_page()};
    if (page_fd.fd >= 0) a.page = gw::map_wake_page(page_fd.fd);
    if (a.page == nullptr) {
      const std::string why = std::strerror(errno);
      FASTMM_LOG_ERROR(
          "gateway: cannot create the wake page of attachment {}: {}", id, std::string_view(why));
      send_error(fd, "cannot create the wake page: " + why);
      ::close(fd);
      return;
    }
    a.engine_waker = std::make_unique<Waker>();
    a.engine_waker->share(&a.page->engine.flag);
    const bool strategy_blocks = (req.flags & gw::kStrategyBlocks) != 0;
    const std::string stem = "/dev/shm/fastmm-gw-" + cfg_.engine.name + "-" +
                             std::to_string(::getpid()) + "-" + std::to_string(id) + "-";
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      Attachment::Rings r;
      const std::string base = stem + std::string(slots_[i]->venue->name());
      r.paths = {base + ".md", base + ".ord", base + ".out"};
      auto md = ShmRing::create(r.paths[0], ring_size(cfg_.engine.md_ring_bytes));
      auto order = ShmRing::create(r.paths[1], ring_size(cfg_.engine.order_ring_bytes));
      auto out = ShmRing::create(r.paths[2], ring_size(cfg_.engine.order_ring_bytes));
      if (!md || !order || !out) {
        const std::string why = !md ? md.error() : !order ? order.error() : out.error();
        FASTMM_LOG_ERROR(
            "gateway: cannot create the rings of attachment {}: {}", id, std::string_view(why));
        send_error(fd, "cannot create the rings: " + why);
        ::close(fd);
        remove_rings(a.rings);
        std::error_code ec;
        for (const std::string& p : r.paths) std::filesystem::remove(p, ec);
        gw::unmap_wake_page(a.page);
        return;
      }
      r.md = std::make_unique<ShmRing>(std::move(*md));
      r.order = std::make_unique<ShmRing>(std::move(*order));
      r.outbound = std::make_unique<ShmRing>(std::move(*out));
      a.rings.push_back(std::move(r));
    }
    a.routes = std::make_unique<Route[]>(slots_.size());
    a.md_dropped_logged.assign(slots_.size(), 0);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      Route& r = a.routes[i];
      r.att = &a;
      r.epoch = a.epoch;
      r.md = a.rings[i].md.get();
      r.order = a.rings[i].order.get();
      r.out = a.rings[i].outbound.get();
      r.waker = strategy_blocks ? a.engine_waker.get() : nullptr;
      r.known = a.known[i].get();
      r.slot = *slot;
      for (const InstrumentId inst : a.owned) r.claims.set(inst.value);
    }
    a.slot = *slot;
    for (const InstrumentId inst : a.owned) {
      if (!acct_.shared[inst.value]) owner_[inst.value] = &a;
    }
    // The account's position of an instrument starts with what its first owner restored from its
    // store (flat when it restored nothing, or its venue cannot replay executions and so the
    // strategy starts flat). From then on the account books every execution itself; a later owner
    // finds the account's position, whatever its store says.
    std::vector<InstrumentId> to_seed;
    for (const InstrumentId inst : a.owned) {
      if (!acct_.shared[inst.value] && !seeded_[inst.value]) to_seed.push_back(inst);
    }
    // A shared instrument's account position is every strategy's store's, each added once per
    // gateway run, the first time that strategy trades it; its share starts there.
    std::vector<InstrumentId> to_share;
    for (const InstrumentId inst : a.owned) {
      if (acct_.shared[inst.value] && !share_seeded_[inst.value].test(*slot))
        to_share.push_back(inst);
    }
    // On each network thread, between two of its callbacks: from here on the venue's events reach
    // the strategy's rings, starting with the gateway's books and the account's truth (executions,
    // then the open orders), so the strategy's OMS starts from what the venue holds.
    on_net_threads(slots_, [&](std::size_t i) {
      VenueRouter& v = *routers_[i];
      VenueSlot& s = *slots_[i];
      Route& r = a.routes[i];
      // Where its history starts on this venue, in the venue's clock: the replay start its store
      // gave, else now (the venue's time: the host clock plus the connector's measured offset).
      const Resume& res = resumes[i];
      const bool resume = res.set && executions(i);
      r.replay_from_ms =
          resume ? res.since_ms : wall_now().ns / 1'000'000 + s.venue->status().clock_offset_ms;
      static_cast<void>(v.routes.push_back(&r));
      v.by_slot[r.slot] = &r;
      for (const InstrumentId inst : a.owned) {
        if (instruments_.get(inst).venue == v.vid && !acct_.shared[inst.value])
          v.owner[inst.value] = &r;
      }
      for (const InstrumentId inst : to_share) {
        if (instruments_.get(inst).venue != v.vid) continue;
        Qty q{};
        Price px{};
        if (executions(i)) {
          for (const Seed& sd : seeds) {
            if (sd.inst == inst) {
              q = sd.qty;
              px = sd.avg_px;
            }
          }
        }
        seed_share(v, inst, r.slot, q, px);
        v.share_seeds[inst.value].push_back(
            VenueRouter::ShareSeed{r.slot, r.replay_from_ms, a.known[i]});
      }
      for (const InstrumentId inst : to_seed) {
        if (instruments_.get(inst).venue != v.vid) continue;
        Qty q{};
        Price px{};
        if (executions(i)) {
          for (const Seed& sd : seeds) {
            if (sd.inst == inst) {
              q = sd.qty;
              px = sd.avg_px;
            }
          }
        }
        set_account_position(v, inst, q, px);
        v.seed_from_ms[inst.value] = r.replay_from_ms;
        v.seed_known[inst.value] = a.known[i];
        v.seeded.set(inst.value);
      }
      // Its earlier sessions' orders by the venue's id, then whatever was kept for it: executions
      // a replay found before it attached, of its orders or of the instruments it now owns.
      for (const PastOrder& o : res.orders)
        v.past_orders[{o.inst.value, std::string(o.venue_order_id.view())}] = o.id;
      if (!v.parked.empty()) unpark(v);
      r.snapshot_pending = true;
      // Its books start from the gateway's copies and go on with the events routed after them;
      // the venue is asked for nothing, so no other attachment's books pause.
      r.md_gap = !push_books(v, r);
      if (resume) {
        s.venue->resume_executions(res.since_ms, res.known);
        static_cast<void>(s.venue->request_executions(res.since_ms));
      }
      s.venue->request_open_orders();
    });

    // The reply: header, venues, instruments.
    std::vector<std::byte> out(sizeof(gw::AttachReply) + slots_.size() * sizeof(gw::VenueInfo) +
                               instruments_.size() * sizeof(Instrument));
    gw::AttachReply rep{};
    rep.hdr = gw::Header{gw::kMagic,
                         gw::kVersion,
                         gw::MsgType::AttachReply,
                         static_cast<std::uint32_t>(out.size()),
                         0};
    rep.status = 0;
    rep.attach_id = id;
    rep.venue_count = static_cast<std::uint32_t>(slots_.size());
    rep.instrument_count = static_cast<std::uint32_t>(instruments_.size());
    rep.instrument_bytes = sizeof(Instrument);
    rep.gateway_pid = static_cast<std::uint32_t>(::getpid());
    rep.session_epoch = a.epoch;
    if (cfg_.spin_mode() == SpinMode::Adaptive) rep.flags |= gw::kGatewayBlocks;
    std::memcpy(out.data(), &rep, sizeof rep);
    std::byte* p = out.data() + sizeof rep;
    for (std::size_t i = 0; i < slots_.size(); ++i, p += sizeof(gw::VenueInfo)) {
      gw::VenueInfo vi{};
      vi.id = static_cast<std::uint8_t>(i);
      if (slots_[i]->venue->caps().supports_replace && cfg_.venues[i].supports_replace)
        vi.flags |= gw::kVenueReplace;
      if (executions(i)) vi.flags |= gw::kVenueExecutions;
      to_field(vi.name, slots_[i]->venue->name());
      to_field(vi.md_path, a.rings[i].paths[0]);
      to_field(vi.order_path, a.rings[i].paths[1]);
      to_field(vi.outbound_path, a.rings[i].paths[2]);
      std::memcpy(p, &vi, sizeof vi);
    }
    for (const Instrument& inst : instruments_) {
      std::memcpy(p, &inst, sizeof inst);
      p += sizeof inst;
    }
    a.fd = fd;
    a.since_ns = steady_now().ns;
    a.since_wall_ns = wall_now().ns;
    a.blocks = strategy_blocks;
    atts_.push_back(std::move(ap));
    std::vector<int> fds{page_fd.fd, net_page_fd_};
    for (const auto& s : slots_) fds.push_back(s->reactor->wake_fd());
    if (gw::send_with_fds(fd, out.data(), out.size(), fds) != static_cast<ssize_t>(out.size())) {
      FASTMM_LOG_ERROR("gateway: cannot answer {} (pid {}): {}",
                       std::string_view(engine),
                       req.pid,
                       std::strerror(errno));
      detach(a, "the attach reply could not be sent");
      return;
    }
    std::string symbols;
    for (const InstrumentId inst : a.owned) {
      if (!symbols.empty()) symbols += ",";
      symbols += std::string(instruments_.get(inst).symbol.view());
    }
    for (const InstrumentId inst : to_seed) {
      seeded_[inst.value] = true;
      FASTMM_LOG_INFO("gateway: account position of {} starts at {} (from {})",
                      instruments_.get(inst).symbol,
                      Qty::from_raw(acct_.qty[inst.value].load(std::memory_order_relaxed)),
                      std::string_view(a.who()));
    }
    for (const InstrumentId inst : to_share) {
      share_seeded_[inst.value].set(*slot);
      FASTMM_LOG_INFO("gateway: {}'s share of {} (shared) starts at {}; the account's is {}",
                      std::string_view(a.who()),
                      instruments_.get(inst).symbol,
                      Qty::from_raw(acct_.share[inst.value][*slot].load(std::memory_order_relaxed)),
                      Qty::from_raw(acct_.qty[inst.value].load(std::memory_order_relaxed)));
    }
    for (const InstrumentId inst : a.owned) {
      if (std::find(to_seed.begin(), to_seed.end(), inst) != to_seed.end() ||
          std::find(to_share.begin(), to_share.end(), inst) != to_share.end())
        continue;
      Qty stored{};
      for (const Seed& sd : seeds) {
        if (sd.inst == inst) stored = sd.qty;
      }
      if (acct_.shared[inst.value]) {
        FASTMM_LOG_INFO(
            "gateway: {}'s share of {} (shared) is {}, booked here since it first attached; its "
            "store says {} before its replay",
            std::string_view(a.who()),
            instruments_.get(inst).symbol,
            Qty::from_raw(acct_.share[inst.value][*slot].load(std::memory_order_relaxed)),
            stored);
        continue;
      }
      const Qty q = Qty::from_raw(acct_.qty[inst.value].load(std::memory_order_relaxed));
      FASTMM_LOG_INFO(
          "gateway: account position of {} is {}, booked here since its first owner attached; {}'s "
          "store says {} before its replay",
          instruments_.get(inst).symbol,
          q,
          std::string_view(a.who()),
          stored);
    }
    FASTMM_LOG_WARN(
        "gateway: {} attached: {} venue(s), trades {}, {} strategies attached, reconciling{}",
        std::string_view(a.who()),
        slots_.size(),
        std::string_view(symbols),
        atts_.size(),
        (req.flags & gw::kResumeExecutions) != 0 ? std::string_view(" from its store's last fill")
                                                 : std::string_view());
  }

  [[nodiscard]] bool executions(std::size_t i) const {
    const venues::VenueEntry* e = venues::VenueRegistry::instance().find(cfg_.venues[i].kind);
    return e != nullptr && e->caps.executions;
  }

  const Config& cfg_;
  const GatewayOptions& opts_;
  VenueSlots& slots_;
  const InstrumentTable& instruments_;
  int net_page_fd_;
  std::vector<std::unique_ptr<VenueRouter>> routers_;
  std::vector<std::unique_ptr<Attachment>> atts_;
  std::array<const Attachment*, kMaxInstruments> owner_{};
  std::array<bool, kMaxInstruments> seeded_{};  // the account's position has started
  // [engine] names by slot (Account::share), and per shared instrument the slots whose store has
  // joined the account's position.
  std::vector<std::string> slot_names_;
  std::array<std::bitset<Account::kMaxSlots>, kMaxInstruments> share_seeded_{};
  std::array<std::int64_t, kMaxInstruments> unexplained_logged_{};
  std::array<std::string, kMaxInstruments> shared_logged_;
  std::array<Logged, 8> logged_{};
  std::uint32_t next_id_ = 0;
  Account& acct_;
  std::string kill_path_;
  KillState kill_;  // as loaded, plus this run
  KillState kill_last_;
  bool kill_written_ = false;
  bool kill_write_failed_ = false;
  bool trip_handled_ = false;
  StatusWriter status_;
  std::int64_t started_ns_ = 0;
  std::array<std::int64_t, 5> account_logged_{};
  std::array<std::int64_t, kMaxUnderlyings> und_logged_{};
  bool account_logged_tripped_ = false;
  std::array<std::int64_t, kMaxInstruments> qty_logged_{};
};

}  // namespace

namespace {
std::string_view off_if_empty(const std::string& s) {
  return s.empty() ? std::string_view("off") : std::string_view(s);
}
}  // namespace

std::string default_gateway_path(const Config& cfg) {
  return cfg.engine.journal_dir + "/" + cfg.engine.name + ".gw";
}

std::string_view gateway_control_usage() noexcept {
  return "gateway commands (one per datagram; the reply starts with ok or error)\n"
         "  pull [--instrument SYM | --venue NAME]   the strategies in that scope stop quoting\n"
         "                                           (those trading SYM, every strategy on NAME,\n"
         "                                           or every strategy)\n"
         "  resume [--instrument SYM | --venue NAME] they quote again\n"
         "  kill                                     trip the account kill switch, as max_loss\n"
         "                                           does (GatewayOperator)\n"
         "  clear-kill                               clear it and arm the loss budget again;\n"
         "                                           refused while a strategy is attached\n"
         "  attachments                              one line per attached strategy\n"
         "  status                                   the fastmm-top frame\n"
         "  help                                     this text\n";
}

int run_gateway(const Config& cfg, const GatewayOptions& opts) {
  const char* prog = opts.program.c_str();
  InstrumentTable instruments;
  try {
    instruments = load_instruments(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  if (instruments.size() == 0) {
    std::fprintf(stderr, "%s: no [[instruments]] configured\n", prog);
    return kExitConfig;
  }
  if (cfg.venues.empty() || cfg.venues.size() > 8) {
    std::fprintf(stderr, "%s: a gateway runs 1 to 8 venues\n", prog);
    return kExitConfig;
  }
  if (cfg.gateway.orders_per_sec < 0 || cfg.gateway.burst < 0) {
    std::fprintf(stderr, "%s: [gateway] orders_per_sec and burst cannot be negative\n", prog);
    return kExitConfig;
  }
  // The notional limits: empty is off, otherwise a non-negative decimal.
  const auto limit = [&](const char* key, const std::string& text, std::int64_t* raw) {
    *raw = 0;
    if (text.empty()) return true;
    const auto n = Notional::from_decimal(text);
    if (!n || n->raw < 0) {
      std::fprintf(stderr,
                   "%s: [gateway] %s: '%s' is not a non-negative decimal\n",
                   prog,
                   key,
                   text.c_str());
      return false;
    }
    *raw = n->raw;
    return true;
  };
  auto acct = std::make_unique<Account>();
  std::int64_t open_notional = 0;
  if (!limit("max_open_notional", cfg.gateway.max_open_notional, &open_notional) ||
      !limit("max_loss", cfg.gateway.max_loss, &acct->max_loss) ||
      !limit("max_gross_notional", cfg.gateway.max_gross_notional, &acct->max_gross) ||
      !limit("max_net_notional", cfg.gateway.max_net_notional, &acct->max_net))
    return kExitConfig;
  acct->stale_ns = milliseconds(cfg.risk.stale_md_ms).ns;
  // The account's loss so far and a latched trip, as fastmm-live keeps a strategy's.
  const std::string kill_path =
      cfg.engine.kill_file.empty()
          ? KillStateStore::default_path(cfg.engine.journal_dir, cfg.engine.name)
          : cfg.engine.kill_file;
  if (const std::filesystem::path kp(kill_path); kp.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(kp.parent_path(), ec);
  }
  if (opts.clear_kill) {
    if (auto r = KillStateStore::clear(kill_path); !r) {
      std::fprintf(stderr, "%s: %s\n", prog, r.error().c_str());
      return kExitConfig;
    }
    FASTMM_LOG_WARN("--clear-kill: {} removed; the whole [gateway] max_loss budget is armed again",
                    kill_path);
  }
  KillState kill_state;
  if (acct->max_loss > 0) {
    auto loaded = KillStateStore::load(kill_path);
    if (!loaded) {
      std::fprintf(stderr, "%s: %s\n", prog, loaded.error().c_str());
      return kExitConfig;
    }
    kill_state = *loaded;
    if (kill_state.latched) {
      std::fprintf(stderr,
                   "%s: the account kill switch is latched (%s) in %s (net PnL %.8f over %llu "
                   "run(s)). Check the positions, then clear it with --clear-kill or by removing "
                   "the file; that arms the whole [gateway] max_loss budget again.\n",
                   prog,
                   std::string(to_string(kill_state.reason)).c_str(),
                   kill_path.c_str(),
                   kill_state.carry().to_double(),
                   static_cast<unsigned long long>(kill_state.sessions));
      return kExitKilled;
    }
    ++kill_state.sessions;
    acct->carry.store(kill_state.carry().raw, std::memory_order_relaxed);
  }
  VenueSlots slots;
  venues::VenueFactoryOptions vopts;
  vopts.dry_run = opts.dry_run;
  vopts.busy_poll = cfg.spin_mode() == SpinMode::Busy;
  if (const int rc = make_venue_slots(cfg, vopts, instruments, prog, slots); rc != 0) return rc;
  // [accounting] converts the account's PnL and exposure to one reporting currency, as the engine
  // does; without it they are one currency-less Notional. The venues' reference data is loaded:
  // kInverse is known. The account holds every instrument, so every one must be covered.
  const bool guarded = acct->max_loss > 0 || acct->exposure_limits();
  std::vector<std::string> venue_names;
  for (const auto& s : slots) venue_names.emplace_back(s->venue->name());
  if (cfg.accounting.configured()) {
    std::string warning;
    auto plan = session_fx_plan(
        instruments, cfg.accounting, venue_names, /*all_instruments=*/true, guarded, &warning);
    if (!plan) {
      std::fprintf(stderr,
                   "%s: [accounting]: %s. The [gateway] account limits are in %s.\n",
                   prog,
                   plan.error().c_str(),
                   cfg.accounting.reporting_currency.c_str());
      return kExitConfig;
    }
    if (!warning.empty()) FASTMM_LOG_WARN("gateway: [accounting]: {}", warning);
    acct->fx = *plan;
  }
  if (acct->fx.active()) {
    for (std::size_t c = 1; c < acct->fx.count; ++c) {
      const Instrument& si = instruments.get(acct->fx.sources[c].instrument);
      FASTMM_LOG_INFO("gateway: accounting: {} converts to {} at the mid of {}:{}{}",
                      acct->fx.names[c],
                      acct->fx.reporting(),
                      venue_names[si.venue.value],
                      si.symbol,
                      acct->fx.sources[c].invert ? " (inverted)" : "");
    }
  } else if (guarded) {
    if (const SettlementMix mix = instruments.settlement_mix(); mix.mixed()) {
      const std::string a(mix.first->settlement_ccy());
      const std::string b(mix.other->settlement_ccy());
      std::fprintf(stderr,
                   "%s: instruments settle in different currencies (%s in %s, %s in %s) and the "
                   "[gateway] account limits are one number in one currency. Add\n\n"
                   "  [accounting]\n"
                   "  reporting_currency = \"%s\"\n\n"
                   "  [accounting.fx]\n"
                   "  %s = \"<venue>:<symbol>\"\n\n"
                   "where <symbol> is an [[instruments]] entry whose mid prices %s in %s (or %s in "
                   "%s), one line per settlement currency other than %s; or run one gateway per "
                   "settlement currency; or unset max_loss, max_gross_notional and "
                   "max_net_notional.\n",
                   prog,
                   std::string(mix.first->symbol.view()).c_str(),
                   a.c_str(),
                   std::string(mix.other->symbol.view()).c_str(),
                   b.c_str(),
                   a.c_str(),
                   b.c_str(),
                   b.c_str(),
                   a.c_str(),
                   a.c_str(),
                   b.c_str(),
                   a.c_str());
      return kExitConfig;
    }
  }
  // [gateway.underlying]: the account's net position per base asset, over every instrument of the
  // gateway (multipliers and kInverse come from the venues' reference data, loaded by now).
  if (cfg.gateway.underlying.configured()) {
    auto plan = build_underlying_plan(instruments, cfg.gateway.underlying, "gateway");
    if (!plan) {
      std::fprintf(stderr, "%s: %s\n", prog, plan.error().c_str());
      return kExitConfig;
    }
    acct->und = *plan;
    for (std::size_t u = 0; u < acct->und.count; ++u) {
      std::string members;
      for (const InstrumentId id : acct->und.instruments(u)) {
        const Instrument& i = instruments.get(id);
        if (!members.empty()) members += ", ";
        members += venue_names[i.venue.value] + ":" + std::string(i.symbol.view());
        if (i.inverse()) members += " (inverse)";
      }
      FASTMM_LOG_INFO("gateway: underlying {} max_net={} over {}",
                      acct->und.names[u],
                      acct->und.max_net[u],
                      members);
    }
  }
  venues::SymbolTable symbols;
  if (!symbols.build(instruments)) {
    std::fprintf(stderr, "%s: duplicate or empty instrument symbols\n", prog);
    return kExitConfig;
  }
  TscCalibrator tsc_calibrator;
  const Seqlocked<TscCalibration> tsc_pub(tsc_calibrator.start());
  const net::ReactorBackend net_backend = resolve_net_backend(cfg);
  for (std::size_t i = 0; i < slots.size(); ++i)
    wire_venue_slot(*slots[i],
                    VenueId{static_cast<std::uint8_t>(i)},
                    cfg,
                    net_backend,
                    symbols,
                    instruments,
                    &tsc_pub);

  // The network threads' sleeping flags, in a page every attachment maps.
  const int net_page_fd = gw::create_wake_page();
  gw::WakePage* const net_page = net_page_fd >= 0 ? gw::map_wake_page(net_page_fd) : nullptr;
  if (net_page == nullptr) {
    std::fprintf(stderr, "%s: cannot create the wake page: %s\n", prog, std::strerror(errno));
    return kExitConfig;
  }
  struct PageGuard {
    int fd;
    gw::WakePage* page;
    ~PageGuard() {
      gw::unmap_wake_page(page);
      ::close(fd);
    }
  } page_guard{net_page_fd, net_page};

  const std::string path = opts.socket_path.empty() ? default_gateway_path(cfg) : opts.socket_path;
  std::string err;
  const int listen_fd = listen_seqpacket(path, &err);
  if (listen_fd < 0) {
    std::fprintf(stderr, "%s: gateway socket %s: %s\n", prog, path.c_str(), err.c_str());
    return kExitConfig;
  }

  acct->venue_count = slots.size();
  // [gateway.shared]: the instruments several strategies may trade at once, with their primaries.
  std::vector<std::pair<InstrumentId, std::string>> primaries;
  for (const auto& [where, primary] : cfg.gateway.shared) {
    const std::size_t colon = where.find(':');
    const VenueId vid = cfg.venue_id(where.substr(0, colon));
    const Instrument* inst = vid.valid() ? instruments.find(vid, where.substr(colon + 1)) : nullptr;
    if (inst == nullptr) {
      std::fprintf(stderr,
                   "%s: [gateway.shared.\"%s\"]: not an instrument of the gateway\n",
                   prog,
                   where.c_str());
      return kExitConfig;
    }
    acct->shared[inst->id.value] = true;
    primaries.emplace_back(inst->id, primary);
    FASTMM_LOG_INFO("gateway: {} is shared: several strategies may trade it; primary {}",
                    where,
                    primary.empty()
                        ? std::string_view("none (the account books what names no order)")
                        : std::string_view(primary));
  }
  Gateway gateway(cfg,
                  opts,
                  slots,
                  instruments,
                  net_page,
                  net_page_fd,
                  *acct,
                  kill_path,
                  kill_state,
                  primaries);
  gateway.persist_kill();
  // fastmm-top reads the status segment; fastmm-ctl --gateway talks to the control socket.
  if (!opts.no_status) {
    const std::string status_path =
        opts.status_path.empty() ? default_gateway_status_path(cfg.engine.name) : opts.status_path;
    std::string status_error;
    if (gateway.open_status(status_path, &status_error)) {
      FASTMM_LOG_INFO("status: {} (watch with fastmm-top --path {})", status_path, status_path);
    } else {
      FASTMM_LOG_WARN("status file {} unavailable: {}", status_path, status_error);
    }
  }
  gateway.publish_status(StatusRunState::Starting);
  ControlSocket control;
  if (!opts.no_control) {
    const std::string ctl_path = opts.control_path.empty() ? path + ".ctl" : opts.control_path;
    std::string ctl_error;
    if (control.open(ctl_path, &ctl_error)) {
      FASTMM_LOG_INFO("control socket: {} (fastmm-ctl --path {} status)", ctl_path, ctl_path);
    } else {
      FASTMM_LOG_WARN("control socket {} unavailable: {}", ctl_path, ctl_error);
    }
  }
  const ControlSocket::Handler on_command = [&gateway](std::string_view request) {
    return gateway.command(request);
  };
  const GatewaySignals signals;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const int cpu = i < cfg.engine.net_cpus.size() ? cfg.engine.net_cpus[i] : -1;
    const bool shared = net_cpu_shared(-1, cfg.engine.net_cpus, i);
    slots[i]->thread = std::thread(net_loop, std::ref(*slots[i]), cpu, i, cfg.spin_mode(), shared);
  }
  FASTMM_LOG_INFO(
      "fastmm-gateway: {} venue(s), {} instrument(s), dry_run={}, net={}, orders_per_sec={}, "
      "max_open_notional={}, max_loss={} (carried {}), max_gross_notional={}, "
      "max_net_notional={}; attach with fastmm-live --gateway {}",
      slots.size(),
      instruments.size(),
      opts.dry_run,
      net::to_string(net_backend),
      cfg.gateway.orders_per_sec,
      off_if_empty(cfg.gateway.max_open_notional),
      off_if_empty(cfg.gateway.max_loss),
      Notional::from_raw(acct->carry.load(std::memory_order_relaxed)),
      off_if_empty(cfg.gateway.max_gross_notional),
      off_if_empty(cfg.gateway.max_net_notional),
      path);

  gateway.log_account(/*force=*/true);
  gateway.publish_status(StatusRunState::Running);
  const std::int64_t start = steady_now().ns;
  std::int64_t next_tick = start + 1'000'000'000;
  std::int64_t next_status = start + 250'000'000;
  std::vector<pollfd> fds;
  while (g_gw_signal == 0) {
    const std::int64_t now = steady_now().ns;
    if (opts.duration_ns > 0 && now - start >= opts.duration_ns) break;
    fds.clear();
    fds.push_back(pollfd{listen_fd, POLLIN, 0});
    gateway.poll_fds(fds);
    // Wakes at once when a strategy's connection closes; otherwise at least every 50 ms for the
    // signals, the overflow check and the once-a-second housekeeping.
    const int pr = ::poll(fds.data(), fds.size(), 50);
    if (pr > 0) {
      // Back to front: a detach removes its attachment, and the others keep their index.
      for (std::size_t k = fds.size() - 1; k >= 1; --k) {
        if (fds[k].revents != 0) gateway.on_readable(k - 1);
      }
      if ((fds[0].revents & POLLIN) != 0) {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) gateway.on_connection(fd);
      }
    }
    gateway.check_overflows();
    gateway.check_trip();
    control.poll(on_command);
    if (steady_now().ns >= next_status) {
      next_status = steady_now().ns + 250'000'000;
      gateway.publish_status(StatusRunState::Running);
    }
    if (steady_now().ns >= next_tick) {
      next_tick += 1'000'000'000;
      for (auto& s : slots) {
        venues::Venue* v = s->venue.get();
        s->reactor->post([v] { v->on_timer(net::Reactor::now_ns()); });
        log_venue_status(*v);
      }
      gateway.log_counters();
      gateway.log_account();
      gateway.persist_kill();
      // The once-a-second lines are the gateway's status: on disk now, not when a buffer fills.
      Logger::instance().flush();
    }
  }
  FASTMM_LOG_WARN("fastmm-gateway: shutting down ({})",
                  g_gw_signal != 0 ? std::string_view("signal") : std::string_view("duration"));
  ::close(listen_fd);
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  control.close();
  gateway.publish_status(StatusRunState::Stopping);
  gateway.detach_all("the gateway is shutting down");
  bool cancel_ok = true;
  if (!opts.dry_run) {
    for (auto& s : slots) cancel_ok = s->venue->cancel_all() && cancel_ok;
  }
  for (auto& s : slots) {
    s->stop.store(true);
    s->reactor->wake();
  }
  for (auto& s : slots) {
    if (s->thread.joinable()) s->thread.join();
  }
  gateway.log_counters();
  gateway.check_trip();
  gateway.log_account(/*force=*/true);
  gateway.persist_kill();
  gateway.publish_status(StatusRunState::Stopped);
  for (auto& s : slots) {
    const venues::VenueStatus st = s->venue->status();
    FASTMM_LOG_INFO("[{}] final: md_msgs={} orders={} cancels={} order_events={} reconnects={}",
                    s->venue->name(),
                    st.md_messages,
                    st.orders_sent,
                    st.cancels_sent,
                    st.order_events,
                    st.reconnects);
    log_wire_latency(s->venue->name(), st, true);
  }
  FASTMM_LOG_INFO("fastmm-gateway: cancel_all {}", cancel_ok ? "ok" : "FAILED");
  return cancel_ok ? kExitOk : kExitRuntime;
}

}  // namespace fastmm::live
