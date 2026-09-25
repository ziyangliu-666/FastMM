#include "test_support.hpp"

#include "fastmm/venues/connector_common.hpp"
TEST_CASE("venues.smoke") {
  CHECK(true);
}

TEST_CASE("venues.status: a quiet user or order channel is live, quiet market data is stale") {
  using fastmm::venues::ChannelState;
  CHECK(fastmm::venues::channel_state(fastmm::net::ConnState::Stale) == ChannelState::Stale);
  CHECK(fastmm::venues::private_channel_state(fastmm::net::ConnState::Stale) == ChannelState::Live);
  CHECK(fastmm::venues::private_channel_state(fastmm::net::ConnState::Backoff) ==
        ChannelState::Down);
}
