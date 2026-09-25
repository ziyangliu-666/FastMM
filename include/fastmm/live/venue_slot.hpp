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
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/registry.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/venue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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
  Hook hook = nullptr;  // set before the thread starts
  Pending pending = nullptr;
  void* hook_ctx = nullptr;
  std::thread thread;
};

using VenueSlots = std::vector<std::unique_ptr<VenueSlot>>;

// Builds one connector per [venues.*] section and loads its reference data into `instruments`.
// Returns 0, or the exit code (kExitConfig, kExitVenue) after printing the error prefixed with
// `prog` to stderr.
[[nodiscard]] int make_venue_slots(const Config& cfg,
                                   const venues::VenueFactoryOptions& vopts,
                                   InstrumentTable& instruments,
                                   const char* prog,
                                   VenueSlots& slots);

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

// LiveTransport's wake hook context: the slots, and whether the network threads never block.
struct Wake {
  VenueSlots* slots;
  bool busy;  // [engine] spin_mode = "busy": the network threads never block
};
// LiveTransport::WakeFn: the engine queued orders for venue `v`.
void wake_venue(void* ctx, VenueId v) noexcept;

// The network thread (fm-net-<index>). Busy: poll sockets and the engine's wake flag forever.
// Adaptive: the same while active and for a short spin after, then block in the reactor until a
// socket, timer, posted task or the engine (wake_venue) needs the thread. s.consumer, when set, is
// notified after the sinks pushed events (it wakes an engine blocked while idle), in busy mode too.
void net_loop(VenueSlot& s, int cpu, std::size_t index, SpinMode spin);

// The once-a-second status line of a venue, and its latency and feed lines.
void log_venue_status(const venues::Venue& v);
void log_wire_latency(std::string_view venue, const venues::VenueStatus& st, bool final);
void log_feed(std::string_view venue, const venues::VenueFeedStatus& f, bool final);

// A venue's status in a status segment's venue entry (all but its name and kill state).
void fill_status_venue(const venues::VenueStatus& st, StatusVenue& sv) noexcept;

}  // namespace fastmm::live
