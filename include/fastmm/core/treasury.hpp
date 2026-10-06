#pragma once
// Pool treasury: keeps one asset spread over the accounts of an account pool
// (core/account_pool.hpp) by internal transfers, so an account that runs short of it is topped up
// from one that holds more than it needs ([venues.<primary>.treasury]).
//
//   plan_transfers()  the plan: pure, deterministic, from each account's free balance
//   TreasuryLimiter   how often transfers may be sent: an interval, an hourly count, a cool-down
//   TreasuryLedger    the transfers not yet resolved, the id counter and the limiter, in a text
//                     file written atomically after every change
//   Treasury          the driver: resolves what is in flight, then plans and sends one transfer
//                     through a TreasuryPort (the connector, the journal, a balance refresh)
//
// The plan. Each account has a target: the pool's total free balance shared by `weight` (equal by
// default). An account is short when its free balance is below its floor, max(target * (1 -
// threshold), min_free); it then asks for max(target, min_free) less what it has. An account gives
// what it holds above max(target, min_free), and no more than it can transfer (a venue that says
// what may leave the account: Binance USD-M maxWithdrawAmount). The largest need is matched with
// the largest giver first (ties: the account listed first), each transfer at most max_amount,
// rounded down to `step`, and none below min_amount. Any account whose balance is not known stops
// the plan.
//
// One transfer at a time per pool: while a transfer is unresolved, or done and not yet visible in
// both accounts' balance reports (a report stamped at or after the transfer was seen done, or
// settle_ns), nothing is planned. A free balance is the engine's estimate (what the venue reported
// less what the engine's orders hold since), so a transfer never takes what an order holds.
//
// Not on any hot path: the driver runs on its own thread, calls the connector's blocking transfer
// requests and allocates.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/transfer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

struct TreasuryConfig {
  static constexpr std::size_t kMax = PoolMembers::kMax;

  bool enabled = false;
  bool dry_run = false;  // log and journal the plan, send nothing
  std::string asset;
  // The pool's accounts, the primary first (PoolPlan::members), and their names for the log and
  // the ledger. weight[i], min_free[i] belong to member[i].
  PoolMembers members;
  std::array<std::string, kMax> names;
  std::array<double, kMax> weight{1, 1, 1, 1, 1, 1, 1, 1};
  std::array<Notional, kMax> min_free{};
  double threshold = 0.2;                         // short below target * (1 - threshold)
  Notional min_amount;                            // smaller transfers are not made
  Notional max_amount;                            // per transfer; zero: no cap
  Notional step = Notional::from_raw(1'000'000);  // amounts round down to it (0.01)
  std::int64_t interval_ns = 10'000'000'000;      // how often the driver looks
  std::int64_t min_interval_ns = 60'000'000'000;  // between two transfers sent
  std::uint32_t max_per_hour = 12;                // transfers sent in any hour
  std::int64_t cooldown_ns = 300'000'000'000;     // after a failure or a timeout
  std::int64_t timeout_ns = 60'000'000'000;       // an unresolved transfer this old timed out
  std::int64_t settle_ns = 30'000'000'000;        // longest wait for the balances after a transfer
  std::string state_file;                         // TreasuryLedger's file; empty: kept in memory

  [[nodiscard]] std::size_t index_of(VenueId v) const noexcept {
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (members[i] == v) return i;
    }
    return kMax;
  }
};

// One account as the plan sees it.
struct TreasuryAccount {
  VenueId venue;
  Notional free;
  bool known = false;  // the venue has reported the asset on this account
  // What can leave the account by a transfer (BalanceMsg::kWithdrawable, Binance USD-M
  // maxWithdrawAmount); a negative value: `free` can.
  Notional transferable = Notional::from_raw(-1);
};

struct PlannedTransfer {
  VenueId from;
  VenueId to;
  Notional amount;
  constexpr bool operator==(const PlannedTransfer&) const noexcept = default;
};

struct TreasuryPlan {
  enum class Status : std::uint8_t { Balanced, Transfers, Unknown, Empty };
  Status status = Status::Balanced;  // Unknown: an account's balance is not known
  std::array<PlannedTransfer, TreasuryConfig::kMax> transfers{};
  std::uint8_t count = 0;
  [[nodiscard]] std::span<const PlannedTransfer> view() const noexcept {
    return {transfers.data(), count};
  }
};

// `accounts` in the order of cfg.members (an account cfg.members does not list is ignored).
[[nodiscard]] TreasuryPlan plan_transfers(const TreasuryConfig& cfg,
                                          std::span<const TreasuryAccount> accounts) noexcept;

// When the next transfer may be sent. Times are wall-clock ns.
struct TreasuryLimiter {
  std::int64_t last_sent_ns = 0;
  std::int64_t cooldown_until_ns = 0;
  std::deque<std::int64_t> sent;  // send times within the last hour, oldest first

  enum class Verdict : std::uint8_t { Ok, Interval, Hourly, Cooldown };
  [[nodiscard]] Verdict check(const TreasuryConfig& cfg, std::int64_t now_ns);
  void on_sent(std::int64_t now_ns) {
    last_sent_ns = now_ns;
    sent.push_back(now_ns);
  }
  void cool_down(const TreasuryConfig& cfg, std::int64_t now_ns) noexcept {
    cooldown_until_ns = now_ns + cfg.cooldown_ns;
  }
};
[[nodiscard]] std::string_view to_string(TreasuryLimiter::Verdict v) noexcept;

