#include "live_runners.hpp"

#include "fastmm/strategies/basic_mm.hpp"

namespace fastmm::live {

std::unique_ptr<IEngineRunner> make_live_basic_mm(RunnerDeps& deps,
                                                  TscClock& clock,
                                                  LiveTransport& transport,
                                                  RingFeed& feed) {
  return make_engine_runner<BasicMM>(deps, clock, transport, feed);
}
const ParamSchema& basic_mm_schema() {
  return BasicMM::schema();
}
std::string_view basic_mm_name() noexcept {
  return BasicMM::name();
}

}  // namespace fastmm::live
