#pragma once
// Strategy hooks (ADR-0012): the hook table, the compile-time checker and the Fill view.
//
// A strategy is a plain class. Its hooks are ordinary public member functions, all optional:
//
//   on_start(ctx)  on_stop(ctx)  on_book(ctx, id, book)  on_book_ticker(ctx, id, m)
//   on_trade(ctx, id, m)  on_option_ticker(ctx, id, m)  on_fill(ctx, fill)
//   on_order_update(ctx, u)  on_timer(ctx, timer_id, tag)  on_connection(ctx, m)
//   on_quoting(ctx, enabled)  on_params(ctx)
//
// The engine checks every hook name when it is instantiated. If the strategy has a member with a
// hook's name (function, template, data member, static, inherited or private) and the engine's
// call does not compile, the build stops with one error per hook:
//
//   fastmm: on_trade has the wrong signature or is not public; expected
//   void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)
//
// A few likely misspellings (on_fills, on_tick, onBook, ...) produce a deprecation warning instead,
// because a private helper may use such a name; `static constexpr bool
// fastmm_allow_near_miss_names = true;` in the strategy silences it. Strategy headers can run the
// same check early with stand-in context and book types:
//
//   static_assert(fastmm::verify_strategy<MyMM>());
//
// Limits:
//   * a `final` class loses the name probe; only the call check remains, so a wrong signature is
//     silently not called;
//   * a hook that is ambiguous across two bases is reported as a wrong signature;
//   * a hook with a deduced (`auto`) return type must not be checked with verify_strategy: the
//     stand-in types instantiate its body;
//   * private hooks are rejected, also when the class befriends the engine;
//   * implicit argument conversions are accepted (on_timer(auto&, TimerId, int) compiles);
//   * near-miss warnings are emitted inside this header, so a build that includes FastMM as a
//     system header does not show them.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/timer_wheel.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace fastmm {

// One execution as a strategy sees it (on_fill). The engine builds it on its stack after the
// position and fees are updated. The pointers are valid only during the call; do not store them.
struct Fill {
  InstrumentId instrument;  // the order's instrument, else the fill message's
  Side side{};              // the order's side, else the fill message's
  Price price;              // this execution
  Qty qty;
  Qty position_delta;  // signed change of the position; differs from qty when the commission is
                       // charged in the base asset
  Notional fee;        // quote currency, as booked
  bool fee_converted = true;  // false: commission in a third asset, not booked (counted in stats)
  Liquidity liquidity{};
  bool known = false;       // the id matched an order, open or recently terminal (`update->order`)
  bool late = false;        // the order was already terminal when the fill arrived
  bool order_done = false;  // the order reached a terminal state with this fill
  const OmsUpdate* update = nullptr;  // non-null when known
  const OrderFillMsg* msg = nullptr;  // always set by the engine
};

// The hook table. X(Enum, name, word, (parameters), (arguments), "expected signature"). The
// parameters name the engine's call; `Ctx` and `Book` are the engine's context and book types.
#define FASTMM_STRATEGY_HOOKS(X)                                                   \
  X(Start, on_start, "start", (Ctx & ctx), (ctx), "void on_start(auto& ctx)")      \
  X(Stop, on_stop, "stop", (Ctx & ctx), (ctx), "void on_stop(auto& ctx)")          \
  X(Book,                                                                          \
    on_book,                                                                       \
    "book",                                                                        \
    (Ctx & ctx, InstrumentId id, const Book& book),                                \
    (ctx, id, book),                                                               \
    "void on_book(auto& ctx, InstrumentId id, const auto& book)")                  \
  X(BookTicker,                                                                    \
    on_book_ticker,                                                                \
    "book_ticker",                                                                 \
    (Ctx & ctx, InstrumentId id, const BookTickerMsg& m),                          \
    (ctx, id, m),                                                                  \
    "void on_book_ticker(auto& ctx, InstrumentId id, const BookTickerMsg& m)")     \
  X(Trade,                                                                         \
    on_trade,                                                                      \
    "trade",                                                                       \
    (Ctx & ctx, InstrumentId id, const TradeMsg& m),                               \
    (ctx, id, m),                                                                  \
    "void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)")                \
  X(OptionTicker,                                                                  \
    on_option_ticker,                                                              \
    "option_ticker",                                                               \
    (Ctx & ctx, InstrumentId id, const OptionTickerMsg& m),                        \
    (ctx, id, m),                                                                  \
    "void on_option_ticker(auto& ctx, InstrumentId id, const OptionTickerMsg& m)") \
  X(Fill,                                                                          \
    on_fill,                                                                       \
    "fill",                                                                        \
    (Ctx & ctx, const Fill& fill),                                                 \
    (ctx, fill),                                                                   \
    "void on_fill(auto& ctx, const Fill& fill)")                                   \
  X(OrderUpdate,                                                                   \
    on_order_update,                                                               \
    "order_update",                                                                \
    (Ctx & ctx, const OmsUpdate& u),                                               \
    (ctx, u),                                                                      \
    "void on_order_update(auto& ctx, const OmsUpdate& u)")                         \
  X(Timer,                                                                         \
    on_timer,                                                                      \
    "timer",                                                                       \
    (Ctx & ctx, TimerId id, std::uint64_t tag),                                    \
    (ctx, id, tag),                                                                \
    "void on_timer(auto& ctx, TimerId id, std::uint64_t tag)")                     \
  X(Connection,                                                                    \
    on_connection,                                                                 \
    "connection",                                                                  \
    (Ctx & ctx, const ConnectionStateMsg& m),                                      \
    (ctx, m),                                                                      \
    "void on_connection(auto& ctx, const ConnectionStateMsg& m)")                  \
  X(Quoting,                                                                       \
    on_quoting,                                                                    \
    "quoting",                                                                     \
    (Ctx & ctx, bool enabled),                                                     \
    (ctx, enabled),                                                                \
    "void on_quoting(auto& ctx, bool enabled)")                                    \
  X(Params, on_params, "params", (Ctx & ctx), (ctx), "void on_params(auto& ctx)")

