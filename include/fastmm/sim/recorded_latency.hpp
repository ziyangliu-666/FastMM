#pragma once
// RecordedLatency: the latencies a live session's own messages had, by time, for a simulated venue
// to follow ([backtest] latency_from_journal). A venue's order gateway and its replies are not as
// fast in a burst as on average: when the market moves, a cancel can take many times its usual
// time to reach the book and the replies come back late, which a latency model drawing every
// message from one distribution does not produce at the moment it matters. The journal records each
// order's send time, the venue's time on its ack (or cancel ack) and the ack's receipt, so for
// every moment it says how long the path took.
//
// out(): the time to the venue of a live order (or cancel) sent near `t` on the same account: the
// one of the same instrument within twin_window (the same decision, live), else the nearest within
// window, else none. in(): the reply path's time from the venue's time `t`: the nearest live reply
// (acks for acks, fills for fills, else either) within window. A simulated message takes the
// recorded time where it is longer than its model draw by more than margin, so a quiet moment keeps
// the model (the venue's millisecond timestamps make each recorded value uncertain by half a
// millisecond) and a congested one gets what live got.
//
// Control path: built once before the run; queries are binary searches over sorted arrays.
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace fastmm::sim {

class RecordedLatency {
 public:
  enum class Path : std::uint8_t { New = 0, Cancel = 1, Ack = 2, Fill = 3 };
  struct Sample {
    std::int64_t t_ns = 0;     // out paths: the send time; reply paths: the venue's time
    std::int64_t took_ns = 0;  // how long the message took on the path
    std::uint32_t instrument = 0;
    VenueId venue{};
    Path path = Path::New;
  };

  RecordedLatency() = default;
  explicit RecordedLatency(const std::vector<Sample>& samples);

  // How long a message of `path` sent (out) or answered (in) at `t` took live; 0: none recorded.
  [[nodiscard]] Duration out(Path path, VenueId venue, std::uint32_t instrument, Timestamp t) const;
  [[nodiscard]] Duration in(Path path, VenueId venue, Timestamp t) const;

  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t n = 0;
    for (const std::vector<Sample>& v : by_path_) n += v.size();
    return n;
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  Duration twin_window = milliseconds(2);
  Duration window = milliseconds(20);
  Duration margin = milliseconds(1);

 private:
  [[nodiscard]] const Sample* nearest(const std::vector<Sample>& v,
                                      Timestamp t,
                                      VenueId venue,
                                      std::uint32_t instrument,
                                      bool by_instrument) const;
  std::vector<Sample> by_path_[4];  // each sorted by t_ns
};

}  // namespace fastmm::sim
