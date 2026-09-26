// Python strategy API (ADR-0012, section 7): the PyRun bridge behind PyStrategy, the Context and
// the event views a fastmm.Strategy receives, and the value types (Instrument, Order, Portfolio).
//
// Views are created once per run (one per instrument for books and positions, one per event type
// otherwise) and repointed before every hook. Each view remembers the epoch it was stamped with;
// the epoch advances when an outermost hook starts and when it returns, so a view read outside the
// callback that received it raises StaleViewError after one integer compare. The context checks
// that a hook is running on the backtest's thread.
#include "py_strategy.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/core/book/book_features.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/sim/sim_driver.hpp"

#include <pybind11/stl.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::py_bind {

namespace {

using Book = L2Book<256>;

constexpr std::size_t kHookCount = static_cast<std::size_t>(Hook::Count);
constexpr std::uint64_t kHooksPerCheck = 1024;  // signal check + GIL release interval
constexpr double kMaxFixed = 9.2e10;            // largest magnitude a 1e-8 fixed value holds

// Exception types created at import (module attributes of fastmm._core).
PyObject* g_stale_view_error = nullptr;
PyObject* g_order_rejected = nullptr;

[[noreturn]] void raise_stale() {
  PyErr_SetString(g_stale_view_error,
                  "fastmm: view read outside the hook that received it (views are reused "
                  "between events; copy the values you need)");
  throw py::error_already_set();
}

[[noreturn]] void raise_rejected(RejectReason r) {
  const std::string reason(to_string(r));
  py::object exc =
      py::reinterpret_borrow<py::object>(g_order_rejected)("order rejected: " + reason);
  exc.attr("reason") = reason;
  PyErr_SetObject(g_order_rejected, exc.ptr());
  throw py::error_already_set();
}

// Shared by the context and every view of one run. Views may outlive the run (a strategy can keep
// a reference), so they hold it by shared_ptr and it never points at freed engine state: `engine`
// is null once the run is over.
struct RunGuard {
  std::uint64_t epoch = 1;  // advances when an outermost hook starts and when it returns
  int depth = 0;            // hooks running
  PySimEngine* engine = nullptr;
  PyThreadState* thread = nullptr;  // the thread running the backtest
};

template <class T>
struct View {
  std::shared_ptr<RunGuard> guard;
  std::uint64_t epoch = 0;
  const T* p = nullptr;
  InstrumentId id{};

  [[nodiscard]] const T& get() const {
    if (FASTMM_UNLIKELY(epoch != guard->epoch)) raise_stale();
    return *p;
  }
  void point(const T& v) noexcept {
    p = &v;
    epoch = guard->epoch;
  }
};

using BookView = View<Book>;
using PositionView = View<Position>;
using TradeView = View<TradeMsg>;
using BookTickerView = View<BookTickerMsg>;
using OptionTickerView = View<OptionTickerMsg>;
using ConnectionView = View<ConnectionStateMsg>;
using OrderUpdateView = View<OmsUpdate>;
struct FillView : View<Fill> {
  PyObject* update = nullptr;  // the run's OrderUpdateView object (borrowed; read after get())
};

// Value types: copies, valid forever.
struct InstrumentInfo {
  Instrument inst;
};
struct OrderInfo {
  Order order;
  OrderTimes times;
};
struct PortfolioInfo {
  Portfolio p;
};

double to_f(std::int64_t raw) noexcept {
  return static_cast<double>(raw) / static_cast<double>(kFixedScale);
}

}  // namespace

// ---- PyRun ----------------------------------------------------------------------------------

class PyRun {
 public:
  PyRun(PySimEngine& engine, PyStrategy& strategy, const py::object& instance, std::uint32_t mask);
  PyRun(const PyRun&) = delete;
  PyRun& operator=(const PyRun&) = delete;
  ~PyRun() = default;

  // Enter / leave one hook call; the epoch advances around the outermost one.
  class Scope {
   public:
    explicit Scope(PyRun& r) noexcept : g_(*r.guard_) {
      if (g_.depth++ == 0) ++g_.epoch;
    }
    ~Scope() {
      if (--g_.depth == 0) ++g_.epoch;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    RunGuard& g_;
  };

  // args[0] is free (PY_VECTORCALL_ARGUMENTS_OFFSET); args[1..n] are the hook's arguments.
  void call(Hook h, PyObject** args, std::size_t n) noexcept {
    const auto i = static_cast<std::size_t>(h);
    PyObject* r =
        PyObject_Vectorcall(fns_[i].ptr(), args + 1, n | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
    if (FASTMM_LIKELY(r != nullptr)) {
      Py_DECREF(r);
    } else {
      fail(kHooks[i].name);
    }
    if ((++calls_ % kHooksPerCheck) == 0) periodic();
  }

  // Stores the pending Python error (the first one wins), pulls all quotes and requests stop.
  void fail(std::string_view where) noexcept;
  // Converts a C++ exception thrown inside the bridge into a stored Python error.
  void fail_current(std::string_view where) noexcept;
  // Ctrl-C and other threads: PyErr_CheckSignals() and a brief GIL release.
  void periodic() noexcept;
  // The run is over: every context call and view read fails from now on.
  void close() noexcept {
    guard_->engine = nullptr;
    guard_->depth = 0;
    ++guard_->epoch;
  }

  [[nodiscard]] bool failed() const noexcept { return static_cast<bool>(error_); }
  [[nodiscard]] py::tuple error_info() const {
    return py::make_tuple(error_, error_hook_, error_ns_, error_events_);
  }

  [[nodiscard]] InstrumentId resolve(py::handle inst) const;

  std::shared_ptr<RunGuard> guard_;
  PySimEngine* engine_;
  PyStrategy* strategy_;
  std::array<py::object, kHookCount> fns_{};
  py::object ctx_;
  std::vector<py::object> inst_objs_;
  py::tuple inst_tuple_;
  std::vector<py::object> book_objs_;
  std::vector<BookView*> book_views_;
  std::vector<py::object> pos_objs_;
  std::vector<PositionView*> pos_views_;
  py::object trade_obj_, ticker_obj_, option_obj_, conn_obj_, fill_obj_, update_obj_;
  TradeView* trade_ = nullptr;
  BookTickerView* ticker_ = nullptr;
  OptionTickerView* option_ = nullptr;
  ConnectionView* conn_ = nullptr;
  FillView* fill_ = nullptr;
  OrderUpdateView* update_ = nullptr;
  std::uint64_t calls_ = 0;

  py::object error_;
  std::string error_hook_;
  std::int64_t error_ns_ = 0;
  std::uint64_t error_events_ = 0;
};

namespace {

struct ContextHandle {
  std::shared_ptr<RunGuard> guard;
  PyRun* run = nullptr;

