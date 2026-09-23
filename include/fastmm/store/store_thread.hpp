#pragma once
// StoreThread: drains the engine's record ring into a Backend, on its own thread.
//
// The mirror of JournalFileWriter: the engine writes into an SPSC MsgRing and never waits, this
// side batches what it finds into one backend transaction. A pass over the ring commits every
// max_batch records and whatever is left at its end, and the thread sleeps 2 ms when the ring is
// empty; so a busy session does not commit per fill and an idle one commits within 2 ms.
//
// It never signals the engine. A backend that fails counts the error and keeps going; a ring that
// fills drops records on the engine side (EngineStats::records_dropped). Both are logged when the
// session ends and stored in the session's row; neither stops trading (docs/reference/storage.md).
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/store/backend.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace fastmm::store {

struct StoreThreadOptions {
  std::size_t max_batch = 512;  // records per backend transaction
};

struct StoreThreadStats {
  std::uint64_t records = 0;
  std::uint64_t batches = 0;
  std::uint64_t unknown = 0;  // records of a type this build does not know (a newer writer)
};

class StoreThread {
 public:
  StoreThread(MsgRing& ring, std::unique_ptr<Backend> backend, const StoreThreadOptions& opts = {});
  ~StoreThread();
  StoreThread(const StoreThread&) = delete;
  StoreThread& operator=(const StoreThread&) = delete;

  [[nodiscard]] Backend& backend() noexcept { return *backend_; }

  void start();  // spawns the drain thread
  void stop();   // joins, drains what is left, commits

  // Single-threaded driving (tests): drains and commits everything the ring holds now.
  std::size_t drain_once();

  [[nodiscard]] StoreThreadStats stats() const noexcept;

 private:
  void run();
  std::size_t drain(bool force_commit);

  MsgRing& ring_;
  std::unique_ptr<Backend> backend_;
  StoreThreadOptions opts_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> records_{0};
  std::atomic<std::uint64_t> batches_{0};
  std::atomic<std::uint64_t> unknown_{0};
  bool stopped_ = false;
};

}  // namespace fastmm::store
