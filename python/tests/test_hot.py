"""Python hot hooks (ADR-0013, section 1): parity with C++, class checks, compile checks, failures,
the GIL and the Numba cache."""

import ctypes
import os
import subprocess
import sys
import threading

import numpy as np
import pytest

numba = pytest.importorskip("numba")

import fastmm  # noqa: E402
from conftest import FIXTURE_FMJ, FIXTURE_SHA, REPO  # noqa: E402
from fastmm import Param, State, Strategy, fx  # noqa: E402
from fastmm._hot import abi, compiler  # noqa: E402

EXAMPLES = REPO / "examples" / "python" / "strategies"
sys.path.insert(0, str(EXAMPLES))

from basic_mm_hot import BasicMMHot, BasicMMHotFloat  # noqa: E402


@pytest.fixture(autouse=True, scope="module")
def _cache_dir(tmp_path_factory):
    old = os.environ.get("FASTMM_CACHE_DIR")
    os.environ["FASTMM_CACHE_DIR"] = str(tmp_path_factory.mktemp("fastmm-cache"))
    yield
    if old is None:
        os.environ.pop("FASTMM_CACHE_DIR", None)
    else:
        os.environ["FASTMM_CACHE_DIR"] = old


def _synthetic(example_config, seconds, fill_model="matching"):
    cfg = example_config.copy()
    cfg.clear_params()
    cfg.duration_s = seconds
    cfg.fill_model = fill_model
    return cfg


def _fixture_run(example_config, cls):
    cfg = example_config.copy()
    cfg.clear_params()
    return fastmm.run_backtest(cfg, data=FIXTURE_FMJ, strategy=cls)


# ---- parity with C++ ------------------------------------------------------------------------------

def test_hot_basic_mm_reproduces_the_committed_fixture_hash(example_config):
    expected = FIXTURE_SHA.read_text().strip()
    for cls in (BasicMMHot, BasicMMHotFloat):
        r = fastmm.run_backtest(example_config, data=FIXTURE_FMJ, strategy=cls)
        assert r.outbound_sha256 == expected, cls.__name__
        assert r.outbound_messages > 0 and r.md_events == 1000
        assert r.strategy == "py:" + cls.__name__
    assert r.params["quote_qty"] == "0.002"


@pytest.mark.parametrize(
    "fill_model, params",
    [
        ("l2_queue", {"levels": 3, "level_step_ticks": 2, "pull_on_stale_ms": 20,
                      "skew_bps_per_unit": 0.05, "max_inventory": 0.006}),
        ("matching", {"levels": 2, "half_spread_bps": 0.003, "skew_bps_per_unit": 0.02,
                      "requote_threshold_ticks": 1}),
    ],
)
def test_hot_basic_mm_matches_cpp_with_fills_timers_and_skew(example_config, fill_model, params):
    cfg = example_config.copy()
    cfg.fill_model = fill_model
    cfg.duration_s = 30
    cfg.seed = 11
    cpp = fastmm.run_backtest(cfg, data="synthetic", strategy="basic_mm", params=params)
    hot = fastmm.run_backtest(cfg, data="synthetic", strategy=BasicMMHot, params=params)
    assert cpp.stats()["fills"] > 10 and cpp.stats()["cancels"] > 10
    if fill_model == "l2_queue":
        assert cpp.engine_stats()["timers_fired"] > 0
    assert hot.outbound_sha256 == cpp.outbound_sha256
    assert hot.outbound_messages == cpp.outbound_messages
    assert hot.stats()["net_pnl"] == cpp.stats()["net_pnl"]


# ---- class creation --------------------------------------------------------------------------------

def test_a_hot_class_cannot_define_strategy_hooks():
    with pytest.raises(TypeError, match="cannot define the hooks of fastmm.Strategy"):
        class Mixed(Strategy):
            @fastmm.hot
            def on_book(self, ctx, book):
                pass

            def on_trade(self, ctx, inst, trade):
                pass


