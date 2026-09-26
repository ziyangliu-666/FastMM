#include "fastmm/store/store_thread.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <ctime>

namespace fastmm::store {

namespace {
// How long the drain thread sleeps when the ring is empty. It bounds how long a record waits
// before it is committed, so it also bounds what a crash loses.
constexpr long kIdleSleepNs = 2'000'000;  // 2 ms
}  // namespace

StoreThread::StoreThread(MsgRing& ring,
                         std::unique_ptr<Backend> backend,
                         const StoreThreadOptions& opts)
    : ring_(ring), backend_(std::move(backend)), opts_(opts) {}

StoreThread::~StoreThread() {
  stop();
}

void StoreThread::start() {
  if (thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void StoreThread::stop() {
  if (stopped_) return;
  if (thread_.joinable()) {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }
  static_cast<void>(drain(/*force_commit=*/true));
  stopped_ = true;
}

std::size_t StoreThread::drain_once() {
  return drain(/*force_commit=*/true);
}

StoreThreadStats StoreThread::stats() const noexcept {
  StoreThreadStats s;
  s.records = records_.load(std::memory_order_relaxed);
  s.batches = batches_.load(std::memory_order_relaxed);
  s.unknown = unknown_.load(std::memory_order_relaxed);
  return s;
}

// Copies each record out of the ring before handing it to the backend: the backend may keep a
// pointer for the length of the call, and release() frees the slot.
std::size_t StoreThread::drain(bool force_commit) {
  std::size_t n = 0;
  bool open = false;
  const auto close_batch = [&] {
    if (!open) return;
    backend_->commit();
    batches_.fetch_add(1, std::memory_order_relaxed);
    open = false;
  };
  while (const std::byte* p = ring_.try_peek()) {
    const auto* h = reinterpret_cast<const RecordHeader*>(p);
    if (!open) {
      backend_->begin();
      open = true;
    }
    switch (h->type) {
      case RecordType::Fill: {
        FillRecord r;
        std::memcpy(&r, p, sizeof r);
        backend_->fill(r);
        break;
      }
      case RecordType::Order: {
        OrderRecord r;
        std::memcpy(&r, p, sizeof r);
        backend_->order(r);
        break;
      }
      case RecordType::Position: {
        PositionRecord r;
        std::memcpy(&r, p, sizeof r);
        backend_->position(r);
        break;
      }
      case RecordType::Kill: {
        KillRecord r;
        std::memcpy(&r, p, sizeof r);
        backend_->kill(r);
        break;
      }
      case RecordType::Funding: {
        FundingRecord r;
        std::memcpy(&r, p, sizeof r);
        backend_->funding(r);
        break;
      }
      default:
        unknown_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    ring_.release();
    ++n;
    records_.fetch_add(1, std::memory_order_relaxed);
    if (n % opts_.max_batch == 0) close_batch();
  }
  if (force_commit || open) close_batch();
  return n;
}

void StoreThread::run() {
  set_thread_name("fm-store");
  Logger::instance().attach_current_thread();
  while (!stop_.load(std::memory_order_acquire)) {
    if (drain(/*force_commit=*/true) == 0) {
      timespec ts{0, kIdleSleepNs};  // a store is never on a latency path
      nanosleep(&ts, nullptr);
    }
  }
  static_cast<void>(drain(/*force_commit=*/true));
}

}  // namespace fastmm::store
