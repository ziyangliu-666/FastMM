"""Slow methods of Python strategies (ADR-0013): scheduling at simulated times, publish, snapshot,
recent rows, fills, slow timing, max_param_age_ms, the live runner loop and fastmm.replay."""

import re
import sys
import threading
import time
import warnings

import numpy as np
import pytest

numba = pytest.importorskip("numba")

import fastmm  # noqa: E402
from conftest import EXAMPLE_TOML, REPO  # noqa: E402
from fastmm import Param, State, Strategy  # noqa: E402
from fastmm._hot import decl as hot_decl  # noqa: E402
from fastmm._slow import abi as slow_abi  # noqa: E402
from fastmm._slow import decl as slow_decl  # noqa: E402
from fastmm._slow import runner as slow_runner  # noqa: E402

sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

from hot_slow_mm import HotSlowMM  # noqa: E402

SECOND = 1_000_000_000


@pytest.fixture(autouse=True, scope="module")
def _cache_dir(tmp_path_factory):
    import os

    old = os.environ.get("FASTMM_CACHE_DIR")
    os.environ["FASTMM_CACHE_DIR"] = str(tmp_path_factory.mktemp("fastmm-cache"))
    yield
    if old is None:
        os.environ.pop("FASTMM_CACHE_DIR", None)
    else:
        os.environ["FASTMM_CACHE_DIR"] = old


def _cfg(example_config, seconds=20):
    cfg = example_config.copy()
    cfg.clear_params()
    cfg.duration_s = seconds
    cfg.fill_model = "matching"
    return cfg


@pytest.fixture()
def two_instruments(tmp_path):
    extra = ('[[instruments]]\nvenue = "sim"\nsymbol = "ETHUSDT"\nbase = "ETH"\nquote = "USDT"\n'
             'asset_class = "spot"\ntick = "0.01"\nlot = "0.0001"\nmin_qty = "0.0001"\n'
             'min_notional = "5"\n\n')
    text = re.sub(r"^\[strategy\]", lambda m: extra + m.group(0), EXAMPLE_TOML.read_text(),
                  count=1, flags=re.M)
    path = tmp_path / "two.toml"
    path.write_text(text)
    cfg = fastmm.BacktestConfig.from_toml(path)
    cfg.clear_params()
    cfg.duration_s = 5
    return cfg


class Recorder(Strategy):
    """Quotes at the touch and records what its slow methods see."""

    half_spread_bps = Param(0.2, min=0.0, max=1000.0)
    qty = Param(0.001, min=0.0, max=1.0)

    @fastmm.hot
    def on_book(self, ctx, book):
        if book.valid:
            half = book.mid * self.half_spread_bps * 1e-4
            ctx.quote(book.mid - half, book.mid + half, self.qty)
            ctx.keep_passive()

    def on_start(self, ctx):
        self.start_ns = ctx.now_ns
        self.calls = []
        self.final_fills = None
        ctx.publish(half_spread_bps=0.2)

    @fastmm.every("250ms")
    def look(self, ctx):
        self.calls.append((ctx.now_ns, ctx.snapshot(), ctx.recent("BTCUSDT"), ctx.fills()))
        ctx.publish()

    def on_stop(self, ctx):
        self.final_fills = ctx.fills()


def _noop(self, ctx, book):
    pass


# ---- scheduling, snapshot, recent rows and fills --------------------------------------------------

