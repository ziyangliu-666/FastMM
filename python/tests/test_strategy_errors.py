"""Python strategy failures, interrupts, stale views and memory (ADR-0012, section 7)."""

import collections
import signal
import sys
import traceback
import tracemalloc

import numpy as np
import pytest

import fastmm
from conftest import EXAMPLE_TOML
from fastmm import BUY, SELL, Strategy


def _cfg(example_config, seconds=30):
    cfg = example_config.copy()
    cfg.clear_params()
    cfg.duration_s = seconds
    return cfg


FAIL_AFTER = {
    "on_start": 1,
    "on_book": 20,
    "on_book_ticker": 20,
    "on_trade": 200,
    "on_fill": 1,
    "on_order_update": 5,
    "on_timer": 3,
    "on_quoting": 1,
    "on_stop": 1,
}


class Quoter(Strategy):
    """Quotes the touch on every book so fills and order updates happen; raises in `fail_in`."""

    fail_in = ""

    def __init__(self):
        super().__init__()
        self.calls = collections.Counter()
        self.live_at_failure = 0

    def fail_maybe(self, ctx, hook):
        self.calls[hook] += 1
        if hook != self.fail_in or self.calls[hook] != FAIL_AFTER[hook]:
            return
        if hook not in ("on_start", "on_stop"):
            for side in (BUY, SELL):
                q = ctx.working_quote(0, side)
                if q is not None and q.state in ("Live", "PartiallyFilled"):
                    self.live_at_failure += 1
        raise ValueError(f"boom in {hook}")

    def on_start(self, ctx):
        ctx.every(50_000_000)
        self.fail_maybe(ctx, "on_start")

    def on_stop(self, ctx):
        self.fail_maybe(ctx, "on_stop")

    def on_book(self, ctx, inst, book):
        if book.valid:
            ctx.set_quotes(inst, [(book.best_bid[0], 0.002)], [(book.best_ask[0], 0.002)])
        self.fail_maybe(ctx, "on_book")

    def on_book_ticker(self, ctx, inst, msg):
        self.fail_maybe(ctx, "on_book_ticker")

    def on_trade(self, ctx, inst, trade):
        self.fail_maybe(ctx, "on_trade")

    def on_fill(self, ctx, fill):
        self.fail_maybe(ctx, "on_fill")

    def on_order_update(self, ctx, update):
        self.fail_maybe(ctx, "on_order_update")

    def on_timer(self, ctx, timer_id, tag):
        self.fail_maybe(ctx, "on_timer")

    def on_quoting(self, ctx, enabled):
        self.fail_maybe(ctx, "on_quoting")


class KillQuoter(Quoter):
    """Takes liquidity once; with a tiny max_loss the kill switch trips and on_quoting fires."""

    def on_book(self, ctx, inst, book):
        if book.valid and not getattr(self, "took", False):
            self.took = True
            ctx.send(inst, BUY, book.best_ask[0], 0.002)
        self.fail_maybe(ctx, "on_book")


def _kill_config(seconds):
    text = EXAMPLE_TOML.read_text().replace('max_loss = "100"', 'max_loss = "0.000001"')
    assert "0.000001" in text
    cfg = fastmm.BacktestConfig.from_toml_string(text)
    cfg.clear_params()
    cfg.duration_s = seconds
    return cfg


@pytest.mark.parametrize("hook", list(FAIL_AFTER))
def test_a_raising_hook_stops_the_run_with_strategy_error(example_config, hook):
    base = KillQuoter if hook == "on_quoting" else Quoter
    cfg = _kill_config(30) if hook == "on_quoting" else _cfg(example_config)
    full_strategy = base()
    full = fastmm.run_backtest(cfg, data="synthetic", strategy=full_strategy)
    assert full_strategy.calls[hook] >= FAIL_AFTER[hook], "the data must reach the hook"

    cls = type("Fail_" + hook, (base,), {"fail_in": hook})
    s = cls()
    with pytest.raises(fastmm.StrategyError) as ei:
        fastmm.run_backtest(cfg, data="synthetic", strategy=s)
    err = ei.value
    assert err.hook == hook
    assert f"Fail_{hook}.{hook} raised ValueError: boom in {hook}" in str(err)
    cause = err.__cause__
    assert isinstance(cause, ValueError) and str(cause) == f"boom in {hook}"
    frames = [f.name for f in traceback.extract_tb(cause.__traceback__)]
    assert "fail_maybe" in frames
    r = err.result
    assert isinstance(r, fastmm.BacktestResult)
    assert r.strategy == f"py:{cls.__qualname__}"
    if hook == "on_stop":
        assert r.md_events == full.md_events
    else:
        assert r.md_events < full.md_events  # stopped after the failing event
    assert s.calls[hook] == FAIL_AFTER[hook]  # never called again
    if s.live_at_failure:
        at_failure = (r.orders["ts"] == err.now_ns) & (r.orders["kind"] == 1)
        assert int(at_failure.sum()) >= s.live_at_failure  # quotes pulled


