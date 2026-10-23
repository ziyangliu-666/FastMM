#pragma once
// The latencies of a live session's own messages (sim/recorded_latency.hpp), read from its journal:
// for each order or cancel, its send time against the venue's time on its first ack or cancel ack;
// for each ack, cancel ack and fill, its receipt against the venue's time on it (a fill against its
// public trade's, when the journal has the trade). Venue times in whole milliseconds (Binance's)
// are taken at the middle of their millisecond.
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/recorded_latency.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace fastmm::bt {

// Takes a journal's records in order (JournalSource reads them once) and keeps what it needs.
class RecordedLatencyCollector {
 public:
  void on_event(const EventHeader& h);
  // The latencies; fills are resolved against the trades seen.
  [[nodiscard]] sim::RecordedLatency finish() const { return sim::RecordedLatency(samples()); }
  // The samples finish() builds them from, with the journal's venue and instrument ids.
  [[nodiscard]] std::vector<sim::RecordedLatency::Sample> samples() const;

 private:
  struct Sent {
    std::int64_t sent = 0;
    std::int64_t cancel = 0;
    std::uint32_t instrument = 0;
    VenueId venue{};
    bool acked = false;
    bool cancel_acked = false;
  };
  // A venue's reply: when it was sent and received; for an ack also the send time of what it
  // answers.
  struct Reply {
    sim::RecordedLatency::Path path;
    VenueId venue{};
    std::uint32_t instrument = 0;
    std::int64_t venue_ns = 0;
    std::int64_t recv_ns = 0;
    std::int64_t sent_ns = 0;  // 0: a fill
    std::uint64_t trade = 0;   // a fill's public trade id (0: none)
  };
  std::unordered_map<std::uint64_t, Sent> sent_;
  std::vector<Reply> replies_;
  std::unordered_map<std::uint64_t, std::int64_t> trades_;  // instrument and trade id -> venue ns
  bool ms_ = true;  // every order event's venue time so far is a whole millisecond
  std::uint64_t venue_times_ = 0;
};

// recorded latencies of a journal's parts, in order (resets each reader).
[[nodiscard]] sim::RecordedLatency recorded_latency(std::span<JournalReader> parts);

}  // namespace fastmm::bt
