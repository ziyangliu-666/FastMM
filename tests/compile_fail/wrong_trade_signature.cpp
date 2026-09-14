// Expected: the Engine constructor rejects on_trade without the InstrumentId parameter.
#include "fastmm/core/engine.hpp"

#include <cstddef>
#include <span>

namespace {

struct NullTransport {
  bool send(const fastmm::EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const fastmm::EventHeader* const> b) noexcept { return b.size(); }
  bool supports_replace(fastmm::VenueId) const noexcept { return false; }
};

struct Quoter {
#ifdef FASTMM_CF_CONTROL
  void on_trade(auto&, fastmm::InstrumentId, const fastmm::TradeMsg&) noexcept {}
#else
  void on_trade(auto&, const fastmm::TradeMsg&) noexcept {}  // the old shape
#endif
};

using TestEngine = fastmm::Engine<Quoter, fastmm::SimClock, NullTransport, fastmm::InlineFeed>;

[[maybe_unused]] void build(const fastmm::InstrumentTable& table,
                            fastmm::SimClock& clock,
                            NullTransport& transport,
                            fastmm::InlineFeed& feed,
                            Quoter& strategy) {
  TestEngine engine(fastmm::EngineConfig{}, table, clock, transport, feed, strategy);
  engine.start();
}

}  // namespace