def test_keyboard_interrupt_from_a_timer_propagates_with_the_partial_result(example_config):
    class Interrupt(Strategy):
        def on_start(self, ctx):
            ctx.every(200_000_000)

        def on_timer(self, ctx, timer_id, tag):
            signal.raise_signal(signal.SIGINT)

    cfg = _cfg(example_config)
    full = fastmm.run_backtest(cfg, data="synthetic", strategy=Quoter)
    with pytest.raises(KeyboardInterrupt) as ei:
        fastmm.run_backtest(cfg, data="synthetic", strategy=Interrupt)
    r = ei.value.result
    assert 0 < r.md_events < full.md_events


def test_signal_between_hooks_is_seen_by_the_periodic_check(example_config):
    class Quiet(Strategy):  # no hook runs Python after on_start
        def on_start(self, ctx):
            signal.setitimer(signal.ITIMER_REAL, 0.005)

    def on_alarm(signum, frame):
        raise KeyboardInterrupt("alarm")

    cfg = _cfg(example_config, seconds=3600)
    previous = signal.signal(signal.SIGALRM, on_alarm)
    try:
        with pytest.raises(KeyboardInterrupt, match="alarm") as ei:
            fastmm.run_backtest(cfg, data="synthetic", strategy=Quiet)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)
    assert ei.value.result.end_ts - ei.value.result.start_ts < 3600 * 1_000_000_000


def test_views_are_stale_outside_their_hook_and_the_context_outside_a_run(example_config):
    class Keeper(Strategy):
        def on_start(self, ctx):
            self.ctx = ctx

        def on_book(self, ctx, inst, book):
            if not hasattr(self, "kept") and book.valid:
                self.kept = book

        def on_trade(self, ctx, inst, trade):
            if hasattr(self, "kept") and not hasattr(self, "stale"):
                try:
                    self.kept.mid
                except fastmm.StaleViewError as e:
                    self.stale = str(e)
                self.trade = trade
                self.same_object = ctx.book(inst) is self.kept  # re-stamped for this hook
                self.fresh_mid = self.kept.mid

    s = Keeper()
    fastmm.run_backtest(_cfg(example_config, 10), data="synthetic", strategy=s)
    assert "outside the hook" in s.stale
    assert s.same_object and s.fresh_mid > 0
    assert issubclass(fastmm.StaleViewError, RuntimeError)
    with pytest.raises(fastmm.StaleViewError):
        s.kept.mid
    with pytest.raises(fastmm.StaleViewError):
        s.trade.price
    with pytest.raises(RuntimeError, match="only be used inside a hook"):
        s.ctx.now_ns
    with pytest.raises(RuntimeError, match="only be used inside a hook"):
        s.ctx.book(0)


def test_a_million_events_reuse_views_without_memory_growth(example_config):
    n = 1_000_000
    cols = {
        "ts": np.arange(n, dtype=np.int64) * 1_000 + 1_700_000_000_000_000_000,
        "type": np.full(n, 2, dtype=np.uint8),
        "inst": np.zeros(n, dtype=np.uint32),
        "side": (np.arange(n) % 2).astype(np.int8),
        "price": np.full(n, 6_000_000_000_000, dtype=np.int64),
        "qty": np.full(n, 100_000, dtype=np.int64),
    }

    class Reader(Strategy):
        def on_start(self, ctx):
            self.n = 0
            self.total = 0.0

        def on_trade(self, ctx, inst, trade):
            self.n += 1
            self.total += trade.price * trade.qty
            book = ctx.book(inst)
            if self.n == 1:
                self.views = (trade, book)
            elif self.n == 2:
                self.refs_start = (sys.getrefcount(trade), sys.getrefcount(book))
            elif self.n == 10_000:
                tracemalloc.start()
                self.mem_start = tracemalloc.get_traced_memory()[0]
            elif self.n == n:
                self.mem_end = tracemalloc.get_traced_memory()[0]
                tracemalloc.stop()
                self.refs_end = (sys.getrefcount(trade), sys.getrefcount(book))
                self.same = trade is self.views[0] and book is self.views[1]

    s = Reader()
    try:
        r = fastmm.run_backtest(_cfg(example_config), data=cols, strategy=s)
    finally:
        if tracemalloc.is_tracing():
            tracemalloc.stop()
    assert r.md_events == n and s.n == n
    assert s.total == pytest.approx(n * 60000.0 * 0.001)
    assert s.same
    assert s.refs_end == s.refs_start
    assert s.mem_end - s.mem_start < 256 * 1024
