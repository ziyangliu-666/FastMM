#pragma once
// PyStrategy (ADR-0012, section 7): runs a fastmm.Strategy subclass inside the C++ engine.
//
// PyStrategy is an ordinary StrategyLike class, so Engine<PyStrategy, SimClock, SimTransport,
// InlineFeed> is the same engine, risk, OMS, QuoteManager, journal and outbound hash a C++ strategy
// uses. Every hook is compiled in with the C++ signature; a bitmask set when the run starts says
// which hooks the Python class defines, so an undefined hook costs one branch and never touches
// Python. The hooks forward to PyRun (bind_strategy_api.cpp), which holds the bound methods, one
// reusable view object per instrument and per event type, and calls Python with vectorcall. The GIL
// is held for the whole run.
//
// Compiled only into fastmm._core: fastmm::backtest and the apps never instantiate it.
#include "bind_common.hpp"

#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fastmm::py_bind {

class PyRun;

// Bridge entry points (bind_strategy_api.cpp). Each runs the Python hook of the same name.
void py_on_start(PyRun& run) noexcept;
void py_on_stop(PyRun& run) noexcept;
void py_on_book(PyRun& run, InstrumentId id, const L2Book<256>& book) noexcept;
void py_on_book_ticker(PyRun& run, InstrumentId id, const BookTickerMsg& m) noexcept;
void py_on_trade(PyRun& run, InstrumentId id, const TradeMsg& m) noexcept;
void py_on_option_ticker(PyRun& run, InstrumentId id, const OptionTickerMsg& m) noexcept;
void py_on_fill(PyRun& run, const Fill& fill) noexcept;
void py_on_order_update(PyRun& run, const OmsUpdate& u) noexcept;
void py_on_timer(PyRun& run, TimerId id, std::uint64_t tag) noexcept;
void py_on_connection(PyRun& run, const ConnectionStateMsg& m) noexcept;
void py_on_quoting(PyRun& run, bool enabled) noexcept;
// Called every kDriverStepsPerCheck driver steps: signal check and a brief GIL release, so Ctrl-C
// also works for a strategy whose hooks rarely run.
void py_on_driver_steps(PyRun& run) noexcept;

[[nodiscard]] constexpr std::uint32_t hook_bit(Hook h) noexcept {
  return std::uint32_t{1} << static_cast<unsigned>(h);
}

class PyStrategy {
 public:
  static constexpr std::uint64_t kDriverStepsPerCheck = 4096;

  static constexpr std::string_view name() noexcept { return "python"; }
  // The parameters live in Python (fastmm.Param); the engine sees none.
  static const ParamSchema& schema() {
    static const ParamSchema s{};
    return s;
  }
  std::optional<std::string> configure(const ParamMap&) { return std::nullopt; }

  // `mask`: hook_bit() of every hook the Python class defines.
  void attach(PyRun* run, std::uint32_t mask) noexcept {
    run_ = run;
    mask_ = run == nullptr ? 0 : mask;
  }
  // After a failure every hook is a no-op.
  void disable() noexcept { mask_ = 0; }
  [[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }

  template <class Ctx>
  void on_start(Ctx&) noexcept {
    if (on(Hook::Start)) py_on_start(*run_);
  }
  template <class Ctx>
  void on_stop(Ctx&) noexcept {
    if (on(Hook::Stop)) py_on_stop(*run_);
  }
  template <class Ctx, class Book>
  void on_book(Ctx&, InstrumentId id, const Book& book) noexcept {
    if (on(Hook::Book)) py_on_book(*run_, id, book);
  }
  template <class Ctx>
  void on_book_ticker(Ctx&, InstrumentId id, const BookTickerMsg& m) noexcept {
    if (on(Hook::BookTicker)) py_on_book_ticker(*run_, id, m);
  }
  template <class Ctx>
  void on_trade(Ctx&, InstrumentId id, const TradeMsg& m) noexcept {
    if (on(Hook::Trade)) py_on_trade(*run_, id, m);
  }
  template <class Ctx>
  void on_option_ticker(Ctx&, InstrumentId id, const OptionTickerMsg& m) noexcept {
    if (on(Hook::OptionTicker)) py_on_option_ticker(*run_, id, m);
  }
  template <class Ctx>
  void on_fill(Ctx&, const Fill& fill) noexcept {
    if (on(Hook::Fill)) py_on_fill(*run_, fill);
  }
  template <class Ctx>
  void on_order_update(Ctx&, const OmsUpdate& u) noexcept {
    if (on(Hook::OrderUpdate)) py_on_order_update(*run_, u);
  }
  template <class Ctx>
  void on_timer(Ctx&, TimerId id, std::uint64_t tag) noexcept {
    if (on(Hook::Timer)) py_on_timer(*run_, id, tag);
  }
  template <class Ctx>
  void on_connection(Ctx&, const ConnectionStateMsg& m) noexcept {
    if (on(Hook::Connection)) py_on_connection(*run_, m);
  }
  template <class Ctx>
  void on_quoting(Ctx&, bool enabled) noexcept {
    if (on(Hook::Quoting)) py_on_quoting(*run_, enabled);
  }

  // SimDriver calls this through EngineHooks::stopped after every engine step.
  void on_driver_step() noexcept {
    if (run_ != nullptr && (++steps_ % kDriverStepsPerCheck) == 0) py_on_driver_steps(*run_);
  }

 private:
  [[nodiscard]] bool on(Hook h) const noexcept { return (mask_ & hook_bit(h)) != 0; }

  PyRun* run_ = nullptr;
  std::uint32_t mask_ = 0;
  std::uint64_t steps_ = 0;
};

static_assert(StrategyLike<PyStrategy>);
static_assert(verify_strategy<PyStrategy>());

using PySimEngine = Engine<PyStrategy, SimClock, sim::SimTransport, InlineFeed>;

}  // namespace fastmm::py_bind
