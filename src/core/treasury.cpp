#include "fastmm/core/treasury.hpp"

#include "fastmm/core/fx.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/session_state.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace fastmm {

namespace {

constexpr std::int64_t kHourNs = 3'600'000'000'000;
constexpr std::string_view kLedgerMagic = "fastmm-treasury 1";

std::string dec(Notional n) {
  char buf[kMaxDecimalChars];
  return {buf, n.to_decimal(buf)};
}

std::int64_t round_down(std::int64_t raw, std::int64_t step) noexcept {
  if (step <= 1) return raw;
  return raw - raw % step;
}

}  // namespace

// ---- plan ---------------------------------------------------------------------------------------

TreasuryPlan plan_transfers(const TreasuryConfig& cfg,
                            std::span<const TreasuryAccount> accounts) noexcept {
  TreasuryPlan plan;
  const std::size_t n = cfg.members.size();
  if (n < 2) {
    plan.status = TreasuryPlan::Status::Empty;
    return plan;
  }
  std::array<std::int64_t, TreasuryConfig::kMax> free{};
  std::array<bool, TreasuryConfig::kMax> seen{};
  for (const TreasuryAccount& a : accounts) {
    const std::size_t i = cfg.index_of(a.venue);
    if (i >= n) continue;
    if (!a.known) {
      plan.status = TreasuryPlan::Status::Unknown;
      return plan;
    }
    free[i] = a.free.raw;
    seen[i] = true;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!seen[i]) {
      plan.status = TreasuryPlan::Status::Unknown;
      return plan;
    }
  }
  // Targets: the total shared by weight. A negative free balance (a derivative account in loss)
  // counts as it is: it lowers the total and is the first to be topped up.
  std::int64_t total = 0;
  double wsum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    total += free[i];
    wsum += std::max(cfg.weight[i], 0.0);
  }
  if (wsum <= 0) {
    plan.status = TreasuryPlan::Status::Empty;
    return plan;
  }
  std::array<std::int64_t, TreasuryConfig::kMax> need{};  // > 0: short by; < 0: can give
  for (std::size_t i = 0; i < n; ++i) {
    const double share = std::max(cfg.weight[i], 0.0) / wsum;
    const auto target = static_cast<std::int64_t>(std::floor(static_cast<double>(total) * share));
    const std::int64_t keep = std::max(target, cfg.min_free[i].raw);
    const auto floor_raw = static_cast<std::int64_t>(
        std::floor(static_cast<double>(target) * (1.0 - std::clamp(cfg.threshold, 0.0, 1.0))));
    const std::int64_t floor = std::max(floor_raw, cfg.min_free[i].raw);
    if (free[i] < floor) {
      need[i] = keep - free[i];
    } else if (free[i] > keep) {
      need[i] = -(free[i] - keep);
    }
  }
  // Largest need first, from the largest giver first; ties keep the configuration's order.
  std::array<std::size_t, TreasuryConfig::kMax> order{};
  for (std::size_t i = 0; i < n; ++i) order[i] = i;
  std::stable_sort(order.begin(),
                   order.begin() + static_cast<std::ptrdiff_t>(n),
                   [&](std::size_t a, std::size_t b) { return need[a] > need[b]; });
  std::array<std::size_t, TreasuryConfig::kMax> givers{};
  for (std::size_t i = 0; i < n; ++i) givers[i] = i;
  std::stable_sort(givers.begin(),
                   givers.begin() + static_cast<std::ptrdiff_t>(n),
                   [&](std::size_t a, std::size_t b) { return need[a] < need[b]; });
  const std::int64_t step = cfg.step.raw;
  for (std::size_t oi = 0; oi < n; ++oi) {
    const std::size_t to = order[oi];
    if (need[to] <= 0) break;
    for (std::size_t gi = 0; gi < n && need[to] > 0; ++gi) {
      const std::size_t from = givers[gi];
      if (from == to || need[from] >= 0) continue;
      std::int64_t amount = std::min(need[to], -need[from]);
      if (cfg.max_amount.raw > 0) amount = std::min(amount, cfg.max_amount.raw);
      amount = round_down(amount, step);
      if (amount <= 0 || amount < cfg.min_amount.raw) continue;
      if (plan.count == plan.transfers.size()) break;
      plan.transfers[plan.count++] =
          PlannedTransfer{cfg.members[from], cfg.members[to], Notional::from_raw(amount)};
      need[to] -= amount;
      need[from] += amount;
    }
  }
  plan.status = plan.count > 0 ? TreasuryPlan::Status::Transfers : TreasuryPlan::Status::Balanced;
  return plan;
}

