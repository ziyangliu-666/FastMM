// StrategyRegistry::try_add outcomes and lookups, on a local registry with fake factories.
#include "fastmm/strategies/registry.hpp"

#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/listing.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace fastmm;

namespace {
std::unique_ptr<IEngineRunner> fake_factory(TransportKind, RunnerDeps&) {
  return nullptr;
}
std::unique_ptr<IEngineRunner> other_factory(TransportKind, RunnerDeps&) {
  return nullptr;
}
}  // namespace

TEST_CASE("strategies.registry: try_add reports added, already present, conflict and invalid") {
  StrategyRegistry reg;
  const ParamSchema* schema = &BasicMM::schema();
  CHECK(reg.try_add("basic_mm", schema, TransportKind::Live, fake_factory) == AddResult::Added);
  // The same registration again changes nothing.
  CHECK(reg.try_add("basic_mm", schema, TransportKind::Live, fake_factory) ==
        AddResult::AlreadyPresent);
  // Another factory for the same kind would replace the first one.
  CHECK(reg.try_add("basic_mm", schema, TransportKind::Live, other_factory) == AddResult::Conflict);
  CHECK(reg.find("basic_mm")->factories[static_cast<std::size_t>(TransportKind::Live)] ==
        &fake_factory);
  // Another kind of the same strategy is added next to it.
  CHECK(reg.try_add("basic_mm", schema, TransportKind::Replay, other_factory) == AddResult::Added);
  // The same name with a different schema is another strategy.
  CHECK(reg.try_add("basic_mm", &AvellanedaStoikov::schema(), TransportKind::Sim, fake_factory) ==
        AddResult::Conflict);
  CHECK_FALSE(reg.find("basic_mm")->supports(TransportKind::Sim));
  // Invalid input changes nothing.
  CHECK(reg.try_add("x", schema, TransportKind::Count, fake_factory) == AddResult::Invalid);
  CHECK(reg.try_add("x", schema, TransportKind::Sim, nullptr) == AddResult::Invalid);
  CHECK(reg.try_add("x", nullptr, TransportKind::Sim, fake_factory) == AddResult::Invalid);
  CHECK(reg.try_add("", schema, TransportKind::Sim, fake_factory) == AddResult::Invalid);
  CHECK(reg.find("x") == nullptr);
  CHECK(reg.entries().size() == 1);
  CHECK(to_string(AddResult::Conflict) == "conflict");
  CHECK(to_string(TransportKind::Live) == "live");
}

TEST_CASE("strategies.registry: lookup, support per kind and make") {
  StrategyRegistry reg;
  REQUIRE(reg.try_add(BasicMM::name(), &BasicMM::schema(), TransportKind::Live, fake_factory) ==
          AddResult::Added);
  const StrategyEntry* e = reg.find("basic_mm");
  REQUIRE(e != nullptr);
  CHECK(e->schema == &BasicMM::schema());
  CHECK(e->supports(TransportKind::Live));
  CHECK_FALSE(e->supports(TransportKind::Sim));
  CHECK_FALSE(e->supports(TransportKind::Count));
  CHECK(reg.find("nope") == nullptr);
  RunnerDeps deps;
  CHECK(reg.make("basic_mm", TransportKind::Live, deps) == nullptr);  // the fake factory's result
  CHECK(reg.make("basic_mm", TransportKind::Sim, deps) == nullptr);   // not registered
  CHECK(reg.make("nope", TransportKind::Live, deps) == nullptr);
}

TEST_CASE("strategies.registry: the built-in module registers every strategy for every transport") {
  StrategyRegistry reg;
  register_builtin_strategies(reg);
  const std::size_t n = reg.entries().size();
  CHECK(n == 3);
  register_builtin_strategies(reg);  // idempotent
  CHECK(reg.entries().size() == n);
  for (const char* name : {"basic_mm", "avellaneda_stoikov", "options_mm"}) {
    const StrategyEntry* e = reg.find(name);
    REQUIRE_MESSAGE(e != nullptr, name);
    CHECK(e->supports(TransportKind::Sim));
    CHECK(e->supports(TransportKind::Replay));
    CHECK(e->supports(TransportKind::Live));
  }
  // A Live factory refuses a missing backend instead of dereferencing it.
  RunnerDeps deps;
  CHECK(reg.make("basic_mm", TransportKind::Live, deps) == nullptr);
  CHECK(reg.make("basic_mm", TransportKind::Sim, deps) == nullptr);
}

TEST_CASE("strategies.listing: text and JSON strategy lists") {
  StrategyRegistry reg;
  REQUIRE(reg.try_add(BasicMM::name(), &BasicMM::schema(), TransportKind::Sim, fake_factory) ==
          AddResult::Added);
  REQUIRE(reg.try_add(BasicMM::name(), &BasicMM::schema(), TransportKind::Live, fake_factory) ==
          AddResult::Added);
  const std::string text = format_strategies(reg, TransportKind::Sim, ListFormat::Text);
  CHECK(text.rfind("basic_mm\n  half_spread_bps", 0) == 0);
  CHECK(format_strategies(reg, TransportKind::Replay, ListFormat::Text).empty());
  const std::string json = format_strategies(reg, TransportKind::Live, ListFormat::Json);
  CHECK(json.rfind(
            "{\"strategies\": [\n  {\"name\": \"basic_mm\", \"transports\": [\"sim\", \"live\"]",
            0) == 0);
  CHECK(
      json.find("{\"name\": \"half_spread_bps\", \"type\": \"bps\", \"default\": 5, \"min\": 0") !=
      std::string::npos);
  CHECK(format_strategies(reg, TransportKind::Replay, ListFormat::Json) ==
        "{\"strategies\": []}\n");
  CHECK(parse_list_format("json") == ListFormat::Json);
  CHECK(parse_list_format("text") == ListFormat::Text);
  CHECK_FALSE(parse_list_format("yaml").has_value());
}