def test_params_and_state_share_one_namespace():
    class Base(Strategy):
        spread = Param(1.0)

    with pytest.raises(TypeError, match="both as a Param and as a State"):
        class Sub(Base):
            spread = State(0.0)

            @fastmm.hot
            def on_book(self, ctx, book):
                pass

    with pytest.raises(TypeError, match="raw twin of parameter 'spread'"):
        class Twin(Strategy):
            spread = Param(1.0)
            spread_raw = State(0)

            @fastmm.hot
            def on_book(self, ctx, book):
                pass

    with pytest.raises(TypeError, match="name of a ctx method"):
        class Method(Strategy):
            quote = Param(1.0)

            @fastmm.hot
            def on_book(self, ctx, book):
                pass


def test_hot_hook_names_signatures_and_periods():
    with pytest.raises(TypeError, match="applies to on_book, on_fill, on_quoting, on_connection"):
        class Trade(Strategy):
            @fastmm.hot
            def on_trade(self, ctx, book):
                pass

    with pytest.raises(TypeError, match=r"wrong signature; a hot hook is on_book\(self, ctx, book\)"):
        class Short(Strategy):
            @fastmm.hot
            def on_book(self, ctx):
                pass

    with pytest.raises(TypeError, match="needs a name that is not a hook name"):
        class TimerNamedLikeAHook(Strategy):
            @fastmm.hot(every="1s")
            def on_book(self, ctx, book):
                pass

    with pytest.raises(ValueError, match="not a positive period"):
        fastmm.hot(every="10 parsecs")
    assert fastmm._hot.decl.parse_period("1.5s") == 1_500_000_000
    assert fastmm._hot.decl.parse_period("250us") == 250_000


def test_the_source_lint_rejects_assignments_to_parameters():
    with pytest.raises(TypeError, match=r"on_book assigns parameter 'spread' \(line \d+\)"):
        class Writes(Strategy):
            spread = Param(1.0)

            @fastmm.hot
            def on_book(self, ctx, book):
                self.spread = 2.0

    with pytest.raises(TypeError, match="assigns parameter 'spread_raw'"):
        class WritesRaw(Strategy):
            spread = Param(1.0)

            @fastmm.hot(every="1s")
            def refresh(self, ctx, book):
                self.spread_raw += 1


def test_missing_numba_names_the_extra():
    code = (
        "import sys\n"
        "sys.modules['numba'] = None\n"
        "import fastmm\n"
        "assert 'llvmlite' not in sys.modules\n"
        "try:\n"
        "    class M(fastmm.Strategy):\n"
        "        @fastmm.hot\n"
        "        def on_book(self, ctx, book):\n"
        "            pass\n"
        "except ImportError as e:\n"
        "    print(e)\n"
    )
    out = subprocess.run([sys.executable, "-c", code], check=True, capture_output=True, text=True)
    assert 'pip install "fastmm[hot]"' in out.stdout


# ---- the compile checks -----------------------------------------------------------------------------

class Allocates(Strategy):
    @fastmm.hot
    def on_book(self, ctx, book):
        a = np.empty(4)
        a[0] = book.mid
        ctx.quote(a[0] - 1.0, a[0] + 1.0, 0.001)


class BuildsAList(Strategy):
    @fastmm.hot
    def on_book(self, ctx, book):
        levels = [book.best_bid, book.best_ask]
        ctx.quote(levels[0], levels[1], 0.001)


class Prints(Strategy):
    @fastmm.hot(every="1s")
    def report(self, ctx, book):
        print(book.mid)


@pytest.mark.parametrize(
    "cls, hook, symbol",
    [
        (Allocates, "on_book", "NRT_MemInfo_alloc"),
        (BuildsAList, "on_book", "NRT_MemInfo_new_varsize"),
        (Prints, "report", "numba_gil_ensure"),
    ],
)
def test_the_ir_check_names_the_hook_and_the_symbol(example_config, cls, hook, symbol):
    with pytest.raises(fastmm.HotCompileError, match=rf"{cls.__name__}\.{hook} is rejected by the IR "
                                                     rf"check: it calls .*{symbol}"):
        _fixture_run(example_config, cls)