// Y(misspelling, hook): names that produce a "did you mean" warning.
#define FASTMM_STRATEGY_HOOK_NEAR_MISSES(Y) \
  Y(on_fills, on_fill)                      \
  Y(on_execution, on_fill)                  \
  Y(on_trades, on_trade)                    \
  Y(on_order_book, on_book)                 \
  Y(on_orderbook, on_book)                  \
  Y(on_book_update, on_book)                \
  Y(on_depth, on_book)                      \
  Y(on_tick, on_book)                       \
  Y(on_bbo, on_book_ticker)                 \
  Y(on_ticker, on_book_ticker)              \
  Y(on_options_ticker, on_option_ticker)    \
  Y(on_order, on_order_update)              \
  Y(on_order_event, on_order_update)        \
  Y(on_timer_fired, on_timer)               \
  Y(on_connection_state, on_connection)     \
  Y(on_disconnect, on_connection)           \
  Y(on_init, on_start)                      \
  Y(on_shutdown, on_stop)                   \
  Y(on_quote, on_quoting)                   \
  Y(on_param, on_params)                    \
  Y(on_parameters, on_params)               \
  Y(onBook, on_book)                        \
  Y(OnBook, on_book)                        \
  Y(onTrade, on_trade)                      \
  Y(onFill, on_fill)

enum class Hook : std::uint8_t {
#define FASTMM_HOOK_ENUM(E, name, word, params, args, sig) E,
  FASTMM_STRATEGY_HOOKS(FASTMM_HOOK_ENUM)
#undef FASTMM_HOOK_ENUM
      Count
};

// Absent: no member with the hook's name. Ok: the engine calls it. Mismatch: a member with the
// name exists but the call does not compile (a build error through verify_strategy).
enum class HookStatus : std::uint8_t { Absent, Ok, Mismatch };

struct HookInfo {
  Hook hook;
  std::string_view name;       // "on_trade"
  std::string_view word;       // "trade" (start-up log)
  std::string_view signature;  // "void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)"
};

inline constexpr std::array<HookInfo, static_cast<std::size_t>(Hook::Count)> kHooks{{
#define FASTMM_HOOK_INFO(E, name, word, params, args, sig) {Hook::E, #name, word, sig},
    FASTMM_STRATEGY_HOOKS(FASTMM_HOOK_INFO)
#undef FASTMM_HOOK_INFO
}};

namespace detail::hooks {

#define FASTMM_HOOK_UNPAREN(...) __VA_ARGS__

// Name probe: lookup of `name` in name_probe<S> is ambiguous exactly when S has any member called
// `name` (function, template, data, static, inherited, private); an ambiguous lookup is a
// substitution failure, so the requires-expression is false. The conjunction keeps final and
// non-class types away from the derivation.
template <class S>
concept probeable = std::is_class_v<S> && !std::is_final_v<S>;

#define FASTMM_HOOK_PROBE(name)                  \
  struct probe_##name {                          \
    void name();                                 \
  };                                             \
  template <class S>                             \
  struct name_probe_##name : S, probe_##name {}; \
  template <class S>                             \
  concept declares_##name = probeable<S> && (!requires { &name_probe_##name<S>::name; });

