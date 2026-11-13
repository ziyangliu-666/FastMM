#pragma once
// A venue and its network thread, as fastmm-live (src/live/session.cpp) and fastmm-gateway
// (src/live/gateway.cpp) both run it: the connector, its reactor, the rings its sinks write into
// and the outbound ring it drains, and net_loop(), the thread body.
//
//   make_venue_slots()   builds the connectors and loads their reference data (main thread)
//   wire_venue_slot()    reactor, rings, sinks; attaches the venue and subscribes its instruments
//   net_loop()           fm-net-<i>: the reactor loop (connect, poll, drain on wake, disconnect)
//
// A VenueSlot's hook, when set, runs on the network thread after every reactor iteration; the
// gateway moves the engine's orders from a shared ring into the venue there. `pending` tells an
// adaptive thread about to block that the hook has work; `blocked` and `consumer` are where the
// sleeping flag and the engine's wake-up live, which the gateway moves into the attached strategy's
// wake page (live/gateway.hpp).
#include "fastmm/config/config.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/order_budget.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/live/paper_venue.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/md_copy.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/venue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastmm::live {

// Ring sizes are powers of two of at least 64 KiB.
[[nodiscard]] std::size_t ring_size(std::size_t bytes);

// Per-venue plumbing. Heap allocated so addresses stay stable for the sinks/hooks.
struct VenueSlot {
  // Runs on the network thread after each reactor iteration; returns how much work it did (0: none,
  // which lets an adaptive thread go idle).
  using Hook = std::size_t (*)(void* ctx) noexcept;
  // With hook_ctx: true when the hook would find work (the recheck before the thread blocks).
  using Pending = bool (*)(void* ctx) noexcept;

  std::unique_ptr<venues::Venue> venue;
  PaperVenue* paper = nullptr;  // `venue` when a dry run trades on paper (make_venue_slots)
  std::unique_ptr<net::Reactor> reactor;
  std::unique_ptr<MsgRing> md_ring;
  std::unique_ptr<MsgRing> order_ring;
  std::unique_ptr<MsgRing> outbound;
  venues::EventSink md_sink;
  venues::EventSink order_sink;
  std::atomic<bool> wake{false};
  SleepFlag net_blocked;  // set while an adaptive network thread blocks in the reactor
  // The flag the network thread sets before it blocks: net_blocked, or the one in the attached
  // strategy's wake page. Read and switched on the network thread only (set before it starts).
  SleepFlag* blocked = &net_blocked;
  // Notified after the sinks pushed events: the engine's feed waker, or nullptr when the engine
  // never blocks. Network thread only, like `blocked`.
  Waker* consumer = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> order_overflows{0};
  // Redundant market data ([venues.*] md_copies): the filter every copy and the venue's own feed
  // ask, and the copies this slot's network thread runs (pushing into md_sink). Copies on threads
  // of their own are in an MdCopyThread.
  std::unique_ptr<venues::UpdateFilter> md_filter;
  std::vector<std::unique_ptr<venues::MdCopy>> md_copies;
  Hook hook = nullptr;  // set before the thread starts
  Pending pending = nullptr;
  void* hook_ctx = nullptr;
  std::thread thread;
};

using VenueSlots = std::vector<std::unique_ptr<VenueSlot>>;

// Builds one connector per [venues.*] section and loads its reference data into `instruments`.
// Returns 0, or the exit code (kExitConfig, kExitVenue) after printing the error prefixed with
// `prog` to stderr. With `paper` (fastmm-live --dry-run) every venue that takes orders is a
// PaperVenue over its dry-run connector.
[[nodiscard]] int make_venue_slots(const Config& cfg,
                                   const venues::VenueFactoryOptions& vopts,
                                   InstrumentTable& instruments,
                                   const char* prog,
                                   VenueSlots& slots,
                                   bool paper = false);

// [engine] net_backend, falling back to epoll (with a warning) when io_uring is not available.
[[nodiscard]] net::ReactorBackend resolve_net_backend(const Config& cfg);

// Gives the slot its reactor and rings ([engine] md_ring_bytes / order_ring_bytes), attaches the
// sinks (md: lossy, orders: spin, overflow counted in order_overflows), attaches the venue and
// subscribes the instruments of venue `vid`. Main thread, before the network thread starts.
void wire_venue_slot(VenueSlot& s,
                     VenueId vid,
                     const Config& cfg,
                     net::ReactorBackend backend,
                     const venues::SymbolTable& symbols,
                     const InstrumentTable& instruments,
                     const Seqlocked<TscCalibration>* tsc);

// Copies of a venue's market data on a network thread of their own ([venues.*] md_copy_cpus):
// their reactor, the ring they push into (the engine reads it like a venue's md ring) and the
// thread, fm-mdc-<venue>-<core>.
struct MdCopyThread {
  std::string name;
  int cpu = -1;
  bool never_block = false;  // spin_mode = "busy", or net_spin_dedicated on a core of its own
  std::unique_ptr<net::Reactor> reactor;
  std::unique_ptr<MsgRing> ring;
  venues::EventSink sink;
  std::vector<std::unique_ptr<venues::MdCopy>> copies;
  // Notified after the copies pushed (the engine's feed waker), or nullptr when the engine never
  // blocks. Set before the thread starts.
  Waker* consumer = nullptr;
  std::atomic<bool> stop{false};
  std::thread thread;
};
using MdCopyThreads = std::vector<std::unique_ptr<MdCopyThread>>;

