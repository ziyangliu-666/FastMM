// The tape the passes of a sweep share (src/backtest/tape.hpp): readers on their own threads each
// read the source's events in venue order, byte for byte, while the chunks are written again
// behind them; a stop wakes every waiting thread.
#include "../../src/backtest/tape.hpp"

#include "random_market.hpp"

#include "fastmm/sim/venue_order.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// Every event of `source`, one after another.
std::vector<std::byte> bytes_of(sim::MdSource& source) {
  std::vector<std::byte> out;
  while (const EventHeader* h = source.next()) {
    const auto* p = reinterpret_cast<const std::byte*>(h);
    out.insert(out.end(), p, p + h->len);
  }
  return out;
}

RandomMarketConfig market() {
  RandomMarketConfig rc;
  rc.seed = 21;
  rc.events = 40'000;  // about 5 MB: two slots of 64 KiB are written again many times
  rc.venues = {VenueId{0}, VenueId{0}};
  return rc;
}

}  // namespace

TEST_CASE("backtest.tape: readers on their own threads read the events in venue order") {
  // One market for both: a message's bytes past its last level are whatever its buffer held.
  RandomMarket source(market());
  sim::VenueOrderSource ordered(source, milliseconds(1000));
  const std::vector<std::byte> want = bytes_of(ordered);
  REQUIRE(want.size() > 4 * 65'536);
  source.reset();
  constexpr std::size_t kReaders = 3;
  Tape tape(source, milliseconds(1000), kReaders, 2);
  std::vector<std::vector<std::byte>> got(kReaders);
  {
    std::vector<std::jthread> threads;
    threads.emplace_back([&] { tape.fill(); });
    for (std::size_t r = 0; r < kReaders; ++r) {
      threads.emplace_back([&, r] {
        Tape::Cursor cursor(tape);
        CHECK(cursor.start_ts() == source.start_ts());
        got[r] = bytes_of(cursor);
        CHECK(cursor.next() == nullptr);  // stays at its end
      });
    }
  }
  for (const std::vector<std::byte>& g : got) CHECK(g == want);
  REQUIRE(tape.ordered() != nullptr);
  CHECK(tape.ordered()->reordered() == ordered.reordered());
  CHECK(tape.ordered()->late() == ordered.late());
}

TEST_CASE("backtest.tape: a stop wakes the reading thread and the readers") {
  RandomMarket source(market());
  Tape tape(source, Duration{}, 2, 2);
  // The reading waits for a reader that reads one event and stops; the other reader is waiting
  // or reading.
  std::jthread fill([&] { tape.fill(); });
  Tape::Cursor slow(tape);
  REQUIRE(slow.next() != nullptr);
  std::jthread other([&] {
    Tape::Cursor cursor(tape);
    CHECK_THROWS_WITH_AS(
        [&] {
          while (cursor.next() != nullptr) {
          }
        }(),
        "tape: stopped",
        std::runtime_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  tape.stop();
  other.join();
  fill.join();
  CHECK_THROWS_AS(
      [&] {
        while (slow.next() != nullptr) {
        }
      }(),
      std::runtime_error);
}
