// Parameters of Python hot strategies (strategies/hot_params.hpp): the fixed-point twin, the
// runtime schema a ParamPublisher validates against, and HotStrategy::apply_param_update.
#include "fastmm/strategies/hot_params.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/testing/strategy_harness.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

std::int64_t fixed(double v) {
  std::int64_t out = -12345;
  REQUIRE(hot_fixed_raw(v, out));
  return out;
}

// Record layout as numpy aligns it: half_spread (f8) at 0 with its _raw twin at 8, levels (i8) at
// 16, enabled (?) at 24; the parameter block is 32 bytes, then a State int64 at 32.
std::vector<HotParamField> fields() {
  std::vector<HotParamField> f(3);
  f[0].name = "half_spread";
  f[0].type = ParamType::Double;
  f[0].offset = 0;
  f[0].raw_offset = 8;
  f[0].min = 0.0;
  f[0].max = 100.0;
  f[0].def = 5.0;
  f[1].name = "levels";
  f[1].type = ParamType::Int;
  f[1].offset = 16;
  f[1].int_min = 1;
  f[1].int_max = 8;
  f[2].name = "enabled";
  f[2].type = ParamType::Bool;
  f[2].offset = 24;
  return f;
}

std::vector<std::uint8_t> block(double half_spread, std::int64_t levels, bool enabled) {
  std::vector<std::uint8_t> b(40, 0);
  const std::int64_t raw = fixed(half_spread);
  std::memcpy(b.data(), &half_spread, 8);
  std::memcpy(b.data() + 8, &raw, 8);
  std::memcpy(b.data() + 16, &levels, 8);
  b[24] = enabled ? 1 : 0;
  return b;
}

const ParamUpdateMsg& front(MsgRing& ring) {
  const std::byte* p = ring.try_peek();
  REQUIRE(p != nullptr);
  return *reinterpret_cast<const ParamUpdateMsg*>(p);
}

std::string error_of(ParamPublisher& pub, const std::vector<ParamPublisher::ParamValue>& v) {
  try {
    static_cast<void>(pub.publish(v));
  } catch (const std::invalid_argument& e) {
    return e.what();
  }
  return {};
}

double g_seen_half_spread = 0.0;
std::int64_t g_seen_raw = 0;
std::int64_t g_seen_levels = 0;

std::int32_t read_params(std::uint8_t* self, fastmm_hot_ctx*, fastmm_hot_book*) {
  std::memcpy(&g_seen_half_spread, self, 8);
  std::memcpy(&g_seen_raw, self + 8, 8);
  std::memcpy(&g_seen_levels, self + 16, 8);
  return FASTMM_HOT_OK;
}

}  // namespace