// After wire_venue_slot (main thread): the copies of venue `index`, the larger of [venues.*]
// md_copies and Venue::md_copies_requested(). Copy k with a core in md_copy_cpus goes to the
// MdCopyThread of that core in `threads` (made here, one per distinct core; the caller adds its
// ring to the engine's feed and starts it); the others, and every copy when `threads` is nullptr
// (threading = "single", fastmm-gateway), run on the slot's own thread. A connector without copies
// gets a warning and none.
void wire_md_copies(VenueSlot& s,
                    std::size_t index,
                    const Config& cfg,
                    net::ReactorBackend backend,
                    MdCopyThreads* threads);
// The slot's own copies, on its network thread, next to Venue::connect / disconnect.
void connect_md_copies(VenueSlot& s);
void disconnect_md_copies(VenueSlot& s);
// Starts, and stops and joins, the copy threads. Stopping logs each copy's counts: how many
// updates it delivered first and how many after another connection.
void start_md_copy_threads(MdCopyThreads& threads);
void stop_md_copy_threads(MdCopyThreads& threads);
// The counts of one venue's copies on its own thread (stop_md_copy_threads logs the others).
void log_md_copies(std::span<const std::unique_ptr<venues::MdCopy>> copies);
// The core of every copy thread of the configuration ([venues.*] md_copy_cpus, >= 0), and whether
// its thread never blocks: spin_mode = "busy", or net_spin_dedicated on a core shared with neither
// the engine (`engine_cpu`) nor a network or another copy thread.
struct CopyCore {
  int cpu = -1;
  bool never_block = false;
};
[[nodiscard]] std::vector<CopyCore> md_copy_cores(const Config& cfg, int engine_cpu);

// LiveTransport's wake hook context: the slots, and whether the network threads never block.
struct Wake {
  VenueSlots* slots;
  bool busy;  // [engine] spin_mode = "busy": the network threads never block
};
// LiveTransport::WakeFn: the engine queued orders for venue `v`.
void wake_venue(void* ctx, VenueId v) noexcept;

// Whether network thread `index` shares its pinned CPU with the engine (`engine_cpu`, -1 when it
// runs elsewhere) or with another network thread. An unpinned thread (-1) shares nothing.
[[nodiscard]] bool net_cpu_shared(int engine_cpu, std::span<const int> net_cpus, std::size_t index);

// Whether a network thread polls without ever blocking: always with SpinMode::Busy; with Adaptive
// when `spin_dedicated` ([engine] net_spin_dedicated) and the thread is pinned (`cpu` >= 0) to a
// core it shares with nobody (`shared_cpu` false, net_cpu_shared). A blocked thread pays a
// wake-up (microseconds) on the next packet or order.
[[nodiscard]] bool net_never_blocks(SpinMode spin,
                                    int cpu,
                                    bool shared_cpu,
                                    bool spin_dedicated) noexcept;

// The network threads: venues that net_cpus pins to one core share a thread when that thread polls
// without blocking (spin_mode = "busy", or net_spin_dedicated) and the core is not the engine's;
// each other venue has one of its own. Venue indices, each group in ascending order, the groups in
// the order of their first venue.
[[nodiscard]] std::vector<std::vector<std::size_t>> net_groups(int engine_cpu,
                                                               std::span<const int> net_cpus,
                                                               std::size_t venues,
                                                               SpinMode spin,
                                                               bool spin_dedicated);

// The network thread (fm-net-<index>). Busy: poll sockets and the engine's wake flag forever.
// Adaptive: the same while active and for a short spin after, then block in the reactor until a
// socket, timer, posted task or the engine (wake_venue) needs the thread. s.consumer, when set, is
// notified after the sinks pushed events (it wakes an engine blocked while idle), in busy mode too.
// `shared_cpu` (net_cpu_shared): an adaptive thread blocks as soon as it is idle, without the
// spin, since a spinning thread keeps the CPU for a whole scheduler slice (milliseconds) from the
// thread beside it that has an order to send. `spin_dedicated`: an adaptive thread alone on its
// pinned core never blocks (net_never_blocks).
void net_loop(VenueSlot& s,
              int cpu,
              std::size_t index,
              SpinMode spin,
              bool shared_cpu = false,
              bool spin_dedicated = false);
// A group of venues (net_groups) on one thread: each venue's turn of the loop above in order, as
// often as the thread can; it never blocks. `index` names the thread (fm-net-<index>, its first
// venue), the first venue's `stop` ends it, and each venue is connected, flushed and disconnected
// as on a thread of its own.
void net_loop(std::span<VenueSlot* const> group,
              int cpu,
              std::size_t index,
              SpinMode spin,
              bool shared_cpu,
              bool spin_dedicated);

// Starts the network threads of `slots` ([engine] net_cpus, spin_mode, net_spin_dedicated;
// `engine_cpu`: the engine's core, -1 when it runs in another process): one per group of
// net_groups, held by the group's first slot (VenueSlot::thread; the others' stay empty).
void start_net_threads(VenueSlots& slots, const Config& cfg, int engine_cpu);

// The once-a-second status line of a venue, and its latency and feed lines.
void log_venue_status(const venues::Venue& v);
void log_wire_latency(std::string_view venue, const venues::VenueStatus& st, bool final);
void log_feed(std::string_view venue, const venues::VenueFeedStatus& f, bool final);

// A venue's status in a status segment's venue entry (all but its name and kill state).
void fill_status_venue(const venues::VenueStatus& st, StatusVenue& sv) noexcept;
// The order budget a connector published (Venue::budget_source) into its status entry.
void fill_status_budget(const OrderBudget& b, StatusVenue& sv) noexcept;

}  // namespace fastmm::live