#define FASTMM_HOOK_CALL(E, name, word, params, args, sig)            \
  FASTMM_HOOK_PROBE(name)                                             \
  template <class S, class Ctx, class Book>                           \
  concept calls_##name = requires(S& s, FASTMM_HOOK_UNPAREN params) { \
    { s.name args } -> std::same_as<void>;                            \
  };
FASTMM_STRATEGY_HOOKS(FASTMM_HOOK_CALL)
#undef FASTMM_HOOK_CALL

#define FASTMM_HOOK_NEAR_MISS(name, hook)                                      \
  FASTMM_HOOK_PROBE(name)                                                      \
  template <bool Found>                                                        \
  struct near_miss_##name {                                                    \
    static constexpr bool value = false;                                       \
  };                                                                           \
  template <>                                                                  \
  struct near_miss_##name<true> {                                              \
    [[deprecated("fastmm: " #name " is not a hook; did you mean " #hook "?")]] \
    static constexpr bool value = true;                                        \
  };
FASTMM_STRATEGY_HOOK_NEAR_MISSES(FASTMM_HOOK_NEAR_MISS)
#undef FASTMM_HOOK_NEAR_MISS
#undef FASTMM_HOOK_PROBE

template <class S>
concept allows_near_miss_names = requires { requires S::fastmm_allow_near_miss_names; };

// Stand-ins for verify_strategy<S>() in a strategy header, where the engine types are unknown.
struct StandInContext {};
struct StandInBook {};

}  // namespace detail::hooks

template <class S, class Ctx, class Book>
[[nodiscard]] constexpr HookStatus hook_status(Hook h) noexcept {
  switch (h) {
#define FASTMM_HOOK_STATUS(E, name, word, params, args, sig)              \
  case Hook::E:                                                           \
    if (detail::hooks::calls_##name<S, Ctx, Book>) return HookStatus::Ok; \
    return detail::hooks::declares_##name<S> ? HookStatus::Mismatch : HookStatus::Absent;
    FASTMM_STRATEGY_HOOKS(FASTMM_HOOK_STATUS)
#undef FASTMM_HOOK_STATUS
    case Hook::Count:
      break;
  }
  return HookStatus::Absent;
}

// True when the engine calls S's hook `h` (the engine dispatches on this at compile time).
template <class S, class Ctx, class Book>
[[nodiscard]] constexpr bool implements_hook(Hook h) noexcept {
  return hook_status<S, Ctx, Book>(h) == HookStatus::Ok;
}

// Compile-time check of every hook of S; always returns true (failures are static_asserts), so it
// can be written as `static_assert(fastmm::verify_strategy<MyMM>());`.
template <class S,
          class Ctx = detail::hooks::StandInContext,
          class Book = detail::hooks::StandInBook>
[[nodiscard]] constexpr bool verify_strategy() noexcept {
  // The local names make clang's "due to requirement '...'" line readable.
#define FASTMM_HOOK_ASSERT(E, name, word, params, args, sig)                       \
  const bool name##_declared = detail::hooks::declares_##name<S>;                  \
  const bool name##_signature_matches = detail::hooks::calls_##name<S, Ctx, Book>; \
  static_assert(!name##_declared || name##_signature_matches,                      \
                "fastmm: " #name " has the wrong signature or is not public; expected " sig);
  FASTMM_STRATEGY_HOOKS(FASTMM_HOOK_ASSERT)
#undef FASTMM_HOOK_ASSERT
  const bool near_miss_names_allowed = detail::hooks::allows_near_miss_names<S>;
#define FASTMM_HOOK_WARN(name, hook)                                              \
  static_cast<void>(detail::hooks::near_miss_##name < !near_miss_names_allowed && \
                    detail::hooks::declares_##name < S >> ::value);
  FASTMM_STRATEGY_HOOK_NEAR_MISSES(FASTMM_HOOK_WARN)
#undef FASTMM_HOOK_WARN
  return true;
}

namespace detail::hooks {
struct HookSetText {
  std::array<char, 128> chars{};
  std::size_t size = 0;
};
template <class S, class Ctx, class Book>
inline constexpr HookSetText hook_set_text = [] {
  HookSetText t;
  for (const HookInfo& h : kHooks) {
    if (!implements_hook<S, Ctx, Book>(h.hook)) continue;
    if (t.size != 0) t.chars[t.size++] = ' ';
    for (const char c : h.word) t.chars[t.size++] = c;
  }
  return t;
}();
}  // namespace detail::hooks

// The implemented hooks as words in table order ("start book fill timer connection quoting").
template <class S, class Ctx, class Book>
[[nodiscard]] constexpr std::string_view implemented_hooks() noexcept {
  const detail::hooks::HookSetText& t = detail::hooks::hook_set_text<S, Ctx, Book>;
  return std::string_view(t.chars.data(), t.size);
}

}  // namespace fastmm
