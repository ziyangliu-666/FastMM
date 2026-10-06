#include "fastmm/live/fill_auditor.hpp"

#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace fastmm::live {

namespace {

std::int64_t wall_ms() {
  return wall_now().ns / 1'000'000;
}

// One execution in the log: string arguments are capped (kLogMaxStrBytes), so field by field.
void log_row(std::string_view venue, std::string_view what, const AuditFill& f) {
  FASTMM_LOG_WARN("{}: fill audit: {} execution {} {} {} {} @ {} fee {} {} order {} at {} ms",
                  venue,
                  what,
                  f.symbol,
                  f.exec_id,
                  to_string(f.side),
                  Qty::from_raw(f.qty_raw),
                  Price::from_raw(f.price_raw),
                  Qty::from_raw(f.fee_raw),
                  f.fee_asset,
                  f.order_id.empty() ? std::string_view("-") : std::string_view(f.order_id),
                  f.time_ms);
}

}  // namespace

FillAuditor::FillAuditor(std::vector<FillAuditTarget> targets,
                         ReadBooked read,
                         std::int64_t start_ms)
    : targets_(std::move(targets)),
      read_(std::move(read)),
      next_from_ms_(targets_.size(), start_ms),
      reported_(targets_.size()),
      counters_(std::make_unique<Counters[]>(targets_.size())) {}

FillAuditor::~FillAuditor() {
  stop();
}

void FillAuditor::start() {
  if (thread_.joinable() || targets_.empty()) return;
  thread_ = std::thread([this] { run(); });
}