// ---- limiter ------------------------------------------------------------------------------------

TreasuryLimiter::Verdict TreasuryLimiter::check(const TreasuryConfig& cfg, std::int64_t now_ns) {
  while (!sent.empty() && sent.front() <= now_ns - kHourNs) sent.pop_front();
  if (now_ns < cooldown_until_ns) return Verdict::Cooldown;
  if (last_sent_ns != 0 && now_ns - last_sent_ns < cfg.min_interval_ns) return Verdict::Interval;
  if (cfg.max_per_hour != 0 && sent.size() >= cfg.max_per_hour) return Verdict::Hourly;
  return Verdict::Ok;
}

std::string_view to_string(TreasuryLimiter::Verdict v) noexcept {
  switch (v) {
    case TreasuryLimiter::Verdict::Ok:
      return "ok";
    case TreasuryLimiter::Verdict::Interval:
      return "min_interval_s";
    case TreasuryLimiter::Verdict::Hourly:
      return "max_per_hour";
    case TreasuryLimiter::Verdict::Cooldown:
      return "cooldown_s";
  }
  return "?";
}

std::string_view to_string(TreasuryTransfer::Phase p) noexcept {
  switch (p) {
    case TreasuryTransfer::Phase::Created:
      return "created";
    case TreasuryTransfer::Phase::Submitted:
      return "submitted";
    case TreasuryTransfer::Phase::Unknown:
      return "unknown";
  }
  return "?";
}

std::string_view to_string(TreasuryEventKind k) noexcept {
  switch (k) {
    case TreasuryEventKind::Planned:
      return "planned";
    case TreasuryEventKind::Sent:
      return "sent";
    case TreasuryEventKind::Done:
      return "done";
    case TreasuryEventKind::Failed:
      return "failed";
    case TreasuryEventKind::TimedOut:
      return "timed_out";
  }
  return "?";
}

// ---- ledger -------------------------------------------------------------------------------------
//
//   fastmm-treasury 1
//   seq <n>
//   last_sent <ns>
//   cooldown_until <ns>
//   sent <ns>                                     one per transfer sent in the last hour
//   transfer <id> <phase> <from> <to> <asset> <amount> <created_ns> <created_ms>
//
// Accounts by their [venues.<name>] name, so a reordered configuration still reads the file.

std::string TreasuryLedger::serialize(const TreasuryConfig& cfg) const {
  std::string out;
  out += kLedgerMagic;
  out += '\n';
  out += "seq " + std::to_string(seq) + '\n';
  out += "last_sent " + std::to_string(limiter.last_sent_ns) + '\n';
  out += "cooldown_until " + std::to_string(limiter.cooldown_until_ns) + '\n';
  for (const std::int64_t t : limiter.sent) out += "sent " + std::to_string(t) + '\n';
  for (const TreasuryTransfer& t : in_flight) {
    const std::size_t f = cfg.index_of(t.req.from);
    const std::size_t d = cfg.index_of(t.req.to);
    out += "transfer " + t.req.client_id + ' ' + std::string(to_string(t.phase)) + ' ' +
           (f < cfg.names.size() ? cfg.names[f] : std::string("?")) + ' ' +
           (d < cfg.names.size() ? cfg.names[d] : std::string("?")) + ' ' + t.req.asset + ' ' +
           dec(t.req.amount) + ' ' + std::to_string(t.created_ns) + ' ' +
           std::to_string(t.req.created_ms) + '\n';
  }
  return out;
}

namespace {

std::vector<std::string_view> words(std::string_view line) {
  std::vector<std::string_view> w;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    const std::size_t s = i;
    while (i < line.size() && line[i] != ' ') ++i;
    if (i > s) w.push_back(line.substr(s, i - s));
  }
  return w;
}

template <class T>
bool number(std::string_view s, T& out) {
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

}  // namespace