TEST_CASE("hot params: the fixed-point twin rounds the shortest decimal form, halves away from 0") {
  CHECK(fixed(0.002) == 200'000);
  CHECK(fixed(1.5) == 150'000'000);
  CHECK(fixed(0.0) == 0);
  CHECK(fixed(-0.0) == 0);
  CHECK(fixed(0.000000005) == 1);
  CHECK(fixed(-0.000000005) == -1);
  CHECK(fixed(0.0000000049) == 0);
  CHECK(fixed(0.1 + 0.2) == 30'000'000);  // 0.30000000000000004
  CHECK(fixed(5e-324) == 0);
  CHECK(fixed(9.2e10) == 9'200'000'000'000'000'000);
  std::int64_t out = 0;
  CHECK_FALSE(hot_fixed_raw(1e11, out));
  CHECK_FALSE(hot_fixed_raw(std::numeric_limits<double>::quiet_NaN(), out));
  CHECK_FALSE(hot_fixed_raw(std::numeric_limits<double>::infinity(), out));
}

TEST_CASE("hot params: the table is a schema that parses, checks ranges and formats like Param") {
  const HotParamTable table(fields(), 32);
  const ParamSchema& s = table.schema();
  REQUIRE(s.size() == 3);
  CHECK(std::string(s.begin()[0].name) == "half_spread");
  CHECK(s.begin()[0].type == ParamType::Double);
  CHECK(s.begin()[1].type == ParamType::Int);
  CHECK(s.begin()[2].type == ParamType::Bool);
  CHECK(s.begin()[1].min == 1.0);
  CHECK(std::isinf(s.begin()[2].max));

  HotParamBlock b{&table, block(5.0, 2, true)};
  const ParamDesc& half = s.begin()[0];
  CHECK_FALSE(half.parse(&b, "7.25"));
  CHECK(half.format(&b) == "7.25");
  CHECK(half.get_raw(&b) == std::bit_cast<std::int64_t>(7.25));
  std::int64_t twin = 0;
  std::memcpy(&twin, b.bytes.data() + 8, 8);
  CHECK(twin == 725'000'000);
  CHECK(half.parse(&b, "abc").value_or("") == "cannot parse 'abc' as double (a finite number)");
  CHECK(half.parse(&b, "100.5").value_or("") == "value 100.5 outside [0, 100]");
  CHECK(half.format(&b) == "7.25");  // unchanged by the failures

  const ParamDesc& levels = s.begin()[1];
  CHECK(levels.parse(&b, "1.5").value_or("") == "cannot parse '1.5' as int (a whole number)");
  CHECK(levels.parse(&b, "9").value_or("") == "value 9 outside [1, 8]");
  CHECK_FALSE(levels.parse(&b, "3"));
  CHECK(levels.get_raw(&b) == 3);

  const ParamDesc& enabled = s.begin()[2];
  CHECK_FALSE(enabled.parse(&b, "false"));
  CHECK(enabled.format(&b) == "false");
  enabled.set_raw(&b, 1);
  CHECK(enabled.get_raw(&b) == 1);
  CHECK(enabled.parse(&b, "maybe").value_or("") == "cannot parse 'maybe' as bool (true or false)");

  CHECK_THROWS_AS(HotParamTable(fields(), 24), std::invalid_argument);  // enabled lies outside
  std::vector<HotParamField> twice = fields();
  twice[2].name = "levels";
  CHECK_THROWS_AS(HotParamTable(twice, 32), std::invalid_argument);
  std::vector<HotParamField> decimal = fields();
  decimal[1].type = ParamType::Decimal;
  CHECK_THROWS_AS(HotParamTable(decimal, 32), std::invalid_argument);
}

TEST_CASE("hot params: a publisher validates against the table and pushes ParamUpdate messages") {
  const HotParamTable table(fields(), 32);
  MsgRing ring(1U << 16);
  const std::vector<std::uint8_t> initial = block(5.0, 2, true);
  const std::unique_ptr<ParamPublisher> pub = table.publisher(ParamSink::to_ring(ring), initial, 2);

  REQUIRE(pub->publish({{"levels", "3"}, {"half_spread", "6.5"}}));
  const ParamUpdateMsg& m = front(ring);
  CHECK(m.all_instruments());
  REQUIRE(m.count == 2);
  CHECK(m.field[0] == 1);
  CHECK(m.value[0] == 3);
  CHECK(m.field[1] == 0);
  CHECK(m.value[1] == std::bit_cast<std::int64_t>(6.5));
  ring.release();
  CHECK(pub->describe(InstrumentId{1}).find("half_spread=6.5") != std::string::npos);

  REQUIRE(pub->publish({{"enabled", "false"}}, InstrumentId{1}));
  CHECK(front(ring).hdr.instrument == InstrumentId{1});
  ring.release();

  CHECK(error_of(*pub, {{"nope", "1"}}) == "unknown parameter 'nope'");
  CHECK(error_of(*pub, {{"levels", "0"}}) == "parameter 'levels': value 0 outside [1, 8]");
  CHECK(error_of(*pub, {{"half_spread", "1e11"}}) ==
        "parameter 'half_spread': value 1e11 outside [0, 100]");
  CHECK_THROWS_AS(static_cast<void>(pub->publish({{"levels", "2"}}, InstrumentId{2})),
                  std::invalid_argument);
  CHECK(pub->published() == 2);

  pub->close();
  CHECK_FALSE(pub->publish({{"levels", "2"}}));
}

TEST_CASE("hot params: apply_param_update writes the parameter block the next call copies") {
  using Harness = sim::StrategyHarness<HotStrategy>;
  Harness h;
  HotProgram p;
  p.hooks[static_cast<std::size_t>(HotHook::Book)] = &read_params;
  p.record = block(5.0, 2, true);
  p.param_bytes = 32;
  const HotParamTable table(fields(), 32);
  p.params = table.slots();
  REQUIRE(h.strategy().attach(p, h.engine().instruments()));

  ParamUpdateMsg m{};
  init_header(m, EventType::ParamUpdate, ParamUpdateMsg::kAllInstruments);
  m.count = 3;
  m.field[0] = 0;
  m.value[0] = std::bit_cast<std::int64_t>(0.3);
  m.field[1] = 1;
  m.value[1] = 4;
  m.field[2] = 7;  // no such parameter: skipped
  m.value[2] = 99;
  h.strategy().apply_param_update(m);
  h.book("100.00", "100.02");
  CHECK(g_seen_half_spread == 0.3);
  CHECK(g_seen_raw == 30'000'000);
  CHECK(g_seen_levels == 4);

  init_header(m, EventType::ParamUpdate, InstrumentId{5});  // not in the table: ignored
  m.count = 1;
  m.field[0] = 1;
  m.value[0] = 8;
  h.strategy().apply_param_update(m);
  h.book("100.01", "100.03");
  CHECK(g_seen_levels == 4);

  HotProgram bad = p;
  bad.params[1].offset = 30;  // an int64 past the 32-byte block
  HotStrategy other;
  CHECK_FALSE(other.attach(bad, h.engine().instruments()));
}
