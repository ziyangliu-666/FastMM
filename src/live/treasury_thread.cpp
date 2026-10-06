#include "fastmm/live/treasury_thread.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/venue.hpp"

#include <chrono>
#include <utility>

namespace fastmm::live {

LiveTreasuryPort::LiveTreasuryPort(
    VenueId primary, VenueOf venue, Refresh refresh, MsgRing* journal, Wake wake)
    : primary_(primary),
      venue_(std::move(venue)),
      refresh_(std::move(refresh)),
      journal_(journal),
      wake_(std::move(wake)) {}

bool LiveTreasuryPort::named(const TransferRequest& req,
                             TransferRequest& out,
                             std::string& why) const {
  venues::Venue* from = venue_(req.from);
  venues::Venue* to = venue_(req.to);
  if (from == nullptr || to == nullptr || venue_(primary_) == nullptr) {
    why = "an account of the transfer has no connector in this process";
    return false;
  }
  out = req;
  out.from_account = from->transfer_account();
  out.to_account = to->transfer_account();
  return true;
}

TransferResult LiveTreasuryPort::submit(const TransferRequest& req) {
  TransferRequest named_req;
  std::string why;
  if (!named(req, named_req, why)) return {TransferState::Failed, {}, why};
  return venue_(primary_)->transfer(named_req);
}

TransferResult LiveTreasuryPort::status(const TransferRequest& req) {
  TransferRequest named_req;
  std::string why;
  if (!named(req, named_req, why)) return {TransferState::Unknown, {}, why};
  return venue_(primary_)->transfer_status(named_req);
}

void LiveTreasuryPort::refresh(VenueId account) {
  if (refresh_) refresh_(account);
}

void fill_transfer_msg(ControlTransferMsg& m, VenueId primary, const TreasuryEvent& e) noexcept {
  m = ControlTransferMsg{};
  init_header(m, EventType::Control, InstrumentId::invalid(), primary);
  m.command = ControlCommand::Transfer;
  m.event = static_cast<std::uint8_t>(e.kind);
  m.state =
      static_cast<std::uint8_t>(e.result != nullptr ? e.result->state : TransferState::Unknown);
  if (e.req != nullptr) {
    m.from = e.req->from;
    m.to = e.req->to;
    m.arg = static_cast<std::uint64_t>(e.req->amount.raw);
    m.asset.assign(e.req->asset);
    m.client_id.assign(e.req->client_id);
  }
  if (e.result != nullptr) m.venue_ref.assign(e.result->venue_ref);
  m.hdr.recv_ts = wall_now();
}

void LiveTreasuryPort::record(const TreasuryEvent& e) {
  if (journal_ == nullptr) return;
  ControlTransferMsg m;
  fill_transfer_msg(m, primary_, e);
  if (!journal_->try_push(&m, m.hdr.len)) {
    ++dropped_;
    FASTMM_LOG_ERROR("treasury: the treasury ring is full; a transfer record ({}) is not journaled",
                     to_string(e.kind));
    return;
  }
  if (wake_) wake_();
}

// ---- thread -------------------------------------------------------------------------------------

TreasuryThread::TreasuryThread(std::vector<Pool> pools, Balances balances, Clock clock)
    : pools_(std::move(pools)), balances_(std::move(balances)), clock_(std::move(clock)) {
  if (!clock_) clock_ = [] { return wall_now().ns; };
  publish();
}

TreasuryThread::~TreasuryThread() {
  request_stop();
  join();
}

void TreasuryThread::start() {
  if (pools_.empty() || thread_.joinable()) return;
  thread_ = std::thread([this] { run(); });
}

Result<void, std::string> TreasuryThread::reload() {
  for (Pool& p : pools_) {
    if (auto r = p.treasury->open(); !r) return fail(r.error());
  }
  return {};
}

void TreasuryThread::request_stop() noexcept {
  {
    const std::lock_guard<std::mutex> lock(mu_);
    stop_.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
}

void TreasuryThread::join() {
  if (thread_.joinable()) thread_.join();
}

void TreasuryThread::step() {
  rows_.clear();
  if (balances_) balances_(rows_);
  const std::int64_t now = clock_();
  for (Pool& p : pools_) {
    if (stop_.load(std::memory_order_relaxed)) break;
    p.treasury->step(now, rows_, *p.port);
  }
  publish();
}

void TreasuryThread::run() {
  set_thread_name("fm-treasury");
  std::unique_lock<std::mutex> lock(mu_);
  while (!stop_.load(std::memory_order_relaxed)) {
    lock.unlock();
    step();
    lock.lock();
    cv_.wait_for(
        lock, std::chrono::seconds(1), [this] { return stop_.load(std::memory_order_relaxed); });
  }
}

void TreasuryThread::publish() {
  StatusTreasury s;
  s.pools = static_cast<std::uint32_t>(pools_.size());
  for (const Pool& p : pools_) {
    const TreasuryStats& t = p.treasury->stats();
    s.in_flight += t.in_flight;
    s.plans += t.plans;
    s.dry_run_plans += t.dry_run_plans;
    s.sent += t.sent;
    s.done += t.done;
    s.failed += t.failed;
    s.timed_out += t.timed_out;
    s.limited += t.limited;
    s.errors += t.errors;
  }
  stats_.store(s);
}

StatusTreasury TreasuryThread::stats() const noexcept {
  return stats_.load();
}

}  // namespace fastmm::live