  [[nodiscard]] PySimEngine& engine() const {
    const RunGuard& g = *guard;
    if (FASTMM_UNLIKELY(g.depth == 0 || g.engine == nullptr || PyThreadState_Get() != g.thread)) {
      throw std::runtime_error(
          "fastmm: the strategy context can only be used inside a hook, on the thread running "
          "the backtest");
    }
    return *g.engine;
  }
};

}  // namespace

PyRun::PyRun(PySimEngine& engine,
             PyStrategy& strategy,
             const py::object& instance,
             std::uint32_t mask)
    : guard_(std::make_shared<RunGuard>()), engine_(&engine), strategy_(&strategy) {
  guard_->engine = &engine;
  guard_->thread = PyThreadState_Get();
  for (const HookInfo& h : kHooks) {
    if ((mask & hook_bit(h.hook)) != 0) {
      fns_[static_cast<std::size_t>(h.hook)] = instance.attr(std::string(h.name).c_str());
    }
  }
  ctx_ = py::cast(ContextHandle{guard_, this});
  py::list insts;
  for (const Instrument& inst : engine.instruments()) {
    inst_objs_.push_back(py::cast(InstrumentInfo{inst}));
    insts.append(inst_objs_.back());
    book_objs_.push_back(py::cast(BookView{guard_, 0, nullptr, inst.id}));
    book_views_.push_back(book_objs_.back().cast<BookView*>());
    pos_objs_.push_back(py::cast(PositionView{guard_, 0, nullptr, inst.id}));
    pos_views_.push_back(pos_objs_.back().cast<PositionView*>());
  }
  inst_tuple_ = py::tuple(insts);
  trade_obj_ = py::cast(TradeView{guard_, 0, nullptr, InstrumentId{}});
  trade_ = trade_obj_.cast<TradeView*>();
  ticker_obj_ = py::cast(BookTickerView{guard_, 0, nullptr, InstrumentId{}});
  ticker_ = ticker_obj_.cast<BookTickerView*>();
  option_obj_ = py::cast(OptionTickerView{guard_, 0, nullptr, InstrumentId{}});
  option_ = option_obj_.cast<OptionTickerView*>();
  conn_obj_ = py::cast(ConnectionView{guard_, 0, nullptr, InstrumentId{}});
  conn_ = conn_obj_.cast<ConnectionView*>();
  update_obj_ = py::cast(OrderUpdateView{guard_, 0, nullptr, InstrumentId{}});
  update_ = update_obj_.cast<OrderUpdateView*>();
  FillView fv;
  fv.guard = guard_;
  fill_obj_ = py::cast(std::move(fv));
  fill_ = fill_obj_.cast<FillView*>();
  fill_->update = update_obj_.ptr();
}

void PyRun::fail(std::string_view where) noexcept {
  if (!error_) {
#if PY_VERSION_HEX >= 0x030C0000
    PyObject* value = PyErr_GetRaisedException();
#else
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    if (tb != nullptr && value != nullptr) PyException_SetTraceback(value, tb);
    Py_XDECREF(type);
    Py_XDECREF(tb);
#endif
    if (value == nullptr) {
      value = PyObject_CallFunction(
          PyExc_RuntimeError, "s", "fastmm: hook failed without an exception");
    }
    error_ = py::reinterpret_steal<py::object>(value);
    error_hook_ = std::string(where);
    error_ns_ = engine_->now().ns;
    error_events_ = engine_->stats().events;
  } else {
    PyErr_Clear();
  }
  if (strategy_->mask() != 0) {
    strategy_->disable();
    engine_->context().pull_all_quotes();
    engine_->context().request_stop();
  }
}

void PyRun::fail_current(std::string_view where) noexcept {
  try {
    throw;
  } catch (py::error_already_set& e) {
    e.restore();
  } catch (const std::exception& e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "fastmm: unknown C++ exception in a strategy hook");
  }
  fail(where);
}

void PyRun::periodic() noexcept {
  if (PyErr_CheckSignals() != 0) fail("signal handler");
  PyThreadState* ts = PyEval_SaveThread();  // lets other Python threads run for a moment
  PyEval_RestoreThread(ts);
}

InstrumentId PyRun::resolve(py::handle inst) const {
  const std::size_t n = inst_objs_.size();
  for (std::size_t i = 0; i < n; ++i) {  // the objects the run hands out: pointer compare
    if (inst.ptr() == inst_objs_[i].ptr()) return InstrumentId{static_cast<std::uint32_t>(i)};
  }
  long long v = -1;
  if (py::isinstance<InstrumentInfo>(inst)) {
    v = inst.cast<const InstrumentInfo&>().inst.id.value;
  } else if (PyIndex_Check(inst.ptr()) != 0 && PyBool_Check(inst.ptr()) == 0) {
    v = py::int_(py::reinterpret_borrow<py::object>(inst)).cast<long long>();
  } else {
    throw py::type_error("fastmm: instrument must be a fastmm.Instrument or an int id, got " +
                         std::string(py::str(py::type::handle_of(inst).attr("__name__"))));
  }
  if (v < 0 || static_cast<std::size_t>(v) >= n) {
    throw py::value_error("fastmm: instrument " + std::to_string(v) +
                          " is not in the instrument table");
  }
  return InstrumentId{static_cast<std::uint32_t>(v)};
}

// ---- bridge entry points ----------------------------------------------------------------------

void py_on_start(PyRun& r) noexcept {
  const PyRun::Scope scope(r);
  PyObject* args[2] = {nullptr, r.ctx_.ptr()};
  r.call(Hook::Start, args, 1);
}

void py_on_stop(PyRun& r) noexcept {
  const PyRun::Scope scope(r);
  PyObject* args[2] = {nullptr, r.ctx_.ptr()};
  r.call(Hook::Stop, args, 1);
}

void py_on_book(PyRun& r, InstrumentId id, const Book& book) noexcept {
  const PyRun::Scope scope(r);
  r.book_views_[id.value]->point(book);
  PyObject* args[4] = {
      nullptr, r.ctx_.ptr(), r.inst_objs_[id.value].ptr(), r.book_objs_[id.value].ptr()};
  r.call(Hook::Book, args, 3);
}

void py_on_book_ticker(PyRun& r, InstrumentId id, const BookTickerMsg& m) noexcept {
  const PyRun::Scope scope(r);
  r.ticker_->point(m);
  r.ticker_->id = id;
  PyObject* args[4] = {nullptr, r.ctx_.ptr(), r.inst_objs_[id.value].ptr(), r.ticker_obj_.ptr()};
  r.call(Hook::BookTicker, args, 3);
}

void py_on_trade(PyRun& r, InstrumentId id, const TradeMsg& m) noexcept {
  const PyRun::Scope scope(r);
  r.trade_->point(m);
  r.trade_->id = id;
  PyObject* args[4] = {nullptr, r.ctx_.ptr(), r.inst_objs_[id.value].ptr(), r.trade_obj_.ptr()};
  r.call(Hook::Trade, args, 3);
}

void py_on_option_ticker(PyRun& r, InstrumentId id, const OptionTickerMsg& m) noexcept {
  const PyRun::Scope scope(r);
  r.option_->point(m);
  r.option_->id = id;
  PyObject* args[4] = {nullptr, r.ctx_.ptr(), r.inst_objs_[id.value].ptr(), r.option_obj_.ptr()};
  r.call(Hook::OptionTicker, args, 3);
}

void py_on_fill(PyRun& r, const Fill& fill) noexcept {
  const PyRun::Scope scope(r);
  r.fill_->point(fill);
  r.fill_->id = fill.instrument;
  if (fill.update != nullptr) r.update_->point(*fill.update);
  PyObject* args[3] = {nullptr, r.ctx_.ptr(), r.fill_obj_.ptr()};
  r.call(Hook::Fill, args, 2);
}

void py_on_order_update(PyRun& r, const OmsUpdate& u) noexcept {
  const PyRun::Scope scope(r);
  r.update_->point(u);
  PyObject* args[3] = {nullptr, r.ctx_.ptr(), r.update_obj_.ptr()};
  r.call(Hook::OrderUpdate, args, 2);
}

void py_on_timer(PyRun& r, TimerId id, std::uint64_t tag) noexcept {
  const PyRun::Scope scope(r);
  PyObject* tid = PyLong_FromUnsignedLong(id.value);
  PyObject* ptag = PyLong_FromUnsignedLongLong(tag);
  if (tid == nullptr || ptag == nullptr) {
    Py_XDECREF(tid);
    Py_XDECREF(ptag);
    r.fail("on_timer");
    return;
  }
  PyObject* args[4] = {nullptr, r.ctx_.ptr(), tid, ptag};
  r.call(Hook::Timer, args, 3);
  Py_DECREF(tid);
  Py_DECREF(ptag);
}

void py_on_connection(PyRun& r, const ConnectionStateMsg& m) noexcept {
  const PyRun::Scope scope(r);
  r.conn_->point(m);
  PyObject* args[3] = {nullptr, r.ctx_.ptr(), r.conn_obj_.ptr()};
  r.call(Hook::Connection, args, 2);
}

void py_on_quoting(PyRun& r, bool enabled) noexcept {
  const PyRun::Scope scope(r);
  PyObject* args[3] = {nullptr, r.ctx_.ptr(), enabled ? Py_True : Py_False};
  r.call(Hook::Quoting, args, 2);
}

void py_on_driver_steps(PyRun& r) noexcept {
  r.periodic();
}

// ---- context helpers ----------------------------------------------------------------------------

namespace {

Side side_from(int side) {
  if (side == 0) return Side::Buy;
  if (side == 1) return Side::Sell;
  throw py::value_error("fastmm: side must be fastmm.BUY (0) or fastmm.SELL (1), got " +
                        std::to_string(side));
}

std::string where(const char* what, Py_ssize_t i, const char* field) {
  return std::string("fastmm: ") + what + "[" + std::to_string(i) + "] " + field;
}

std::int64_t raw_int(PyObject* o, const char* what, Py_ssize_t i, const char* field) {
  if (PyFloat_Check(o) != 0 || PyIndex_Check(o) == 0 || PyBool_Check(o) != 0) {
    throw py::type_error(where(what, i, field) + " must be an int raw value (1e-8 scale); use " +
                         "set_quotes() for floats");
  }
  const py::object idx = py::reinterpret_steal<py::object>(PyNumber_Index(o));
  if (!idx) throw py::error_already_set();
  const long long v = PyLong_AsLongLong(idx.ptr());
  if (v == -1 && PyErr_Occurred() != nullptr) throw py::error_already_set();
  return v;
}

double float_value(PyObject* o, const char* what, Py_ssize_t i, const char* field) {
  const double d = PyFloat_Check(o) != 0 ? PyFloat_AS_DOUBLE(o) : PyFloat_AsDouble(o);
  if (d == -1.0 && PyErr_Occurred() != nullptr) throw py::error_already_set();
  if (!std::isfinite(d) || std::fabs(d) > kMaxFixed) {
    throw py::value_error(where(what, i, field) + " must be finite and within +-9.2e10");
  }
  return d;
}

// bids / asks: a sequence of (price, qty) pairs, level 0 first.
void parse_levels(py::handle seq,
                  Side side,
                  const Instrument& inst,
                  bool raw,
                  DesiredQuotes& q,
                  const char* what) {
  if (seq.is_none()) return;
  PyObject* fast_ptr =
      PySequence_Fast(seq.ptr(), "fastmm: bids and asks must be sequences of (price, qty) pairs");
  if (fast_ptr == nullptr) throw py::error_already_set();
  const py::object fast = py::reinterpret_steal<py::object>(fast_ptr);
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(fast_ptr);
  if (n > static_cast<Py_ssize_t>(kMaxQuoteLevels)) {
    throw py::value_error(std::string("fastmm: ") + what + " has " + std::to_string(n) +
                          " levels; at most " + std::to_string(kMaxQuoteLevels) + " per side");
  }
  PyObject** items = PySequence_Fast_ITEMS(fast_ptr);
  for (Py_ssize_t i = 0; i < n; ++i) {
    PyObject* item = items[i];
    PyObject* px_obj = nullptr;
    PyObject* qty_obj = nullptr;
    py::object pair_keep;
    if (PyTuple_Check(item) != 0 && PyTuple_GET_SIZE(item) == 2) {
      px_obj = PyTuple_GET_ITEM(item, 0);
      qty_obj = PyTuple_GET_ITEM(item, 1);
    } else if (PyList_Check(item) != 0 && PyList_GET_SIZE(item) == 2) {
      px_obj = PyList_GET_ITEM(item, 0);
      qty_obj = PyList_GET_ITEM(item, 1);
    } else {
      PyObject* p = PySequence_Check(item) != 0 ? PySequence_Fast(item, "") : nullptr;
      if (p == nullptr || PySequence_Fast_GET_SIZE(p) != 2) {
        Py_XDECREF(p);
        PyErr_Clear();
        throw py::type_error(std::string("fastmm: ") + what + "[" + std::to_string(i) +
                             "] must be a (price, qty) pair");
      }
      pair_keep = py::reinterpret_steal<py::object>(p);
      px_obj = PySequence_Fast_GET_ITEM(p, 0);
      qty_obj = PySequence_Fast_GET_ITEM(p, 1);
    }
    Price px;
    Qty qty;
    if (raw) {
      px = Price::from_raw(raw_int(px_obj, what, i, "price"));
      qty = Qty::from_raw(raw_int(qty_obj, what, i, "qty"));
    } else {
      const double dp = float_value(px_obj, what, i, "price");
      const double dq = float_value(qty_obj, what, i, "qty");
      if (dq < 0.0) throw py::value_error(where(what, i, "qty") + " is negative");
      px = inst.round_price(Price::from_double(dp), side);
      qty = inst.round_qty(Qty::from_double(dq));
    }
    if (qty.raw < 0) throw py::value_error(where(what, i, "qty") + " is negative");
    // Like DesiredQuotes::bid/ask in C++: a non-positive price or quantity drops the level.
    if (side == Side::Buy) {
      static_cast<void>(q.bid(px, qty));
    } else {
      static_cast<void>(q.ask(px, qty));
    }
  }
}

bool set_quotes(
    const ContextHandle& c, py::handle inst, py::handle bids, py::handle asks, bool raw) {
  PySimEngine& e = c.engine();
  const InstrumentId id = c.run->resolve(inst);
  const Instrument& in = e.instrument(id);
  DesiredQuotes q;
  parse_levels(bids, Side::Buy, in, raw, q, "bids");
  parse_levels(asks, Side::Sell, in, raw, q, "asks");
  return e.context().set_quotes(id, q);
}

Price price_arg(const Instrument& inst, Side side, py::handle v, bool raw) {
  if (raw) return Price::from_raw(raw_int(v.ptr(), "order", 0, "price"));
  return inst.round_price(Price::from_double(float_value(v.ptr(), "order", 0, "price")), side);
}

Qty qty_arg(const Instrument& inst, py::handle v, bool raw) {
  Qty q = raw ? Qty::from_raw(raw_int(v.ptr(), "order", 0, "qty"))
              : inst.round_qty(Qty::from_double(float_value(v.ptr(), "order", 0, "qty")));
  if (q.raw < 0) throw py::value_error("fastmm: order qty is negative");
  return q;
}

std::uint64_t send(const ContextHandle& c,
                   py::handle inst,
                   int side,
                   py::handle price,
                   py::handle qty,
                   bool post_only,
                   bool reduce_only,
                   bool ioc,
                   std::uint32_t tag,
                   bool raw) {
  PySimEngine& e = c.engine();
  const InstrumentId id = c.run->resolve(inst);
  const Instrument& in = e.instrument(id);
  const Side s = side_from(side);
  LimitOrder o = NewOrderRequest::limit(id, s, price_arg(in, s, price, raw), qty_arg(in, qty, raw));
  if (post_only) o = o.post_only();
  if (reduce_only) o = o.reduce_only();
  if (ioc) o = o.ioc();
  const auto res = e.context().send(o.tag(tag));
  if (!res) raise_rejected(res.error());
  return res->value;
}

void replace(
    const ContextHandle& c, std::uint64_t order_id, py::handle price, py::handle qty, bool raw) {
  PySimEngine& e = c.engine();
  const ClientOrderId cid{order_id};
  const Order* o = e.context().order(cid);
  if (o == nullptr) raise_rejected(RejectReason::UnknownOrder);
  const Instrument& in = e.instrument(o->instrument);
  const Price p = price_arg(in, o->side, price, raw);
  const Qty q = qty_arg(in, qty, raw);
  const auto res = e.context().replace(cid, p, q);
  if (!res) raise_rejected(res.error());
}

std::int64_t positive_ns(std::int64_t ns, const char* what) {
  if (ns <= 0) throw py::value_error(std::string("fastmm: ") + what + " must be > 0 nanoseconds");
  return ns;
}

std::uint32_t timer_or_raise(TimerId t) {
  if (!t.valid()) throw std::runtime_error("fastmm: no free timer slot");
  return t.value;
}

py::object order_or_none(PySimEngine& e, const Order* o) {
  if (o == nullptr) return py::none();
  const OrderTimes* t = e.context().order_times(o->cl_ord_id);
  return py::cast(OrderInfo{*o, t != nullptr ? *t : OrderTimes{}});
}

py::object qty_or_none(std::optional<Qty> q, bool raw) {
  if (!q) return py::none();
  if (raw) return py::int_(q->raw);
  return py::float_(q->to_double());
}

py::tuple level_f(Level l) {
  return py::make_tuple(l.price.to_double(), l.qty.to_double());
}
py::tuple level_raw(Level l) {
  return py::make_tuple(l.price.raw, l.qty.raw);
}

// float and raw property pair: name / name_raw.
#define FASTMM_PY_FIXED(cls, V, name, expr)                \
  cls.def_property_readonly(name,                          \
                            [](const V& v) {               \
                              const auto& x = v.get();     \
                              return to_f((expr).raw);     \
                            })                             \
      .def_property_readonly(name "_raw", [](const V& v) { \
        const auto& x = v.get();                           \
        return static_cast<std::int64_t>((expr).raw);      \
      })
#define FASTMM_PY_FIELD(cls, V, name, expr)        \
  cls.def_property_readonly(name, [](const V& v) { \
    const auto& x = v.get();                       \
    return expr;                                   \
  })

#define FASTMM_PY_VALUE_FIXED(cls, V, name, expr)          \
  cls.def_property_readonly(name,                          \
                            [](const V& v) {               \
                              const auto& x = v;           \
                              return to_f((expr).raw);     \
                            })                             \
      .def_property_readonly(name "_raw", [](const V& v) { \
        const auto& x = v;                                 \
        return static_cast<std::int64_t>((expr).raw);      \
      })

}  // namespace

// ---- bindings
// -------------------------------------------------------------------------------------

void bind_strategy_api(py::module_& m) {
  g_stale_view_error = PyErr_NewExceptionWithDoc(
      "fastmm._core.StaleViewError",
      "A view (book, fill, trade, ...) was read outside the hook that received it.",
      PyExc_RuntimeError,
      nullptr);
  g_order_rejected = PyErr_NewExceptionWithDoc(
      "fastmm._core.OrderRejected",
      "ctx.send() or ctx.replace() was rejected; .reason is the RejectReason name.",
      PyExc_Exception,
      nullptr);
  if (g_stale_view_error == nullptr || g_order_rejected == nullptr) throw py::error_already_set();
  m.attr("StaleViewError") = py::reinterpret_borrow<py::object>(g_stale_view_error);
  m.attr("OrderRejected") = py::reinterpret_borrow<py::object>(g_order_rejected);

  // ---- Instrument (value) -------------------------------------------------------------------
  py::class_<InstrumentInfo> instrument_cls(
      m, "Instrument", "Reference data of one instrument (a copy; safe to keep).", py::is_final());
  instrument_cls
      .def_property_readonly("id", [](const InstrumentInfo& v) { return v.inst.id.value; })
      .def_property_readonly(
          "symbol", [](const InstrumentInfo& v) { return std::string(v.inst.symbol.view()); })
      .def_property_readonly("venue", [](const InstrumentInfo& v) { return v.inst.venue.value; })
      .def_property_readonly(
          "base", [](const InstrumentInfo& v) { return std::string(v.inst.base.view()); })
      .def_property_readonly(
          "quote", [](const InstrumentInfo& v) { return std::string(v.inst.quote.view()); })
      .def_property_readonly(
          "asset_class",
          [](const InstrumentInfo& v) { return std::string(to_string(v.inst.asset_class)); })
      .def(
          "round_price",
          [](const InstrumentInfo& v, double px, int side) {
            return v.inst.round_price(Price::from_double(px), side_from(side)).to_double();
          },
          py::arg("price"),
          py::arg("side"),
          "Round to the tick grid passively: bids down, asks up.")
      .def(
          "round_price_raw",
          [](const InstrumentInfo& v, std::int64_t px, int side) {
            return v.inst.round_price(Price::from_raw(px), side_from(side)).raw;
          },
          py::arg("price_raw"),
          py::arg("side"))
      .def(
          "round_qty",
          [](const InstrumentInfo& v, double q) {
            return v.inst.round_qty(Qty::from_double(q)).to_double();
          },
          py::arg("qty"),
          "Round down to the lot.")
      .def(
          "round_qty_raw",
          [](const InstrumentInfo& v, std::int64_t q) {
            return v.inst.round_qty(Qty::from_raw(q)).raw;
          },
          py::arg("qty_raw"))
      .def("__index__", [](const InstrumentInfo& v) { return v.inst.id.value; })
      .def("__int__", [](const InstrumentInfo& v) { return v.inst.id.value; })
      .def("__hash__",
           [](const InstrumentInfo& v) { return static_cast<Py_ssize_t>(v.inst.id.value); })
      .def("__eq__",
           [](const InstrumentInfo& v, py::handle other) {
             if (py::isinstance<InstrumentInfo>(other))
               return other.cast<const InstrumentInfo&>().inst.id == v.inst.id;
             if (PyLong_Check(other.ptr()) != 0 && PyBool_Check(other.ptr()) == 0)
               return other.cast<long long>() == static_cast<long long>(v.inst.id.value);
             return false;
           })
      .def("__repr__", [](const InstrumentInfo& v) {
        return "<Instrument " + std::to_string(v.inst.id.value) + " " +
               std::string(v.inst.symbol.view()) + ">";
      });
  FASTMM_PY_VALUE_FIXED(instrument_cls, InstrumentInfo, "tick", x.inst.tick);
  FASTMM_PY_VALUE_FIXED(instrument_cls, InstrumentInfo, "lot", x.inst.lot);
  FASTMM_PY_VALUE_FIXED(instrument_cls, InstrumentInfo, "min_qty", x.inst.min_qty);
  FASTMM_PY_VALUE_FIXED(instrument_cls, InstrumentInfo, "min_notional", x.inst.min_notional);
  FASTMM_PY_VALUE_FIXED(
      instrument_cls, InstrumentInfo, "contract_multiplier", x.inst.contract_multiplier);

  // ---- Order (value) ------------------------------------------------------------------------
  py::class_<OrderInfo> order(
      m, "Order", "Snapshot of one of our orders (a copy; safe to keep).", py::is_final());
  order.def_property_readonly("id", [](const OrderInfo& v) { return v.order.cl_ord_id.value; })
      .def_property_readonly("instrument",
                             [](const OrderInfo& v) { return v.order.instrument.value; })
      .def_property_readonly("side",
                             [](const OrderInfo& v) { return static_cast<int>(v.order.side); })
      .def_property_readonly(
          "state", [](const OrderInfo& v) { return std::string(to_string(v.order.state)); })
      .def_property_readonly("user_tag", [](const OrderInfo& v) { return v.order.user_tag; })
      .def_property_readonly("post_only",
                             [](const OrderInfo& v) { return v.order.has(Order::kPostOnly); })
      .def_property_readonly("reduce_only",
                             [](const OrderInfo& v) { return v.order.has(Order::kReduceOnly); })
      .def_property_readonly("created_ns", [](const OrderInfo& v) { return v.order.created.ns; })
      .def_property_readonly(
          "sent_ns",
          [](const OrderInfo& v) { return v.times.sent.ns; },
          "Engine time the order, or its latest acknowledged replace, was sent (ns).")
      .def_property_readonly(
          "venue_ack_ns",
          [](const OrderInfo& v) { return v.times.venue_ack.ns; },
          "The venue's accept time from the ack (exch_ts, ns; whole ms on Binance); 0 before the "
          "ack or when the venue gives none.")
      .def_property_readonly(
          "local_ack_ns",
          [](const OrderInfo& v) { return v.times.local_ack.ns; },
          "When the ack reached us (recv_ts, ns); 0 before the ack.")
      .def("__repr__", [](const OrderInfo& v) {
        return "<Order " + std::to_string(v.order.cl_ord_id.value) + " " +
               std::string(to_string(v.order.side)) + " " + std::string(to_string(v.order.state)) +
               ">";
      });
  FASTMM_PY_VALUE_FIXED(order, OrderInfo, "price", x.order.price);
  FASTMM_PY_VALUE_FIXED(order, OrderInfo, "qty", x.order.qty);
  FASTMM_PY_VALUE_FIXED(order, OrderInfo, "filled", x.order.cum_qty);
  FASTMM_PY_VALUE_FIXED(order, OrderInfo, "leaves", x.order.leaves_qty());

  py::class_<PortfolioInfo> portfolio(
      m, "Portfolio", "PnL totals over every instrument (a copy).", py::is_final());
  FASTMM_PY_VALUE_FIXED(portfolio, PortfolioInfo, "realized", x.p.realized);
  FASTMM_PY_VALUE_FIXED(portfolio, PortfolioInfo, "unrealized", x.p.unrealized);
  FASTMM_PY_VALUE_FIXED(portfolio, PortfolioInfo, "fees", x.p.fees);
  FASTMM_PY_VALUE_FIXED(portfolio, PortfolioInfo, "net", x.p.net);

  // ---- views ----------------------------------------------------------------------------------
  py::class_<BookView> book(m,
                            "BookView",
                            "The engine's L2 book of one instrument. Valid only inside the hook "
                            "that received it or called ctx.book().",
                            py::is_final());
  book.def_property_readonly("instrument", [](const BookView& v) { return v.id.value; })
      .def_property_readonly("valid", [](const BookView& v) { return v.get().is_valid(); })
      .def_property_readonly("best_bid",
                             [](const BookView& v) { return level_f(v.get().best_bid()); })
      .def_property_readonly("best_ask",
                             [](const BookView& v) { return level_f(v.get().best_ask()); })
      .def_property_readonly("best_bid_raw",
                             [](const BookView& v) { return level_raw(v.get().best_bid()); })
      .def_property_readonly("best_ask_raw",
                             [](const BookView& v) { return level_raw(v.get().best_ask()); })
      .def(
          "level",
          [](const BookView& v, int side, std::size_t i) {
            return level_f(v.get().level(side_from(side), i));
          },
          py::arg("side"),
          py::arg("i"),
          "(price, qty) of level i (0 is best); (0.0, 0.0) past the depth.")
      .def(
          "level_raw",
          [](const BookView& v, int side, std::size_t i) {
            return level_raw(v.get().level(side_from(side), i));
          },
          py::arg("side"),
          py::arg("i"))
      .def(
          "depth",
          [](const BookView& v, int side) { return v.get().depth(side_from(side)); },
          py::arg("side"))
      .def_property_readonly("last_update_ns",
                             [](const BookView& v) { return v.get().last_update().ns; })
      .def_property_readonly("seq", [](const BookView& v) { return v.get().seq(); })
      .def("microprice", [](const BookView& v) { return microprice(v.get()).to_double(); })
      .def("microprice_raw", [](const BookView& v) { return microprice(v.get()).raw; })
      .def(
          "imbalance",
          [](const BookView& v, std::size_t levels) {
            return to_f(imbalance(v.get(), levels).raw);
          },
          py::arg("levels") = 1,
          "(bid qty - ask qty) / (bid qty + ask qty) over the top levels, in [-1, 1].");
  FASTMM_PY_FIXED(book, BookView, "mid", x.mid());
  FASTMM_PY_FIXED(book, BookView, "spread", x.spread());

  py::class_<PositionView> pos(m,
                               "PositionView",
                               "Position and PnL of one instrument (valid inside the hook).",
                               py::is_final());
  pos.def_property_readonly("instrument", [](const PositionView& v) { return v.id.value; });
  FASTMM_PY_FIELD(pos, PositionView, "fills", x.fills);
  FASTMM_PY_FIXED(pos, PositionView, "qty", x.qty);
  FASTMM_PY_FIXED(pos, PositionView, "avg_price", x.avg_px);
  FASTMM_PY_FIXED(pos, PositionView, "realized", x.realized);
  FASTMM_PY_FIXED(pos, PositionView, "unrealized", x.unrealized);
  FASTMM_PY_FIXED(pos, PositionView, "fees", x.fees);
  FASTMM_PY_FIXED(pos, PositionView, "net_pnl", x.net_pnl());

  py::class_<TradeView> trade(
      m, "TradeView", "A public trade (valid inside on_trade).", py::is_final());
  trade.def_property_readonly("instrument", [](const TradeView& v) { return v.id.value; });
  FASTMM_PY_FIELD(trade, TradeView, "aggressor", static_cast<int>(x.aggressor));
  FASTMM_PY_FIELD(trade, TradeView, "trade_id", x.trade_id);
  FASTMM_PY_FIELD(trade, TradeView, "exch_ts_ns", x.hdr.exch_ts.ns);
  FASTMM_PY_FIELD(trade, TradeView, "recv_ts_ns", x.hdr.recv_ts.ns);
  FASTMM_PY_FIXED(trade, TradeView, "price", x.price);
  FASTMM_PY_FIXED(trade, TradeView, "qty", x.qty);

  py::class_<BookTickerView> ticker(m,
                                    "BookTickerView",
                                    "Best bid and ask update (valid inside on_book_ticker).",
                                    py::is_final());
  ticker.def_property_readonly("instrument", [](const BookTickerView& v) { return v.id.value; });
  FASTMM_PY_FIELD(ticker, BookTickerView, "exch_ts_ns", x.hdr.exch_ts.ns);
  FASTMM_PY_FIELD(ticker, BookTickerView, "recv_ts_ns", x.hdr.recv_ts.ns);
  FASTMM_PY_FIXED(ticker, BookTickerView, "bid_price", x.bid_px);
  FASTMM_PY_FIXED(ticker, BookTickerView, "bid_qty", x.bid_qty);
  FASTMM_PY_FIXED(ticker, BookTickerView, "ask_price", x.ask_px);
  FASTMM_PY_FIXED(ticker, BookTickerView, "ask_qty", x.ask_qty);

  py::class_<OptionTickerView> option(m,
                                      "OptionTickerView",
                                      "Venue mark, IVs and greeks (valid inside on_option_ticker).",
                                      py::is_final());
  option.def_property_readonly("instrument", [](const OptionTickerView& v) { return v.id.value; });
  FASTMM_PY_FIELD(option, OptionTickerView, "exch_ts_ns", x.hdr.exch_ts.ns);
  FASTMM_PY_FIELD(option, OptionTickerView, "recv_ts_ns", x.hdr.recv_ts.ns);
  FASTMM_PY_FIXED(option, OptionTickerView, "mark_price", x.mark_price);
  FASTMM_PY_FIXED(option, OptionTickerView, "underlying_price", x.underlying_price);
  FASTMM_PY_FIXED(option, OptionTickerView, "index_price", x.index_price);
  FASTMM_PY_FIELD(option, OptionTickerView, "mark_iv", x.mark_iv);
  FASTMM_PY_FIELD(option, OptionTickerView, "bid_iv", x.bid_iv);
  FASTMM_PY_FIELD(option, OptionTickerView, "ask_iv", x.ask_iv);
  FASTMM_PY_FIELD(option, OptionTickerView, "delta", x.delta);
  FASTMM_PY_FIELD(option, OptionTickerView, "gamma", x.gamma);
  FASTMM_PY_FIELD(option, OptionTickerView, "vega", x.vega);
  FASTMM_PY_FIELD(option, OptionTickerView, "theta", x.theta);
  FASTMM_PY_FIELD(option, OptionTickerView, "rho", x.rho);
  FASTMM_PY_FIELD(option, OptionTickerView, "interest_rate", x.interest_rate);

  py::class_<ConnectionView> conn(m,
                                  "ConnectionView",
                                  "A venue connection state change (valid inside on_connection).",
                                  py::is_final());
  FASTMM_PY_FIELD(conn, ConnectionView, "venue", x.hdr.venue.value);
  FASTMM_PY_FIELD(conn, ConnectionView, "state", std::string(to_string(x.state)));
  FASTMM_PY_FIELD(conn, ConnectionView, "live", x.state == ConnState::Live);
  FASTMM_PY_FIELD(conn, ConnectionView, "channel", static_cast<int>(x.channel));
  FASTMM_PY_FIELD(conn, ConnectionView, "reason_code", x.reason_code);
  FASTMM_PY_FIELD(conn, ConnectionView, "recv_ts_ns", x.hdr.recv_ts.ns);

  py::class_<OrderUpdateView> upd(
      m,
      "OrderUpdateView",
      "An order state change (valid inside on_order_update, or inside on_fill as fill.update).",
      py::is_final());
  FASTMM_PY_FIELD(upd, OrderUpdateView, "order_id", x.order.cl_ord_id.value);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "instrument", x.order.instrument.value);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "side", static_cast<int>(x.order.side));
  FASTMM_PY_FIELD(upd, OrderUpdateView, "state", std::string(to_string(x.order.state)));
  FASTMM_PY_FIELD(upd, OrderUpdateView, "prev_state", std::string(to_string(x.prev)));
  FASTMM_PY_FIELD(
      upd, OrderUpdateView, "reject_reason", std::string(to_string(x.order.reject_reason)));
  FASTMM_PY_FIELD(upd, OrderUpdateView, "user_tag", x.order.user_tag);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "known", x.known);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "changed", x.changed);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "terminal", x.terminal);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "sent_ns", x.times.sent.ns);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "venue_ack_ns", x.times.venue_ack.ns);
  FASTMM_PY_FIELD(upd, OrderUpdateView, "local_ack_ns", x.times.local_ack.ns);
  FASTMM_PY_FIXED(upd, OrderUpdateView, "price", x.order.price);
  FASTMM_PY_FIXED(upd, OrderUpdateView, "qty", x.order.qty);
  FASTMM_PY_FIXED(upd, OrderUpdateView, "filled", x.order.cum_qty);
  FASTMM_PY_FIXED(upd, OrderUpdateView, "leaves", x.order.leaves_qty());
  FASTMM_PY_FIXED(upd, OrderUpdateView, "fill_price", x.fill_px);
  FASTMM_PY_FIXED(upd, OrderUpdateView, "fill_qty", x.fill_qty);

  py::class_<FillView> fill(
      m,
      "FillView",
      "One execution, after position and fees are updated (valid inside on_fill).",
      py::is_final());
  fill.def_property_readonly("instrument",
                             [](const FillView& v) { return v.get().instrument.value; })
      .def_property_readonly("update", [](const FillView& v) -> py::object {
        if (v.get().update == nullptr) return py::none();
        return py::reinterpret_borrow<py::object>(v.update);
      });
  FASTMM_PY_FIELD(fill, FillView, "side", static_cast<int>(x.side));
  FASTMM_PY_FIELD(fill, FillView, "liquidity", static_cast<int>(x.liquidity));
  FASTMM_PY_FIELD(fill, FillView, "fee_converted", x.fee_converted);
  FASTMM_PY_FIELD(fill, FillView, "known", x.known);
  FASTMM_PY_FIELD(fill, FillView, "late", x.late);
  FASTMM_PY_FIELD(fill, FillView, "order_done", x.order_done);
  FASTMM_PY_FIELD(fill, FillView, "order_id", x.msg->cl_ord_id.value);
  FASTMM_PY_FIELD(fill, FillView, "exch_ts_ns", x.msg->hdr.exch_ts.ns);
  FASTMM_PY_FIXED(fill, FillView, "price", x.price);
  FASTMM_PY_FIXED(fill, FillView, "qty", x.qty);
  FASTMM_PY_FIXED(fill, FillView, "position_delta", x.position_delta);
  FASTMM_PY_FIXED(fill, FillView, "fee", x.fee);

  // ---- Context ----------------------------------------------------------------------------------
  py::class_<ContextHandle> ctx(m,
                                "Context",
                                "What a Python strategy sees (mirrors the C++ StrategyContext). "
                                "Usable only inside a hook.",
                                py::is_final());
  ctx.def_property_readonly(
         "now_ns",
         [](const ContextHandle& c) { return c.engine().now().ns; },
         "Engine time of the event, timer, start or stop being processed (ns).")
      .def_property_readonly(
          "instruments",
          [](const ContextHandle& c) {
            static_cast<void>(c.engine());
            return c.run->inst_tuple_;
          },
          "Every instrument of the table, as a tuple of Instrument (index == id).")
      .def(
          "instrument",
          [](const ContextHandle& c, py::handle inst) {
            static_cast<void>(c.engine());
            return c.run->inst_objs_[c.run->resolve(inst).value];
          },
          py::arg("inst"))
      .def(
          "contains",
          [](const ContextHandle& c, py::handle inst) {
            const PySimEngine& e = c.engine();
            if (PyBool_Check(inst.ptr()) != 0) return false;
            if (py::isinstance<InstrumentInfo>(inst)) return true;
            if (PyIndex_Check(inst.ptr()) == 0) return false;
            const long long v =
                py::int_(py::reinterpret_borrow<py::object>(inst)).cast<long long>();
            return v >= 0 && static_cast<std::size_t>(v) < e.instruments().size();
          },
          py::arg("inst"))
      .def(
          "book",
          [](const ContextHandle& c, py::handle inst) {
            PySimEngine& e = c.engine();
            const InstrumentId id = c.run->resolve(inst);
            c.run->book_views_[id.value]->point(e.book(id));
            return c.run->book_objs_[id.value];
          },
          py::arg("inst"),
          "The instrument's book view (valid until the current hook returns).")
      .def(
          "position",
          [](const ContextHandle& c, py::handle inst) {
            PySimEngine& e = c.engine();
            const InstrumentId id = c.run->resolve(inst);
            c.run->pos_views_[id.value]->point(e.position(id));
            return c.run->pos_objs_[id.value];
          },
          py::arg("inst"))
      .def(
          "portfolio",
          [](const ContextHandle& c) { return PortfolioInfo{c.engine().context().portfolio()}; },
          "PnL totals over every instrument.")
      .def(
          "set_quotes",
          [](const ContextHandle& c, py::handle inst, py::handle bids, py::handle asks) {
            return set_quotes(c, inst, bids, asks, false);
          },
          py::arg("inst"),
          py::arg("bids") = py::none(),
          py::arg("asks") = py::none(),
          "Desired ladder as sequences of (price, qty) floats, level 0 first (at most 8 per side). "
          "Prices are rounded to the tick passively (bids down, asks up), quantities down to the "
          "lot; a non-positive price or quantity drops the level. Returns False when quoting is "
          "disabled.")
      .def(
          "set_quotes_raw",
          [](const ContextHandle& c, py::handle inst, py::handle bids, py::handle asks) {
            return set_quotes(c, inst, bids, asks, true);
          },
          py::arg("inst"),
          py::arg("bids") = py::none(),
          py::arg("asks") = py::none(),
          "Like set_quotes with raw int prices and quantities (1e-8 scale), used as given.")
      .def(
          "pull_quotes",
          [](const ContextHandle& c, py::handle inst) {
            PySimEngine& e = c.engine();
            e.context().pull_quotes(c.run->resolve(inst));
          },
          py::arg("inst"))
      .def("pull_all_quotes",
           [](const ContextHandle& c) { c.engine().context().pull_all_quotes(); })
      .def(
          "working_quote",
          [](const ContextHandle& c, py::handle inst, int side, std::uint32_t level) {
            PySimEngine& e = c.engine();
            return order_or_none(
                e, e.context().working_quote(c.run->resolve(inst), side_from(side), level));
          },
          py::arg("inst"),
          py::arg("side"),
          py::arg("level") = 0,
          "The open order in a quote slot, or None.")
      .def(
          "send",
          [](const ContextHandle& c,
             py::handle inst,
             int side,
             py::handle price,
             py::handle qty,
             bool post_only,
             bool reduce_only,
             bool ioc,
             std::uint32_t tag) {
            return send(c, inst, side, price, qty, post_only, reduce_only, ioc, tag, false);
          },
          py::arg("inst"),
          py::arg("side"),
          py::arg("price"),
          py::arg("qty"),
          py::kw_only(),
          py::arg("post_only") = false,
          py::arg("reduce_only") = false,
          py::arg("ioc") = false,
          py::arg("tag") = 0,
          "Send a limit order (risk-checked, journaled); returns its client order id. The price is "
          "rounded passively to the tick and the quantity down to the lot. Raises OrderRejected.")
      .def(
          "send_raw",
          [](const ContextHandle& c,
             py::handle inst,
             int side,
             py::handle price,
             py::handle qty,
             bool post_only,
             bool reduce_only,
             bool ioc,
             std::uint32_t tag) {
            return send(c, inst, side, price, qty, post_only, reduce_only, ioc, tag, true);
          },
          py::arg("inst"),
          py::arg("side"),
          py::arg("price_raw"),
          py::arg("qty_raw"),
          py::kw_only(),
          py::arg("post_only") = false,
          py::arg("reduce_only") = false,
          py::arg("ioc") = false,
          py::arg("tag") = 0)
      .def(
          "cancel",
          [](const ContextHandle& c, std::uint64_t order_id) {
            return c.engine().context().cancel(ClientOrderId{order_id}).has_value();
          },
          py::arg("order_id"),
          "Request a cancel; False when the order is unknown or cannot be cancelled.")
      .def(
          "replace",
          [](const ContextHandle& c, std::uint64_t order_id, py::handle price, py::handle qty) {
            replace(c, order_id, price, qty, false);
          },
          py::arg("order_id"),
          py::arg("price"),
          py::arg("qty"),
          "Cancel-replace (venues that support it). Raises OrderRejected.")
      .def(
          "replace_raw",
          [](const ContextHandle& c, std::uint64_t order_id, py::handle price, py::handle qty) {
            replace(c, order_id, price, qty, true);
          },
          py::arg("order_id"),
          py::arg("price_raw"),
          py::arg("qty_raw"))
      .def(
          "order",
          [](const ContextHandle& c, std::uint64_t order_id) {
            PySimEngine& e = c.engine();
            return order_or_none(e, e.context().order(ClientOrderId{order_id}));
          },
          py::arg("order_id"),
          "Snapshot of an open order, or None once it is terminal.")
      .def(
          "queue_ahead",
          [](const ContextHandle& c, std::uint64_t order_id) {
            return qty_or_none(c.engine().context().queue_ahead(ClientOrderId{order_id}), false);
          },
          py::arg("order_id"),
          "Estimated quantity resting ahead of an open order at its price (the l2_queue model on "
          "the market data the strategy sees), or None before the ack and once it is terminal. "
          "Queues are tracked from the first call on; call it in on_start to cover every order "
          "from its ack.")
      .def(
          "queue_ahead_raw",
          [](const ContextHandle& c, std::uint64_t order_id) {
            return qty_or_none(c.engine().context().queue_ahead(ClientOrderId{order_id}), true);
          },
          py::arg("order_id"))
      .def(
          "own_qty",
          [](const ContextHandle& c, py::handle inst, int side, double price, py::handle at_ns) {
            PySimEngine& e = c.engine();
            const InstrumentId id = c.run->resolve(inst);
            const Price p = Price::from_double(price);
            const Qty q = at_ns.is_none()
                              ? e.context().own_qty(id, side_from(side), p)
                              : e.context().own_qty(
                                    id, side_from(side), p, Timestamp{at_ns.cast<std::int64_t>()});
            return q.to_double();
          },
          py::arg("inst"),
          py::arg("side"),
          py::arg("price"),
          py::arg("at_ns") = py::none(),
          "Our resting quantity that the venue's feed shows at this price, as of the book's last "
          "update or of venue time at_ns. 0.0 where the feed does not show our orders (a "
          "backtest).")
      .def(
          "own_qty_raw",
          [](const ContextHandle& c,
             py::handle inst,
             int side,
             std::int64_t price,
             py::handle at_ns) {
            PySimEngine& e = c.engine();
            const InstrumentId id = c.run->resolve(inst);
            const Price p = Price::from_raw(price);
            const Qty q = at_ns.is_none()
                              ? e.context().own_qty(id, side_from(side), p)
                              : e.context().own_qty(
                                    id, side_from(side), p, Timestamp{at_ns.cast<std::int64_t>()});
            return q.raw;
          },
          py::arg("inst"),
          py::arg("side"),
          py::arg("price_raw"),
          py::arg("at_ns") = py::none())
      .def(
          "best_ex_self",
          [](const ContextHandle& c, py::handle inst, int side) {
            PySimEngine& e = c.engine();
            return level_f(e.context().best_ex_self(c.run->resolve(inst), side_from(side)));
          },
          py::arg("inst"),
          py::arg("side"),
          "(price, qty) of the book's best level on one side after own_qty is taken out; a level "
          "that was only ours is skipped. (0.0, 0.0) when none is left.")
      .def(
          "best_ex_self_raw",
          [](const ContextHandle& c, py::handle inst, int side) {
            PySimEngine& e = c.engine();
            return level_raw(e.context().best_ex_self(c.run->resolve(inst), side_from(side)));
          },
          py::arg("inst"),
          py::arg("side"))
      .def(
          "open_qty",
          [](const ContextHandle& c, py::handle inst, int side) {
            PySimEngine& e = c.engine();
            return e.context().open_qty(c.run->resolve(inst), side_from(side)).to_double();
          },
          py::arg("inst"),
          py::arg("side"))
      .def(
          "open_qty_raw",
          [](const ContextHandle& c, py::handle inst, int side) {
            PySimEngine& e = c.engine();
            return e.context().open_qty(c.run->resolve(inst), side_from(side)).raw;
          },
          py::arg("inst"),
          py::arg("side"))
      .def(
          "every",
          [](const ContextHandle& c, std::int64_t period_ns, std::uint64_t tag) {
            return timer_or_raise(
                c.engine().context().every(nanoseconds(positive_ns(period_ns, "period_ns")), tag));
          },
          py::arg("period_ns"),
          py::arg("tag") = 0,
          "Repeating timer; on_timer(ctx, timer_id, tag) fires every period_ns. Returns the id.")
      .def(
          "once",
          [](const ContextHandle& c, std::int64_t delay_ns, std::uint64_t tag) {
            return timer_or_raise(
                c.engine().context().once(nanoseconds(positive_ns(delay_ns, "delay_ns")), tag));
          },
          py::arg("delay_ns"),
          py::arg("tag") = 0,
          "One-shot timer after delay_ns. Returns the id.")
      .def(
          "cancel_timer",
          [](const ContextHandle& c, std::uint32_t timer_id) {
            return c.engine().context().cancel_timer(TimerId{timer_id});
          },
          py::arg("timer_id"))
      .def_property_readonly("quoting_enabled",
                             [](const ContextHandle& c) { return c.engine().quoting_enabled(); })
      .def_property_readonly("killed",
                             [](const ContextHandle& c) { return c.engine().risk().killed(); })
      .def(
          "request_stop",
          [](const ContextHandle& c) { c.engine().context().request_stop(); },
          "End the backtest after the current event.")
      .def(
          "random",
          [](const ContextHandle& c) { return c.engine().context().rng().uniform01(); },
          "Float in [0, 1) from the engine's seeded RNG (deterministic).")
      .def(
          "randint",
          [](const ContextHandle& c, std::int64_t lo, std::int64_t hi) {
            PySimEngine& e = c.engine();
            if (hi < lo) throw py::value_error("fastmm: randint needs lo <= hi");
            return e.context().rng().between(lo, hi);
          },
          py::arg("lo"),
          py::arg("hi"),
          "Integer in [lo, hi] inclusive from the engine's seeded RNG.");
}