Result<TreasuryLedger, std::string> TreasuryLedger::parse(const TreasuryConfig& cfg,
                                                          std::string_view text) {
  TreasuryLedger l;
  std::size_t line_no = 0;
  bool header = false;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    ++line_no;
    if (line.empty()) continue;
    const auto bad = [&](std::string_view why) {
      return fail(std::string("treasury ledger line ") + std::to_string(line_no) + ": " +
                  std::string(why));
    };
    if (!header) {
      if (line != kLedgerMagic) return bad("not a treasury ledger");
      header = true;
      continue;
    }
    const std::vector<std::string_view> w = words(line);
    if (w.empty()) continue;
    if (w[0] == "seq" && w.size() == 2) {
      if (!number(w[1], l.seq)) return bad("bad seq");
    } else if (w[0] == "last_sent" && w.size() == 2) {
      if (!number(w[1], l.limiter.last_sent_ns)) return bad("bad last_sent");
    } else if (w[0] == "cooldown_until" && w.size() == 2) {
      if (!number(w[1], l.limiter.cooldown_until_ns)) return bad("bad cooldown_until");
    } else if (w[0] == "sent" && w.size() == 2) {
      std::int64_t t = 0;
      if (!number(w[1], t)) return bad("bad sent");
      l.limiter.sent.push_back(t);
    } else if (w[0] == "transfer" && w.size() == 9) {
      TreasuryTransfer t;
      t.req.client_id = std::string(w[1]);
      if (w[2] == "created") {
        t.phase = TreasuryTransfer::Phase::Created;
      } else if (w[2] == "submitted") {
        t.phase = TreasuryTransfer::Phase::Submitted;
      } else if (w[2] == "unknown") {
        t.phase = TreasuryTransfer::Phase::Unknown;
      } else {
        return bad("bad phase");
      }
      const auto account = [&](std::string_view name) {
        for (std::size_t i = 0; i < cfg.members.size(); ++i) {
          if (cfg.names[i] == name) return cfg.members[i];
        }
        return VenueId::invalid();
      };
      t.req.from = account(w[3]);
      t.req.to = account(w[4]);
      if (!t.req.from.valid() || !t.req.to.valid()) {
        return bad("transfer " + std::string(w[1]) + " names an account that is not in the pool (" +
                   std::string(w[3]) + " -> " + std::string(w[4]) +
                   "); resolve it with the venue and remove the line");
      }
      t.req.asset = std::string(w[5]);
      const std::optional<Notional> amount = Notional::parse(w[6]);
      if (!amount) return bad("bad amount");
      t.req.amount = *amount;
      if (!number(w[7], t.created_ns) || !number(w[8], t.req.created_ms)) return bad("bad time");
      l.in_flight.push_back(std::move(t));
    } else {
      return bad("unknown record");
    }
  }
  return l;
}

Result<TreasuryLedger, std::string> TreasuryLedger::load(const TreasuryConfig& cfg) {
  if (cfg.state_file.empty()) return TreasuryLedger{};
  std::string text;
  const Result<bool, std::string> r = load_strategy_state(cfg.state_file, text);
  if (!r) return fail(r.error());
  if (!*r) return TreasuryLedger{};
  auto l = parse(cfg, text);
  if (!l) return fail(cfg.state_file + ": " + l.error());
  return l;
}

Result<void, std::string> TreasuryLedger::save(const TreasuryConfig& cfg) const {
  if (cfg.state_file.empty()) return {};
  return write_file_atomic(cfg.state_file, serialize(cfg));
}

// ---- driver -------------------------------------------------------------------------------------

Result<void, std::string> Treasury::open() {
  auto l = TreasuryLedger::load(cfg_);
  if (!l) return fail(l.error());
  ledger_ = std::move(*l);
  stats_.in_flight = static_cast<std::uint32_t>(ledger_.in_flight.size());
  for (const TreasuryTransfer& t : ledger_.in_flight) {
    FASTMM_LOG_WARN("[treasury {}] transfer {} of an earlier session is {}: asking the venue",
                    cfg_.names[0],
                    t.req.client_id,
                    to_string(t.phase));
  }
  return {};
}

std::string Treasury::client_id(std::int64_t now_ns, std::uint64_t seq) const {
  // "fm" + the primary's venue id + the creation time in ms (base 36, 9 digits) + the sequence
  // number (base 36): unique per pool across restarts (the ledger keeps `seq`), alphanumeric.
  const auto b36 = [](std::uint64_t v, std::size_t width) {
    std::string s;
    do {
      const auto d = static_cast<char>(v % 36);
      s.insert(s.begin(), d < 10 ? static_cast<char>('0' + d) : static_cast<char>('a' + d - 10));
      v /= 36;
    } while (v != 0);
    while (s.size() < width) s.insert(s.begin(), '0');
    return s;
  };
  const std::uint64_t ms = now_ns > 0 ? static_cast<std::uint64_t>(now_ns / 1'000'000) : 0;
  return "fm" + b36(cfg_.members.empty() ? 0 : cfg_.members[0].value, 1) + b36(ms, 9) + b36(seq, 1);
}

void Treasury::persist() {
  if (auto r = ledger_.save(cfg_); !r) {
    ++stats_.errors;
    FASTMM_LOG_ERROR(
        "[treasury {}] cannot write {}: {}", cfg_.names[0], cfg_.state_file, r.error());
  }
  stats_.in_flight = static_cast<std::uint32_t>(ledger_.in_flight.size());
}