// A transfer the treasury has created and not yet seen resolved.
struct TreasuryTransfer {
  enum class Phase : std::uint8_t {
    Created = 0,    // written to the ledger, not yet answered by the venue
    Submitted = 1,  // the venue took it (or answered Pending)
    Unknown = 2,    // the request got no usable answer: ask for its state
  };
  TransferRequest req;
  Phase phase = Phase::Created;
  std::int64_t created_ns = 0;
  std::int64_t next_poll_ns = 0;
  bool timed_out = false;
};
[[nodiscard]] std::string_view to_string(TreasuryTransfer::Phase p) noexcept;

// The durable part of a treasury: the id counter, the limiter and the transfers in flight. Text,
// one record per line; save() goes through write_file_atomic.
struct TreasuryLedger {
  std::uint64_t seq = 0;
  TreasuryLimiter limiter;
  std::vector<TreasuryTransfer> in_flight;

  // `names` resolves account names to venue ids (cfg.names / cfg.members). A record naming an
  // account the configuration no longer has is an error: its transfer would be lost from view.
  [[nodiscard]] std::string serialize(const TreasuryConfig& cfg) const;
  [[nodiscard]] static Result<TreasuryLedger, std::string> parse(const TreasuryConfig& cfg,
                                                                 std::string_view text);
  // An absent file is an empty ledger.
  [[nodiscard]] static Result<TreasuryLedger, std::string> load(const TreasuryConfig& cfg);
  [[nodiscard]] Result<void, std::string> save(const TreasuryConfig& cfg) const;
};

// What the treasury reports through TreasuryPort::record (journal) and its counters.
enum class TreasuryEventKind : std::uint8_t {
  Planned = 0,  // dry run: what would have been sent
  Sent = 1,     // handed to the venue
  Done = 2,
  Failed = 3,
  TimedOut = 4,  // unresolved past timeout_ns; still asked for
};
[[nodiscard]] std::string_view to_string(TreasuryEventKind k) noexcept;

struct TreasuryEvent {
  TreasuryEventKind kind = TreasuryEventKind::Planned;
  const TransferRequest* req = nullptr;
  const TransferResult* result = nullptr;  // the venue's answer, when there is one
};

struct TreasuryStats {
  std::uint64_t plans = 0;          // plans that had a transfer
  std::uint64_t dry_run_plans = 0;  // of them, not sent (dry_run)
  std::uint64_t sent = 0;
  std::uint64_t done = 0;
  std::uint64_t failed = 0;
  std::uint64_t timed_out = 0;
  std::uint64_t limited = 0;  // plans held back by the limiter
  std::uint64_t errors = 0;   // ledger writes that failed, venue requests without an answer
  std::uint32_t in_flight = 0;
  Notional moved;  // done transfers' amounts
};

// The connector side of a treasury. submit() and status() are the venue's blocking requests
// (Venue::transfer, Venue::transfer_status).
class TreasuryPort {
 public:
  virtual ~TreasuryPort() = default;
  virtual TransferResult submit(const TransferRequest& req) = 0;
  virtual TransferResult status(const TransferRequest& req) = 0;
  // Ask the venue for `account`'s balances now (Venue::request_balances).
  virtual void refresh(VenueId /*account*/) {}
  virtual void record(const TreasuryEvent& /*e*/) {}
};

// One balance row as the driver reads it: the engine's estimate of `asset` on `venue`.
struct TreasuryBalance {
  VenueId venue;
  FixedString<8> asset;
  Notional free;
  bool known = false;
  std::int64_t as_of_ns = 0;                       // the venue's time of the last report
  Notional transferable = Notional::from_raw(-1);  // TreasuryAccount::transferable
};

class Treasury {
 public:
  explicit Treasury(TreasuryConfig cfg) : cfg_(std::move(cfg)) {}

  // Loads the ledger (an unresolved transfer of an earlier process is asked for before anything
  // new is planned). An error: the file cannot be read or names unknown accounts.
  [[nodiscard]] Result<void, std::string> open();
  // One look, at wall time `now_ns`: resolves the transfers in flight, else plans and sends at
  // most one. Blocking (the port's requests); returns quickly when it is not yet time to look.
  void step(std::int64_t now_ns, std::span<const TreasuryBalance> balances, TreasuryPort& port);

  [[nodiscard]] const TreasuryConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const TreasuryStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const TreasuryLedger& ledger() const noexcept { return ledger_; }
  // The next client id this treasury would give a transfer created at `now_ns`.
  [[nodiscard]] std::string client_id(std::int64_t now_ns, std::uint64_t seq) const;

 private:
  void resolve(std::int64_t now_ns, TreasuryPort& port);
  void finish(std::size_t i,
              TransferState state,
              const TransferResult& r,
              std::int64_t now_ns,
              TreasuryPort& port);
  [[nodiscard]] bool settled(std::int64_t now_ns, std::span<const TreasuryBalance> balances) const;
  void plan_and_send(std::int64_t now_ns,
                     std::span<const TreasuryBalance> balances,
                     TreasuryPort& port);
  void persist();

  TreasuryConfig cfg_;
  TreasuryLedger ledger_;
  TreasuryStats stats_;
  std::int64_t next_look_ns_ = 0;
  std::int64_t last_dry_plan_ns_ = 0;
  std::optional<PlannedTransfer> last_dry_plan_;
  // The last transfer seen done: planning waits for both accounts' reports after `settle_from_ns`.
  VenueId settle_from_;
  VenueId settle_to_;
  std::int64_t settle_from_ns_ = 0;
  bool settling_ = false;
};

}  // namespace fastmm
