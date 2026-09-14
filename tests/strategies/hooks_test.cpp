// The hook checker's traits (strategies/hooks.hpp). The static_assert messages themselves are
// covered by tests/compile_fail; here every status is pinned without failing the build.
#include "fastmm/strategies/hooks.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/options_mm.hpp"

#include <cstdint>
#include <string_view>

using namespace fastmm;
using detail::hooks::StandInBook;
using detail::hooks::StandInContext;

namespace {

template <class S>
constexpr HookStatus status(Hook h) {
  return hook_status<S, StandInContext, StandInBook>(h);
}

// Every hook, half of them in `template <class Ctx>` spelling.
struct AllHooks {
  template <class Ctx>
  void on_start(Ctx&) noexcept {}
  void on_stop(auto&) noexcept {}
  template <class Ctx, class Book>
  void on_book(Ctx&, InstrumentId, const Book&) noexcept {}
  void on_book_ticker(auto&, InstrumentId, const BookTickerMsg&) noexcept {}
  template <class Ctx>
  void on_trade(Ctx&, InstrumentId, const TradeMsg&) noexcept {}
  void on_option_ticker(auto&, InstrumentId, const OptionTickerMsg&) noexcept {}
  template <class Ctx>
  void on_fill(Ctx&, const Fill&) noexcept {}
  void on_order_update(auto&, const OmsUpdate&) noexcept {}
  template <class Ctx>
  void on_timer(Ctx&, TimerId, std::uint64_t) noexcept {}
  void on_connection(auto&, const ConnectionStateMsg&) noexcept {}
  template <class Ctx>
  void on_quoting(Ctx&, bool) noexcept {}
};
static_assert(verify_strategy<AllHooks>());

struct NoHooks {};
static_assert(verify_strategy<NoHooks>());

struct OldTrade {
  void on_trade(auto&, const TradeMsg&) noexcept {}
};
struct PrivateBook {
 private:
  void on_book(auto&, InstrumentId, const auto&) noexcept {}
};
struct DataMember {
  int on_timer = 0;
};
struct NonVoid {
  bool on_quoting(auto&, bool) noexcept { return true; }
};
struct FinalWrong final {
  void on_trade(auto&, const TradeMsg&) noexcept {}
};
struct FinalRight final {
  void on_trade(auto&, InstrumentId, const TradeMsg&) noexcept {}
};
struct Converting {
  void on_timer(auto&, TimerId, int) noexcept {}  // documented: conversions are accepted
};
struct WithHelper {  // a correct hook next to a same-name helper overload
  void on_fill(auto&, const Fill&) noexcept {}
  void on_fill(int) noexcept {}
};
struct Base {
  void on_connection(auto&, const ConnectionStateMsg&) noexcept {}
};
struct Derived : Base {};
struct NearMiss {
  void on_fills(auto&, const Fill&) noexcept {}
};
struct NearMissAllowed {
  static constexpr bool fastmm_allow_near_miss_names = true;
  void on_tick() noexcept {}
};
static_assert(verify_strategy<NearMissAllowed>());

static_assert(status<AllHooks>(Hook::Start) == HookStatus::Ok);
static_assert(status<AllHooks>(Hook::Quoting) == HookStatus::Ok);
static_assert(status<NoHooks>(Hook::Fill) == HookStatus::Absent);
static_assert(status<OldTrade>(Hook::Trade) == HookStatus::Mismatch);
static_assert(status<OldTrade>(Hook::Book) == HookStatus::Absent);
static_assert(status<PrivateBook>(Hook::Book) == HookStatus::Mismatch);
static_assert(status<DataMember>(Hook::Timer) == HookStatus::Mismatch);
static_assert(status<NonVoid>(Hook::Quoting) == HookStatus::Mismatch);
static_assert(status<FinalWrong>(Hook::Trade) == HookStatus::Absent);  // documented limit
static_assert(status<FinalRight>(Hook::Trade) == HookStatus::Ok);
static_assert(status<Converting>(Hook::Timer) == HookStatus::Ok);
static_assert(status<WithHelper>(Hook::Fill) == HookStatus::Ok);
static_assert(status<Derived>(Hook::Connection) == HookStatus::Ok);
static_assert(detail::hooks::declares_on_fills<NearMiss>);
static_assert(!detail::hooks::declares_on_fill<NearMiss>);
static_assert(!detail::hooks::allows_near_miss_names<NearMiss>);
static_assert(detail::hooks::allows_near_miss_names<NearMissAllowed>);
static_assert(!detail::hooks::declares_on_start<int>);

template <class S>
std::string_view words() {
  return implemented_hooks<S, StandInContext, StandInBook>();
}

}  // namespace

TEST_CASE("strategies.hooks: implemented hook sets, table and built-in strategies") {
  CHECK(words<NoHooks>().empty());
  CHECK(words<AllHooks>() ==
        "start stop book book_ticker trade option_ticker fill order_update "
        "timer connection quoting");
  CHECK(words<BasicMM>() == "start book fill timer connection quoting");
  CHECK(words<AvellanedaStoikov>() == "start book trade fill connection quoting");
  CHECK(words<OptionsMM>() == "start book option_ticker fill timer connection quoting");
  REQUIRE(kHooks.size() == static_cast<std::size_t>(Hook::Count));
  for (std::size_t i = 0; i < kHooks.size(); ++i) {
    CAPTURE(kHooks[i].name);
    CHECK(static_cast<std::size_t>(kHooks[i].hook) == i);
    CHECK(kHooks[i].signature.starts_with("void " + std::string(kHooks[i].name) + "(auto& ctx"));
  }
  CHECK(kHooks[static_cast<std::size_t>(Hook::Trade)].signature ==
        "void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)");
}