def test_slow_methods_run_at_simulated_times_and_repeat_exactly(example_config):
    runs = []
    for _ in range(2):
        s = Recorder()
        r = fastmm.run_backtest(_cfg(example_config), data="synthetic", strategy=s)
        runs.append((s, r))
    (a, ra), (b, rb) = runs
    assert ra.outbound_messages > 0 and ra.outbound_sha256 == rb.outbound_sha256
    times = [now - a.start_ns for now, *_ in a.calls]
    assert times[:4] == [0, SECOND // 4, SECOND // 2, 3 * SECOND // 4]
    assert times == [now - b.start_ns for now, *_ in b.calls]
    assert [c[1].version for c in a.calls] == [c[1].version for c in b.calls]
    assert [len(c[2].rows) for c in a.calls] == [len(c[2].rows) for c in b.calls]
    assert a.final_fills is not None
    assert ra.slow_methods["look"]["calls"] == len(a.calls)


def test_snapshot_recent_rows_and_fills_describe_the_engine(example_config):
    s = Recorder()
    r = fastmm.run_backtest(_cfg(example_config, 30), data="synthetic", strategy=s)
    fills = np.concatenate([c[3] for c in s.calls] + [s.final_fills])
    assert len(fills) == len(r.fills["ts"]) > 0
    assert np.array_equal(fills["seq"], np.arange(1, len(fills) + 1))
    assert fills["qty"].sum() == pytest.approx(r.fills["qty"].sum() * 1e-8)
    for now, snap, recent, got in s.calls[1:]:
        assert snap.ts_ns <= now and snap.age_ms >= 0
        inst = snap["BTCUSDT"]
        assert inst is snap[0] or inst == snap[0]
        assert inst.book_valid and 0 < inst.bid < inst.ask
        assert inst.param_age_ms == pytest.approx(250.0)
        rows = recent.rows
        assert 0 < len(rows) <= 4096 and recent.dropped >= 0
        assert np.all(np.diff(rows["ts_ns"]) >= 0) and rows["ts_ns"][-1] <= now
        assert set(np.unique(rows["kind"]).tolist()) <= {0, 1}
        if len(got):
            assert got["ts_ns"][-1] <= now
    snap = s.calls[-1][1]
    before = fills[fills["ts_ns"] <= snap.ts_ns]
    assert snap[0].fills == len(before)
    if len(before):
        assert snap[0].position == pytest.approx(before["position"][-1])


# ---- publish ------------------------------------------------------------------------------------------

class Validating(Strategy):
    a = Param(1.0, min=0.0, max=10.0)
    n = Param(1, min=0, max=5)
    flag = Param(False)
    counter = State(0)

    def validate(self):
        if self.a > 8 and self.n > 4:
            return "a > 8 needs n <= 4"
        return None

    @fastmm.hot
    def on_book(self, ctx, book):
        pass

    def on_start(self, ctx):
        self.errors = {}

        def attempt(key, **values):
            try:
                ctx.publish(**values)
            except ValueError as e:
                self.errors[key] = str(e)

        attempt("unknown", nope=1)
        attempt("range", a=11.0)
        attempt("type", n=1.5)
        attempt("object", a=[1.0])
        attempt("state", counter=3)
        attempt("too_many", **{f"x{i}": 1 for i in range(33)})
        attempt("instrument", inst=5, a=1.0)
        attempt("symbol", inst="NOPE", a=1.0)
        self.first = ctx.publish(a=9.0, flag=True)
        attempt("validate", n=5)  # a = 9 from the previous publish, n = 5 fails validate()
        self.second = ctx.publish(n=4)


def test_publish_validates_in_the_caller(example_config):
    s = Validating()
    fastmm.run_backtest(_cfg(example_config, 2), data="synthetic", strategy=s)
    e = s.errors
    assert e["unknown"] == "unknown parameter 'nope'"
    assert e["range"] == "parameter 'a': value 11.0 outside [0, 10]"
    assert e["type"].startswith("parameter 'n': cannot parse '1.5' as int")
    assert e["object"].startswith("parameter 'a': ")
    assert e["state"] == "'counter' is a State field; publish takes parameters"
    assert e["too_many"] == "at most 32 parameters per update, got 33"
    assert "instrument 5 is not in the instrument table" in e["instrument"]
    assert "'NOPE' is not in the instrument table" in e["symbol"]
    assert e["validate"] == "a > 8 needs n <= 4"
    assert s.first is True and s.second is True


class Partial(Strategy):
    a = Param(0, min=0, max=100)
    b = Param(0, min=0, max=100)

    @fastmm.hot
    def on_params(self, ctx, book):
        if self.a == 7 and self.b == 9:
            ctx.fail(79)

    def on_start(self, ctx):
        self.start_ns = ctx.now_ns
        ctx.publish(a=7)

    @fastmm.every("1s")
    def later(self, ctx):
        if ctx.now_ns - self.start_ns == 2 * SECOND:
            ctx.publish(b=9)  # a keeps 7
        else:
            ctx.publish()


@pytest.mark.parametrize("delay_ms", [0, 250])
def test_a_partial_publish_keeps_the_other_values_and_slow_delay_ms_delays_it(example_config,
                                                                              delay_ms):
    s = Partial()
    with pytest.raises(fastmm.StrategyError) as info:
        fastmm.run_backtest(_cfg(example_config, 5), data="synthetic", strategy=s,
                            slow_delay_ms=delay_ms)
    assert info.value.fail_code == 79 and info.value.hook == "on_params"
    assert info.value.now_ns == s.start_ns + 2 * SECOND + delay_ms * 1_000_000


class PerInstrument(Strategy):
    a = Param(0, min=0, max=100)

    @fastmm.hot
    def on_params(self, ctx, book):
        if self.a == 5:
            ctx.fail(100 + ctx.instrument)

    def on_start(self, ctx):
        ctx.publish(inst=self.target, a=5)


def test_a_publish_for_one_instrument_changes_only_that_instrument(two_instruments):
    for target, code in (("ETHUSDT", 101), (1, 101), (None, 100)):
        s = PerInstrument()
        s.target = target
        with pytest.raises(fastmm.StrategyError) as info:
            fastmm.run_backtest(two_instruments, data="synthetic", strategy=s)
        assert info.value.fail_code == code, target


class Threaded(Strategy):
    b = Param(0, min=0, max=100)

    @fastmm.hot
    def on_params(self, ctx, book):
        if self.b == 42:
            ctx.fail(42)

    def on_start(self, ctx):
        self.published = threading.Event()
        self.accepted = None

        def model():
            self.accepted = self.publish(b=42)
            self.published.set()

        self.thread = threading.Thread(target=model)
        self.thread.start()

    @fastmm.every("100ms")
    def wait(self, ctx):
        self.published.wait(10)  # holds simulated time until the model thread has published
        ctx.publish()


def test_strategy_publish_from_another_thread(example_config):
    s = Threaded()
    assert s.publish(b=1) is False  # not running
    with pytest.raises(fastmm.StrategyError) as info:
        fastmm.run_backtest(_cfg(example_config, 5), data="synthetic", strategy=s)
    s.thread.join(5)
    assert s.accepted is True and info.value.fail_code == 42
    assert s.publish(b=1) is False  # the session has stopped


# ---- max_param_age_ms ---------------------------------------------------------------------------------

class PublishOnce(Strategy):
    @fastmm.hot
    def on_book(self, ctx, book):
        if book.valid:
            ctx.quote(book.mid * 0.999, book.mid * 1.001, 0.001)

    def on_start(self, ctx):
        self.start_ns = ctx.now_ns
        self.enabled = {}
        if self.publish_at_start:
            ctx.publish()

    @fastmm.every("500ms")
    def watch(self, ctx):
        self.enabled[ctx.now_ns - self.start_ns] = ctx.snapshot().quoting_enabled


def _publish_once(example_config, publish_at_start=True, **kwargs):
    s = PublishOnce()
    s.publish_at_start = publish_at_start
    r = fastmm.run_backtest(_cfg(example_config, 6), data="synthetic", strategy=s, **kwargs)
    return s, r


def test_max_param_age_ms_defaults_to_three_periods_for_slow_methods(example_config):
    assert slow_decl.default_max_param_age_ms([100_000_000]) == 1000
    assert slow_decl.default_max_param_age_ms([5 * SECOND, 2 * SECOND]) == 6000

    s, _ = _publish_once(example_config)  # 3 x 500 ms = 1500 ms
    assert s.enabled[SECOND] is True and s.enabled[2 * SECOND] is False

    s, _ = _publish_once(example_config, max_param_age_ms=4000)
    assert s.enabled[3 * SECOND] is True and s.enabled[5 * SECOND] is False

    cfg_age = example_config.copy()
    cfg_age.max_param_age_ms = 2500
    s = PublishOnce()
    s.publish_at_start = True
    fastmm.run_backtest(_cfg(cfg_age, 6), data="synthetic", strategy=s)
    assert s.enabled[2 * SECOND] is True and s.enabled[3 * SECOND] is False

    s, _ = _publish_once(example_config, max_param_age_ms=0)
    assert all(s.enabled.values())

    _, never = _publish_once(example_config, publish_at_start=False)
    assert never.outbound_messages == 0  # quoting is disabled before the first publish
    _, off = _publish_once(example_config, publish_at_start=False, max_param_age_ms=0)
    assert off.outbound_messages > 0


# ---- failures and timing ------------------------------------------------------------------------------

class SlowDrainer(Recorder):
    @fastmm.every("30s")
    def look(self, ctx):
        ctx.publish()


def test_a_full_fills_ring_stops_the_run(example_config):
    with pytest.raises(fastmm.StrategyError, match=r"fills ring \(2 fills\) was full") as info:
        fastmm.run_backtest(_cfg(example_config, 30), data="synthetic", strategy=SlowDrainer,
                            fills_capacity=2)
    assert info.value.slow_failure == "fills overflow"
    assert info.value.result is not None and len(info.value.result.fills["ts"]) >= 3


class Raises(Strategy):
    @fastmm.hot
    def on_book(self, ctx, book):
        pass

    @fastmm.every("1s")
    def model(self, ctx):
        if ctx.now_ns > self.start_ns:
            raise ZeroDivisionError("model diverged")

    def on_start(self, ctx):
        self.start_ns = ctx.now_ns
        ctx.publish()


def test_an_exception_in_a_slow_method_stops_the_run(example_config):
    with pytest.raises(fastmm.StrategyError, match="Raises.model raised ZeroDivisionError") as info:
        fastmm.run_backtest(_cfg(example_config, 10), data="synthetic", strategy=Raises)
    assert info.value.hook == "model" and info.value.slow_failure == "exception"
    assert isinstance(info.value.__cause__, ZeroDivisionError)
    assert info.value.result.end_ts - info.value.result.start_ts < 2 * SECOND


class Sleeper(Strategy):
    @fastmm.hot
    def on_book(self, ctx, book):
        pass

    @fastmm.every("1s")
    def nap(self, ctx):
        time.sleep(0.003)
        ctx.publish()


def test_the_result_reports_slow_wall_time_and_warns_below_the_delay(example_config):
    with pytest.warns(RuntimeWarning, match=r"Sleeper.nap takes .* pass slow_delay_ms=\d+ or more"):
        r = fastmm.run_backtest(_cfg(example_config, 5), data="synthetic", strategy=Sleeper)
    t = r.slow_methods["nap"]
    assert t["calls"] >= 5 and t["p50_ms"] >= 3.0 and t["max_ms"] >= t["p99_ms"] >= t["p50_ms"]
    with warnings.catch_warnings():
        warnings.simplefilter("error", RuntimeWarning)
        fastmm.run_backtest(_cfg(example_config, 5), data="synthetic", strategy=Sleeper,
                            slow_delay_ms=100)


# ---- replay -------------------------------------------------------------------------------------------

def test_a_hot_and_slow_journal_replays_to_the_same_hash(example_config, tmp_path):
    cfg = _cfg(example_config, 60)
    cfg.journal_out = str(tmp_path / "hot_slow.fmj")
    r = fastmm.run_backtest(cfg, data="synthetic", strategy=HotSlowMM, slow_delay_ms=5)
    assert r.outbound_messages > 0
    meta = fastmm.inspect_journal(cfg.journal_out)["strategy_meta"]
    assert meta["class"] == "hot_slow_mm:HotSlowMM" and meta["param.quote_qty"] == "0.001"
    assert meta["max_param_age_ms"] == "3000" and len(meta["hot_source_sha256"]) == 64

    same = fastmm.replay(cfg.journal_out, HotSlowMM, verify=True)
    assert same.ok and not same.what_if, same
    assert same.outbound_sha256 == same.recorded_sha256 == r.outbound_sha256

    skipped = fastmm.replay(cfg.journal_out, HotSlowMM, param_updates=False)
    assert not skipped.ok and skipped.what_if

    class Wider(HotSlowMM):
        @fastmm.hot
        def on_book(self, ctx, book):
            if book.valid:
                ctx.quote(book.mid * 0.999, book.mid * 1.001, self.quote_qty)

    Wider.__qualname__ = HotSlowMM.__qualname__
    Wider.__module__ = HotSlowMM.__module__
    changed = fastmm.replay(cfg.journal_out, Wider)
    assert changed.what_if and len(changed.what_if_reasons) == 1
    assert changed.what_if_reasons[0].startswith("hot-hook source hash")


# ---- class checks, the fixed-point twin and the live loop -----------------------------------------

def test_class_checks_for_slow_methods():
    class Ok(Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            pass

        def on_start(self, ctx):
            pass

        def on_stop(self, ctx):
            pass

        @fastmm.every("1s", timeout="2s")
        def model(self, ctx):
            pass

    spec = Ok._fastmm_hot
    assert spec.has_slow and list(spec.slow) == ["model"]
    assert set(spec.lifecycle) == {"on_start", "on_stop"} and spec.periods_ns() == [SECOND]

    with pytest.raises(TypeError, match="no @fastmm.hot hooks"):
        class NoHot(Strategy):
            @fastmm.every("1s")
            def model(self, ctx):
                pass

    with pytest.raises(TypeError, match=r"a slow method is model\(self, ctx\)"):
        class Signature(Strategy):
            on_book = fastmm.hot(_noop)

            @fastmm.every("1s")
            def model(self):
                pass

    with pytest.raises(TypeError, match=r"expected on_start\(self, ctx\)"):
        class Start(Strategy):
            on_book = fastmm.hot(_noop)

            def on_start(self):
                pass

    with pytest.raises(TypeError, match="needs a name that is not a hook name"):
        class Named(Strategy):
            on_book = fastmm.hot(_noop)

            @fastmm.every("1s")
            def on_fill(self, ctx):
                pass

    with pytest.raises(ValueError, match="timeout="):
        fastmm.every("1s", timeout="soon")
    with pytest.raises(TypeError, match=r"@fastmm.every\('1s'\)"):
        fastmm.every(_noop)

    with pytest.raises(TypeError, match="taken by Strategy.publish"):
        class Reserved(Strategy):
            inst = Param(1)
            on_book = fastmm.hot(_noop)

    with pytest.raises(TypeError, match="at most 32"):
        type("Many", (Strategy,), {**{f"p{i}": Param(1.0) for i in range(33)},
                                   "on_book": fastmm.hot(_noop)})


def test_the_engine_computes_the_raw_twin_like_the_initial_record():
    rng = np.random.default_rng(7)
    values = [0.0, -0.0, 0.002, 2.5e-8, -5e-9, 1e-9, 0.1, 1 / 3, 2 / 3, 123456.78901234, 9.2e10,
              -9.2e10, 1e-300]
    values += rng.uniform(-1e5, 1e5, 2000).tolist()
    values += np.round(rng.uniform(-100, 100, 2000), 9).tolist()
    values += rng.uniform(-1e-7, 1e-7, 1000).tolist()
    for v in values:
        assert fastmm._core._hot_fixed_raw(v) == hot_decl.fixed_raw(v, "x"), repr(v)


class LiveLoop(Strategy):
    x = Param(1.0, min=0.0, max=10.0)

    @fastmm.hot
    def on_book(self, ctx, book):
        pass

    def on_start(self, ctx):
        self.calls = 0
        self.stopped = False

    @fastmm.every("20ms", timeout="1s")
    def tick(self, ctx):
        self.calls += 1
        if self.calls == self.raise_at:
            raise RuntimeError("boom")

    def on_stop(self, ctx):
        self.stopped = True


def _live(raise_at, seconds):
    s = LiveLoop()
    s.raise_at = raise_at
    channel = fastmm._core._SlowChannel(instruments=1, symbols=["BTCUSDT"])
    runner = slow_runner.SlowRunner(s, LiveLoop._fastmm_hot)
    stop = threading.Event()
    errors = []

    def body():
        try:
            runner.run_thread(channel, slow_runner.WallClock(), stop)
        except RuntimeError as e:
            errors.append(e)

    thread = threading.Thread(target=body)
    thread.start()
    thread.join(seconds)
    stop.set()
    thread.join(5)
    assert not thread.is_alive()
    return s, runner, channel, errors


def test_the_live_runner_loop_uses_the_wall_clock_and_records_failures():
    s, runner, channel, errors = _live(raise_at=0, seconds=0.2)
    assert not errors and s.stopped and 5 <= s.calls <= 12
    assert channel.failure() == 0 and channel.last_heartbeat > 0
    assert s.publish(x=2.0) is True and channel.published == 1
    channel.close()
    assert s.publish(x=3.0) is False

    s, runner, channel, errors = _live(raise_at=3, seconds=5)
    assert [str(e) for e in errors] == ["boom"] and s.calls == 3 and not s.stopped
    assert runner.failed_method == "tick" and channel.failure() == slow_abi.FAILURE_EXCEPTION

    watched = fastmm._core._SlowChannel()
    watched.begin_call(time.monotonic_ns(), 1_000_000)
    time.sleep(0.01)
    assert watched.check(time.monotonic_ns()) == slow_abi.FAILURE_TIMEOUT