void Treasury::step(std::int64_t now_ns,
                    std::span<const TreasuryBalance> balances,
                    TreasuryPort& port) {
  if (!cfg_.enabled || now_ns < next_look_ns_) return;
  next_look_ns_ = now_ns + cfg_.interval_ns;
  if (!ledger_.in_flight.empty()) {
    resolve(now_ns, port);
    if (!ledger_.in_flight.empty()) return;
  }
  if (settling_ && !settled(now_ns, balances)) return;
  settling_ = false;
  plan_and_send(now_ns, balances, port);
}

void Treasury::resolve(std::int64_t now_ns, TreasuryPort& port) {
  for (std::size_t i = 0; i < ledger_.in_flight.size();) {
    TreasuryTransfer& t = ledger_.in_flight[i];
    if (now_ns < t.next_poll_ns) {
      ++i;
      continue;
    }
    t.next_poll_ns = now_ns + cfg_.interval_ns;
    const TransferResult r = port.status(t.req);
    const std::int64_t age = now_ns - t.created_ns;
    switch (r.state) {
      case TransferState::Done:
      case TransferState::Failed:
        finish(i, r.state, r, now_ns, port);
        continue;
      case TransferState::NotFound:
        // Nothing arrived under this id: after the timeout it never will (a request lost before
        // the venue read it, or a process that stopped between the ledger and the request).
        if (age >= cfg_.timeout_ns) {
          TransferResult nf = r;
          if (nf.detail.empty()) nf.detail = "the venue has no transfer under this id";
          finish(i, TransferState::Failed, nf, now_ns, port);
          continue;
        }
        break;
      case TransferState::Unknown:
        ++stats_.errors;
        [[fallthrough]];
      case TransferState::Pending:
        if (r.state == TransferState::Pending && t.phase != TreasuryTransfer::Phase::Submitted) {
          t.phase = TreasuryTransfer::Phase::Submitted;
          persist();
        }
        break;
    }
    if (age >= cfg_.timeout_ns && !t.timed_out) {
      t.timed_out = true;
      ++stats_.timed_out;
      ledger_.limiter.cool_down(cfg_, now_ns);
      persist();
      FASTMM_LOG_WARN(
          "[treasury {}] transfer {} unresolved after {} s ({}{}{}): nothing new is planned until "
          "the venue resolves it",
          cfg_.names[0],
          t.req.client_id,
          age / 1'000'000'000,
          to_string(r.state),
          r.detail.empty() ? "" : ": ",
          r.detail);
      const TreasuryEvent e{TreasuryEventKind::TimedOut, &t.req, &r};
      port.record(e);
    }
    ++i;
  }
}