#undef FASTMM_PY_FIXED
#undef FASTMM_PY_FIELD
#undef FASTMM_PY_VALUE_FIXED

// ---- run --------------------------------------------------------------------------------------

py::tuple run_python_strategy(const bt::BacktestConfig& cfg,
                              bt::MdSource* source,
                              const py::object& instance,
                              const std::string& name,
                              const std::vector<std::string>& hooks) {
  std::uint32_t mask = 0;
  for (const std::string& h : hooks) {
    bool found = false;
    for (const HookInfo& info : kHooks) {
      if (info.name == h) {
        mask |= hook_bit(info.hook);
        found = true;
      }
    }
    if (!found) throw py::value_error("fastmm: '" + h + "' is not a strategy hook");
  }
  bt::BacktestSession session(cfg, source);
  std::unique_ptr<IEngineRunner> runner =
      session.backend().template make_runner<PyStrategy>(session.deps());
  auto* er = static_cast<EngineRunner<PySimEngine, PyStrategy>*>(runner.get());
  PyRun run(er->engine(), er->strategy(), instance, mask);
  struct Detach {
    PyRun& run;
    PyStrategy& strategy;
    Detach(PyRun& r, PyStrategy& s) noexcept : run(r), strategy(s) {}
    Detach(const Detach&) = delete;
    Detach& operator=(const Detach&) = delete;
    ~Detach() {
      strategy.attach(nullptr, 0);
      run.close();
    }
  } const detach{run, er->strategy()};
  er->strategy().attach(&run, mask);
  sim::EngineHooks engine_hooks = session.backend().hooks;
  engine_hooks.stopped = [](void* c) {
    auto* e = static_cast<PySimEngine*>(c);
    e->strategy().on_driver_step();
    return e->stopped();
  };
  auto result = std::make_shared<bt::BacktestResult>(session.run(engine_hooks, runner.get(), name));
  py::object error = run.failed() ? py::object(run.error_info()) : py::none();
  return py::make_tuple(result, error);
}

}  // namespace fastmm::py_bind