def test_a_typing_error_names_the_hook(example_config):
    class RawFromFloat(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            ctx.bid_raw(book.best_bid, 1)

    with pytest.raises(fastmm.HotCompileError, match=r"RawFromFloat\.on_book does not compile(.|\n)*"
                                                     r"ctx\.bid_raw takes int raw values"):
        _fixture_run(example_config, RawFromFloat)


def _call(cls, hook, record=None, ctx=None, book=None):
    compiled = compiler.compiled(cls, cls._fastmm_hot, cache=False)
    record = np.zeros(1, compiled.dtype) if record is None else record
    ctx = np.zeros(1, abi.CTX_DTYPE) if ctx is None else ctx
    book = np.zeros(1, abi.BOOK_DTYPE) if book is None else book
    fn = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
        compiled.cfuncs[hook].address)
    return fn(record.ctypes.data, ctx.ctypes.data, book.ctypes.data), record, ctx, book


def test_out_of_bounds_access_returns_a_status_and_writes_nothing_past_the_array():
    class WritesPast(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            for i in range(9):
                ctx.bid_px[i] = 1.0

    class ReadsPast(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            ctx.quote(book.ask_qty_raw[book.n_asks + 10], 1.0, 1.0)

    status, _, ctx, _ = _call(WritesPast, "on_book")
    assert status == abi.EXCEPTION
    assert (ctx["bid_px"][0] == 1.0).all()
    assert (ctx["bid_qty"][0] == 0.0).all()
    status, _, ctx, _ = _call(ReadsPast, "on_book")
    assert status == abi.EXCEPTION
    assert ctx["action"][0] == abi.ACTION_NONE


def test_ctx_methods_write_intents():
    class Intents(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            ctx.quote(100.0, 101.0, 0.5)
            ctx.bid(99.0, 0.25)
            ctx.ask_raw(10_200_000_000, 25_000_000)
            ctx.uncross()
            ctx.keep_passive()

    status, _, ctx, _ = _call(Intents, "on_book")
    c = ctx[0]
    assert status == abi.OK and c["action"] == abi.ACTION_QUOTE
    assert (c["n_bids"], c["n_asks"]) == (2, 2)
    assert list(c["bid_px"][:2]) == [100.0, 99.0] and list(c["bid_is_raw"][:2]) == [0, 0]
    assert c["ask_px"][0] == 101.0 and c["ask_is_raw"][1] == 1
    assert (c["ask_px_raw"][1], c["ask_qty_raw"][1]) == (10_200_000_000, 25_000_000)
    assert c["flags"] == abi.FLAG_UNCROSS | abi.FLAG_KEEP_PASSIVE


def test_book_levels_are_copied_for_code_that_can_read_them(example_config):
    assert compiler.compiled(BasicMMHot, BasicMMHot._fastmm_hot, cache=False).book_depth is False

    @numba.njit
    def best_ask_level(book):
        return book.ask_px_raw[0]

    class ReadsLevels(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid and (book.n_bids == 0 or book.bid_px[0] != book.best_bid):
                ctx.fail(5)

    class ThroughHelper(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid and best_ask_level(book) != book.best_ask_raw:
                ctx.fail(6)

    @numba.extending.register_jitable
    def plain_function(x):
        return x

    class ThroughPlainFunction(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            ctx.quote(plain_function(book.best_bid), book.best_ask, 0.002)

    for cls in (ReadsLevels, ThroughHelper):
        assert compiler.compiled(cls, cls._fastmm_hot, cache=True).book_depth, cls.__name__
        _fixture_run(example_config, cls)
    # A plain function (register_jitable, @overload stubs) cannot be followed: assume it reads them.
    assert compiler.uses_book_depth([ThroughPlainFunction._fastmm_hot.hooks["on_book"].fn])


# ---- failures -----------------------------------------------------------------------------------------

class Quoter(Strategy):
    calls = State(0)
    fail_at = 20

    @fastmm.hot
    def on_book(self, ctx, book):
        self.calls += 1
        if book.valid:
            ctx.quote(book.best_bid, book.best_ask, 0.002)
        if self.calls == 20:
            raise ValueError("boom")


def test_an_exception_trips_the_kill_switch_and_raises_strategy_error(example_config):
    cfg = _synthetic(example_config, 20)
    with pytest.raises(fastmm.StrategyError) as ei:
        fastmm.run_backtest(cfg, data="synthetic", strategy=Quoter)
    e = ei.value
    assert (e.hook, e.status, e.kill_reason) == ("on_book", abi.EXCEPTION, "StrategyError")
    assert "Quoter.on_book raised an exception" in str(e)
    full = fastmm.run_backtest(cfg, data="synthetic", strategy="basic_mm")
    assert 0 < e.events < full.engine_steps
    assert e.result.orders["ts"].size > 0 and e.result.end_ts <= e.now_ns + 1_000_000_000


def test_ctx_fail_and_bad_float_levels(example_config):
    class Fails(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid:
                ctx.fail(7)

    class NotFinite(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid:
                ctx.quote(np.nan, book.best_ask, 0.002)

    class NegativeQty(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid:
                ctx.bid(book.best_bid, -0.002)

    with pytest.raises(fastmm.StrategyError, match=r"Fails\.on_book called ctx\.fail\(7\)") as ei:
        _fixture_run(example_config, Fails)
    assert (ei.value.status, ei.value.fail_code) == (abi.FAILED, 7)
    for cls in (NotFinite, NegativeQty):
        with pytest.raises(fastmm.StrategyError, match="not finite, out of range or negative") as ei:
            _fixture_run(example_config, cls)
        assert ei.value.status == abi.BAD_VALUE and ei.value.kill_reason == "StrategyError"


def test_parameter_writes_through_an_alias_do_not_persist_and_state_does(example_config):
    class Alias(Strategy):
        p = Param(1.0)
        calls = State(0)

        @fastmm.hot
        def on_book(self, ctx, book):
            if self.p != 1.0 or self.p_raw != 100_000_000:
                ctx.fail(7)  # an earlier call's assignment persisted
                return
            t = self
            t.p = 42.0
            t.p_raw = 5
            self.calls += 1
            if self.calls == 20:
                ctx.fail(9)

    with pytest.raises(fastmm.StrategyError) as ei:
        _fixture_run(example_config, Alias)
    assert ei.value.fail_code == 9


def test_warm_up_leaves_state_untouched(example_config):
    class FirstCall(Strategy):
        n = State(0)

        @fastmm.hot
        def on_book(self, ctx, book):
            self.n += 1
            ctx.fail(self.n)

    with pytest.raises(fastmm.StrategyError) as ei:
        _fixture_run(example_config, FirstCall)
    assert ei.value.fail_code == 1


def test_timer_and_fill_hooks(example_config):
    class Timer(Strategy):
        n = State(0)

        @fastmm.hot(every="10ms")
        def tick(self, ctx, book):
            self.n += 1
            if self.n == 30:
                ctx.fail(3)

    with pytest.raises(fastmm.StrategyError) as ei:
        fastmm.run_backtest(_synthetic(example_config, 5), data="synthetic", strategy=Timer)
    assert (ei.value.hook, ei.value.fail_code) == ("tick", 3)
    assert ei.value.now_ns - ei.value.result.start_ts == pytest.approx(300_000_000, abs=10_000_000)

    class Filled(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid:
                ctx.quote(book.best_bid, book.best_ask, 0.002)

        @fastmm.hot
        def on_fill(self, ctx, book):
            if ctx.fill_qty_raw == 200_000 and ctx.position_raw != 0 and ctx.fill_qty == 0.002:
                ctx.fail(1 + ctx.fill_side)

    with pytest.raises(fastmm.StrategyError) as ei:
        fastmm.run_backtest(_synthetic(example_config, 30), data="synthetic", strategy=Filled)
    e = ei.value
    assert e.hook == "on_fill"
    assert e.fail_code == 1 + int(e.result.fills["side"][0])


# ---- GIL, cache, fx -----------------------------------------------------------------------------------

def test_hot_hooks_run_while_another_thread_holds_the_gil(example_config):
    class Ticker(Strategy):
        seen = State(0.0)

        @fastmm.hot(every="1ms")
        def tick(self, ctx, book):
            self.seen = book.mid

    cfg = _synthetic(example_config, 900)
    out = {}
    t = threading.Thread(target=lambda: out.setdefault(
        "r", fastmm.run_backtest(cfg, data="synthetic", strategy=Ticker)))
    t.start()
    reached, calls = fastmm._core._hot_hold_gil(200_000, 120.0)
    t.join()
    assert reached, f"only {calls} hook calls while this thread held the GIL"
    assert out["r"].engine_stats()["timers_fired"] >= 200_000


_CACHE_PROBE = """
import sys
sys.path.insert(0, {examples!r})
import fastmm
from basic_mm_hot import BasicMMHot
from fastmm._hot import compiler
c = compiler.compiled(BasicMMHot, BasicMMHot._fastmm_hot, cache={cache})
print(sum(c.cache_hits.values()), len(c.cfuncs), compiler.cache_dir())
"""


def test_backtests_use_a_keyed_numba_cache_and_cache_false_writes_nothing(tmp_path):
    def probe(cache, base):
        env = dict(os.environ, FASTMM_CACHE_DIR=str(base))
        code = _CACHE_PROBE.format(examples=str(EXAMPLES), cache=cache)
        out = subprocess.run([sys.executable, "-c", code], env=env, check=True,
                             capture_output=True, text=True)
        hits, hooks, path = out.stdout.split()
        return int(hits), int(hooks), path

    first = probe(True, tmp_path / "a")
    second = probe(True, tmp_path / "a")
    assert first[0] == 0 and second[0] == second[1] == 5
    assert f"fastmm-{fastmm.__version__}-abi{abi.VERSION}-{abi.ABI_HASH}" in second[2]
    assert f"numba{numba.__version__}" in second[2]
    assert any((tmp_path / "a").rglob("*.nbi"))
    assert probe(False, tmp_path / "b")[0] == 0
    assert not (tmp_path / "b").exists()


@numba.njit
def _fx_jit(a, b, tick):
    return (fx.tdiv(a, b), fx.mul_ratio(a, b), fx.round_price(a, tick, fx.BUY),
            fx.round_price(a, tick, fx.SELL), fx.round_qty(a, tick), fx.bps_ratio(a))


def test_fx_matches_the_cpp_operators_in_python_and_in_numba():
    values = [0, 1, -1, 7, -7, 150_000_000, -150_000_000, 6_000_000_000_000, -6_000_000_000_000,
              2**62, -(2**62)]
    divisors = [3, -3, 10_000, -10_000, 1_000_000, 2**40]
    for a in values:
        for b in divisors:
            py = (fx.tdiv(a, b), fx.mul_ratio(a, b), fx.round_price(a, 1_000_000, fx.BUY),
                  fx.round_price(a, 1_000_000, fx.SELL), fx.round_qty(a, 1_000_000),
                  fx.bps_ratio(a))
            assert _fx_jit(a, b, 1_000_000) == py, (a, b)
    assert fx.mul_ratio(-150_000_000, 1) == -1  # truncates toward zero
    assert fx.tdiv(-7, 2) == -3
    assert fx.round_price(-150, 100, fx.SELL) == -100 and fx.round_price(-150, 100, fx.BUY) == -200
    assert fx.bps_ratio(1_000_000) == 100  # 0.01 bps
    assert fx.to_raw(99.95) == 9_995_000_000 and fx.to_raw(-0.000000005) == -1
    with pytest.raises(ZeroDivisionError):
        fx.tdiv(1, 0)