void Treasury::finish(std::size_t i,
                      TransferState state,
                      const TransferResult& r,
                      std::int64_t now_ns,
                      TreasuryPort& port) {
  const TreasuryTransfer t = ledger_.in_flight[i];
  ledger_.in_flight.erase(ledger_.in_flight.begin() + static_cast<std::ptrdiff_t>(i));
  const std::size_t f = cfg_.index_of(t.req.from);
  const std::size_t d = cfg_.index_of(t.req.to);
  if (state == TransferState::Done) {
    ++stats_.done;
    stats_.moved = stats_.moved + t.req.amount;
    settling_ = true;
    settle_from_ = t.req.from;
    settle_to_ = t.req.to;
    settle_from_ns_ = now_ns;
    FASTMM_LOG_INFO("[treasury {}] transfer {} done: {} {} {} -> {}{}{}",
                    cfg_.names[0],
                    t.req.client_id,
                    t.req.amount,
                    t.req.asset,
                    cfg_.names[f],
                    cfg_.names[d],
                    r.venue_ref.empty() ? "" : " venue id ",
                    r.venue_ref);
    port.refresh(t.req.from);
    port.refresh(t.req.to);
  } else {
    ++stats_.failed;
    ledger_.limiter.cool_down(cfg_, now_ns);
    FASTMM_LOG_WARN("[treasury {}] transfer {} failed: {} {} {} -> {}: {}; cooling down {} s",
                    cfg_.names[0],
                    t.req.client_id,
                    t.req.amount,
                    t.req.asset,
                    cfg_.names[f],
                    cfg_.names[d],
                    r.detail,
                    cfg_.cooldown_ns / 1'000'000'000);
  }
  persist();
  const TreasuryEvent e{
      state == TransferState::Done ? TreasuryEventKind::Done : TreasuryEventKind::Failed,
      &t.req,
      &r};
  port.record(e);
}

bool Treasury::settled(std::int64_t now_ns, std::span<const TreasuryBalance> balances) const {
  if (now_ns - settle_from_ns_ >= cfg_.settle_ns) return true;
  bool from = false;
  bool to = false;
  for (const TreasuryBalance& b : balances) {
    if (!same_currency(b.asset.view(), cfg_.asset) || b.as_of_ns < settle_from_ns_) continue;
    from = from || b.venue == settle_from_;
    to = to || b.venue == settle_to_;
  }
  return from && to;
}

void Treasury::plan_and_send(std::int64_t now_ns,
                             std::span<const TreasuryBalance> balances,
                             TreasuryPort& port) {
  std::array<TreasuryAccount, TreasuryConfig::kMax> acc{};
  std::size_t n = 0;
  for (const TreasuryBalance& b : balances) {
    if (n == acc.size()) break;
    if (!same_currency(b.asset.view(), cfg_.asset) || cfg_.index_of(b.venue) >= cfg_.members.size())
      continue;
    acc[n++] = TreasuryAccount{b.venue, b.free, b.known};
  }
  const TreasuryPlan plan = plan_transfers(cfg_, std::span<const TreasuryAccount>(acc.data(), n));
  if (plan.status != TreasuryPlan::Status::Transfers) return;
  const PlannedTransfer& p = plan.transfers[0];
  const std::size_t f = cfg_.index_of(p.from);
  const std::size_t d = cfg_.index_of(p.to);

  TransferRequest req;
  req.from = p.from;
  req.to = p.to;
  req.asset = cfg_.asset;
  req.amount = p.amount;
  req.created_ms = now_ns / 1'000'000;

  if (cfg_.dry_run) {
    // The same plan is logged again only after min_interval_s.
    if (last_dry_plan_ == p && now_ns - last_dry_plan_ns_ < cfg_.min_interval_ns) return;
    last_dry_plan_ = p;
    last_dry_plan_ns_ = now_ns;
    ++stats_.plans;
    ++stats_.dry_run_plans;
    req.client_id = "dry-run";
    FASTMM_LOG_INFO("[treasury {}] dry run: would transfer {} {} {} -> {} ({} transfer(s) planned)",
                    cfg_.names[0],
                    p.amount,
                    cfg_.asset,
                    cfg_.names[f],
                    cfg_.names[d],
                    plan.count);
    const TreasuryEvent e{TreasuryEventKind::Planned, &req, nullptr};
    port.record(e);
    return;
  }

  ++stats_.plans;
  if (const TreasuryLimiter::Verdict v = ledger_.limiter.check(cfg_, now_ns);
      v != TreasuryLimiter::Verdict::Ok) {
    ++stats_.limited;
    FASTMM_LOG_DEBUG("[treasury {}] transfer of {} {} {} -> {} held back ({})",
                     cfg_.names[0],
                     p.amount,
                     cfg_.asset,
                     cfg_.names[f],
                     cfg_.names[d],
                     to_string(v));
    return;
  }

  // The ledger has the transfer before the venue does: a process that dies in between asks for it
  // on its next start instead of forgetting it.
  req.client_id = client_id(now_ns, ++ledger_.seq);
  TreasuryTransfer t;
  t.req = req;
  t.phase = TreasuryTransfer::Phase::Created;
  t.created_ns = now_ns;
  t.next_poll_ns = now_ns + cfg_.interval_ns;
  ledger_.in_flight.push_back(t);
  ledger_.limiter.on_sent(now_ns);
  persist();
  ++stats_.sent;
  FASTMM_LOG_INFO("[treasury {}] transfer {}: {} {} {} -> {}",
                  cfg_.names[0],
                  req.client_id,
                  p.amount,
                  cfg_.asset,
                  cfg_.names[f],
                  cfg_.names[d]);
  const TransferResult r = port.submit(req);
  {
    const TreasuryEvent e{TreasuryEventKind::Sent, &req, &r};
    port.record(e);
  }
  TreasuryTransfer& mine = ledger_.in_flight.back();
  switch (r.state) {
    case TransferState::Done:
    case TransferState::Failed:
      finish(ledger_.in_flight.size() - 1, r.state, r, now_ns, port);
      return;
    case TransferState::Pending:
      mine.phase = TreasuryTransfer::Phase::Submitted;
      break;
    case TransferState::NotFound:
    case TransferState::Unknown:
      ++stats_.errors;
      mine.phase = TreasuryTransfer::Phase::Unknown;
      FASTMM_LOG_WARN("[treasury {}] transfer {}: no answer ({}); asking for its state",
                      cfg_.names[0],
                      req.client_id,
                      r.detail);
      break;
  }
  persist();
}

}  // namespace fastmm
