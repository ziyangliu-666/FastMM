#include "fastmm/strategies/params.hpp"

#include "test_support.hpp"

using namespace fastmm;

namespace {
struct P {
  FASTMM_PARAMS(P)
  FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
  FASTMM_PARAM(int, levels, 1, 1, 8, "levels")
  FASTMM_PARAM(bool, flag, false, 0, 1, "a flag")
};
}  // namespace

TEST_CASE("strategies.params: schema collection, apply, describe") {
  const ParamSchema& s = P::schema();
  REQUIRE(s.size() == 3);
  CHECK(std::string(s.find("gamma")->doc) == "risk aversion");
  CHECK(s.find("levels")->type == ParamType::Int);
  CHECK(s.find("flag")->type == ParamType::Bool);
  CHECK(s.find("nope") == nullptr);
  P p;
  CHECK(p.gamma == doctest::Approx(0.1));
  CHECK_FALSE(p.apply({{"gamma", "0.5"}, {"levels", "3"}, {"flag", "true"}}));
  CHECK(p.gamma == doctest::Approx(0.5));
  CHECK(p.levels == 3);
  CHECK(p.flag);
  CHECK(p.apply({{"gamma", "11"}}).value().find("outside") != std::string::npos);
  CHECK(p.apply({{"levels", "2.5"}}).value().find("cannot parse") != std::string::npos);
  CHECK(p.apply({{"flag", "maybe"}}).value().find("cannot parse") != std::string::npos);
  CHECK(p.apply({{"unknown", "1"}}).value().find("unknown parameter") != std::string::npos);
  CHECK(p.describe() == "gamma=0.500000 levels=3 flag=true");
  CHECK(s.find("gamma")->get(&p) == doctest::Approx(0.5));
  static_assert(sizeof(P) ==
                sizeof(double) + sizeof(int) + sizeof(bool) + 3);  // registrars take no space
}