void FillAuditor::stop() {
  {
    const std::lock_guard lock(mu_);
    stop_ = true;
  }
  stopping_.store(true);
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

FillAuditCounters FillAuditor::counters(std::size_t i) const noexcept {
  FillAuditCounters c;
  if (i >= targets_.size()) return c;
  const Counters& s = counters_[i];
  c.audits = s.audits.load(std::memory_order_relaxed);
  c.failures = s.failures.load(std::memory_order_relaxed);
  c.missing = s.missing.load(std::memory_order_relaxed);
  c.phantom = s.phantom.load(std::memory_order_relaxed);
  c.mismatched = s.mismatched.load(std::memory_order_relaxed);
  c.duplicates = s.duplicates.load(std::memory_order_relaxed);
  c.booked = s.booked.load(std::memory_order_relaxed);
  return c;
}

void FillAuditor::run() {
  set_thread_name("fm-audit");
  Logger::instance().attach_current_thread();
  std::vector<std::int64_t> due(targets_.size());
  const std::int64_t t0 = wall_ms();
  for (std::size_t i = 0; i < targets_.size(); ++i) due[i] = t0 + targets_[i].interval_ms;
  std::unique_lock lock(mu_);
  while (!stop_) {
    const std::int64_t next = *std::min_element(due.begin(), due.end());
    const std::int64_t wait = next - wall_ms();
    if (wait > 0) {
      cv_.wait_for(lock, std::chrono::milliseconds(wait), [this] { return stop_; });
      continue;
    }
    lock.unlock();
    for (std::size_t i = 0; i < targets_.size() && !stopping_.load(); ++i) {
      const std::int64_t now = wall_ms();
      if (due[i] > now) continue;
      static_cast<void>(audit_once(i, now));
      due[i] = std::max(due[i] + targets_[i].interval_ms, wall_ms());
    }
    lock.lock();
  }
}

Result<FillAuditReport, std::string> FillAuditor::audit_once(std::size_t i, std::int64_t now_ms) {
  const FillAuditTarget& t = targets_[i];
  if (now_ms - t.lag_ms < next_from_ms_[i]) return fail(std::string("the window is still empty"));
  auto r = audit(i, now_ms);
  Counters& c = counters_[i];
  if (!r) {
    c.failures.fetch_add(1, std::memory_order_relaxed);
    FASTMM_LOG_WARN("{}: fill audit not done, read again with the next: {}",
                    t.venue,
                    std::string_view(r.error()));
    return r;
  }
  c.audits.fetch_add(1, std::memory_order_relaxed);
  report(i, *r);
  return r;
}

Result<FillAuditReport, std::string> FillAuditor::audit(std::size_t i, std::int64_t now_ms) {
  const FillAuditTarget& t = targets_[i];
  const std::int64_t from = next_from_ms_[i];
  const std::int64_t to = now_ms - t.lag_ms;
  struct Answer {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool refused = false;
    bool complete = false;
    std::vector<AuditFill> rows;
  };
  // Shared with the network thread, which may answer after this audit gave up.
  auto answer = std::make_shared<Answer>();
  venues::Venue* v = t.connector;
  t.post([answer, v, from, to] {
    const bool started = v->audit_executions(
        from - kMarginMs, to + kMarginMs, [answer](bool complete, std::vector<AuditFill> rows) {
          const std::lock_guard lock(answer->mu);
          answer->done = true;
          answer->complete = complete;
          answer->rows = std::move(rows);
          answer->cv.notify_all();
        });
    if (!started) {
      const std::lock_guard lock(answer->mu);
      answer->done = true;
      answer->refused = true;
      answer->cv.notify_all();
    }
  });
  std::vector<AuditFill> venue_rows;
  {
    std::unique_lock lock(answer->mu);
    const std::int64_t deadline = wall_ms() + kFetchTimeoutMs;
    while (!answer->done && !stopping_.load() && wall_ms() < deadline)
      answer->cv.wait_for(lock, std::chrono::milliseconds(200));
    if (!answer->done) {
      return fail(stopping_.load() ? std::string("the session is stopping")
                                   : std::string("the venue did not answer in time"));
    }
    if (answer->refused)
      return fail(std::string("the venue cannot be asked now (not connected, or reading)"));
    if (!answer->complete)
      return fail(std::string("the venue's trade history could not be read in full"));
    venue_rows = std::move(answer->rows);
  }
  auto booked = read_(t.venue, from - kMarginMs, to + kMarginMs);
  if (!booked) return fail("the store: " + booked.error());
  FillAuditReport r = audit_fills(venue_rows, *booked, from, to);
  next_from_ms_[i] = to + 1;
  return r;
}

void FillAuditor::report(std::size_t i, const FillAuditReport& r) {
  const FillAuditTarget& t = targets_[i];
  Counters& c = counters_[i];
  // An execution is reported once per kind, also when a later window reads it again.
  auto& seen = reported_[i];
  std::erase_if(seen, [&](const auto& kv) { return kv.second < r.from_ms - kMarginMs; });
  const auto first = [&](char kind, const AuditFill& f) {
    std::string key;
    key += kind;
    key += normalize_symbol(f.symbol);
    key += ' ';
    key += f.exec_id;
    key += f.side == Side::Buy ? 'B' : 'S';
    return seen.emplace(std::move(key), f.time_ms).second;
  };
  std::size_t missing = 0;
  std::size_t phantom = 0;
  std::size_t mismatched = 0;
  std::size_t duplicates = 0;
  for (const AuditFill& f : r.missing) {
    if (!first('m', f)) continue;
    if (missing++ < kMaxLogged) log_row(t.venue, "missing (not booked)", f);
  }
  for (const AuditFill& f : r.phantom) {
    if (!first('p', f)) continue;
    if (phantom++ < kMaxLogged) log_row(t.venue, "phantom (booked, not at the venue)", f);
  }
  for (const AuditMismatch& m : r.mismatched) {
    if (!first('x', m.venue)) continue;
    if (mismatched++ >= kMaxLogged) continue;
    const std::string fields = audit_fields_text(m.fields);
    FASTMM_LOG_WARN("{}: fill audit: execution {} {} differs in {}",
                    t.venue,
                    m.venue.symbol,
                    m.venue.exec_id,
                    std::string_view(fields));
    log_row(t.venue, "  venue", m.venue);
    log_row(t.venue, "  booked", m.booked);
  }
  for (const AuditFill& f : r.duplicates) {
    if (!first('d', f)) continue;
    if (duplicates++ < kMaxLogged) log_row(t.venue, "booked more than once", f);
  }
  c.missing.fetch_add(missing, std::memory_order_relaxed);
  c.phantom.fetch_add(phantom, std::memory_order_relaxed);
  c.mismatched.fetch_add(mismatched, std::memory_order_relaxed);
  c.duplicates.fetch_add(duplicates, std::memory_order_relaxed);
  if (missing + phantom + mismatched + duplicates == 0) {
    FASTMM_LOG_INFO("{}: fill audit {} to {} ms: {} execution(s) agree",
                    t.venue,
                    r.from_ms,
                    r.to_ms,
                    r.matched);
    return;
  }
  FASTMM_LOG_ERROR(
      "{}: fill audit {} to {} ms: {} missing, {} phantom, {} differing, {} booked twice "
      "(venue {}, engine {}, {} agree)",
      t.venue,
      r.from_ms,
      r.to_ms,
      missing,
      phantom,
      mismatched,
      duplicates,
      r.venue_rows,
      r.booked_rows,
      r.matched);
  // "book": the replay reads from the first missing execution not asked for before, and the next
  // audit reads again from there, which tells whether it was booked. Each execution is asked for
  // once: one the replay does not book stays reported, not booked again and again.
  std::int64_t since = 0;
  for (const AuditFill& f : r.missing) {
    if (t.book && first('b', f) && (since == 0 || f.time_ms < since)) since = f.time_ms;
  }
  if (since != 0) {
    next_from_ms_[i] = std::min(next_from_ms_[i], since);
    venues::Venue* v = t.connector;
    t.post([v, since] { static_cast<void>(v->request_executions(since)); });
    c.booked.fetch_add(1, std::memory_order_relaxed);
    FASTMM_LOG_WARN(
        "{}: fill audit: booking the missing execution(s) from the venue's history "
        "since {} ms",
        t.venue,
        since);
  }
}

}  // namespace fastmm::live
