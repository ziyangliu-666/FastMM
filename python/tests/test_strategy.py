"""The Python strategy API: hooks, parameters, context, views (ADR-0012, section 7)."""

import math
import warnings

import pytest

import fastmm
from conftest import FIXTURE_FMJ
from fastmm import BUY, SELL, Param, Strategy


def _cfg(example_config, seconds=10):
    cfg = example_config.copy()
    cfg.clear_params()
    cfg.duration_s = seconds
    return cfg


# ---- hooks ---------------------------------------------------------------------------------------

def test_only_defined_hooks_are_looked_up_and_called(example_config):
    looked_up = []

    class OnlyBook(Strategy):
        def __getattribute__(self, name):
            if name.startswith("on_"):
                looked_up.append(name)
            return object.__getattribute__(self, name)

        def on_book(self, ctx, inst, book):
            self.books = self.__dict__.get("books", 0) + 1

    s = OnlyBook()
    r = fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=s)
    assert OnlyBook.hooks() == ("on_book",)
    assert set(looked_up) == {"on_book"}
    assert s.books == r.engine_stats()["book_updates"] > 0


def test_every_hook_name_and_order_matches_the_cpp_table():
    class All(Strategy):
        def on_quoting(self, ctx, enabled): ...
        def on_start(self, ctx): ...
        def on_stop(self, ctx): ...
        def on_book(self, ctx, inst, book): ...
        def on_book_ticker(self, ctx, inst, msg): ...
        def on_trade(self, ctx, inst, trade): ...
        def on_option_ticker(self, ctx, inst, msg): ...
        def on_fill(self, ctx, fill): ...
        def on_order_update(self, ctx, update): ...
        def on_timer(self, ctx, timer_id, tag): ...
        def on_connection(self, ctx, msg): ...

    assert All.hooks() == tuple(fastmm.strategy.HOOKS)
    assert All.hooks()[0] == "on_start" and All.hooks()[-1] == "on_quoting"


def test_hook_signatures_and_near_misses_are_checked(example_config):
    class Wrong(Strategy):
        def on_trade(self, ctx, trade):
            pass

    with pytest.raises(TypeError, match=r"on_trade has the wrong signature; expected "
                                        r"on_trade\(self, ctx, inst, trade\)"):
        fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=Wrong)

    class Defaults(Strategy):  # extra parameters with defaults are fine
        def on_timer(self, ctx, timer_id, tag, extra=None):
            pass

    assert Defaults.hooks() == ("on_timer",)

    class Static(Strategy):
        @staticmethod
        def on_book(ctx, inst, book):
            pass

    with pytest.raises(TypeError, match="must be a method"):
        Static.hooks()

    class Near(Strategy):
        def on_fills(self, ctx, fill):
            pass

    with pytest.warns(UserWarning, match="on_fills is not a hook; did you mean on_fill"):
        assert Near.hooks() == ()

    class Allowed(Near):
        fastmm_allow_near_miss_names = True

    with warnings.catch_warnings():
        warnings.simplefilter("error")
        assert Allowed.hooks() == ()


# ---- parameters ----------------------------------------------------------------------------------

def _mirror_of(name):
    """A Python class whose Params copy the int / double / bool parameters of a C++ strategy."""
    attrs = {}
    for p in fastmm.strategies()[name]:
        if p["type"] in ("int", "double", "bool"):
            attrs[p["name"]] = Param(p["default"], min=p["min"], max=p["max"], doc=p["doc"])
    return type("Mirror", (Strategy,), attrs)


def _error(cfg, strategy, params):
    with pytest.raises(ValueError) as e:
        fastmm.run_backtest(cfg, data=FIXTURE_FMJ, strategy=strategy, params=params)
    return str(e.value)


@pytest.mark.parametrize(
    "key, value",
    [
        ("gamma", 1000.0),
        ("gamma", 0.0),
        ("gamma", "abc"),
        ("gamma", "nan"),
        ("gamma", "1e3"),
        ("kappa", "1e7"),
        ("sigma_init", -1.5),
        ("min_half_spread_ticks", 2.5),
        ("min_half_spread_ticks", "-1"),
        ("min_half_spread_ticks", True),
        ("estimate_kappa", "maybe"),
        ("estimate_kappa", 2),
        ("no_such_parameter", 1),
    ],
)
def test_parameter_errors_match_the_cpp_messages(example_config, key, value):
    cfg = _cfg(example_config)
    cpp = _error(cfg, "avellaneda_stoikov", {key: value})
    mirror = _mirror_of("avellaneda_stoikov")
    py = _error(cfg, mirror, {key: value})
    assert cpp.startswith("avellaneda_stoikov: ")
    assert py.startswith("py:Mirror: ")
    assert py[len("py:Mirror: "):] == cpp[len("avellaneda_stoikov: "):]


def test_valid_parameters_parse_like_cpp(example_config):
    mirror = _mirror_of("avellaneda_stoikov")
    s = mirror()
    s.configure({"gamma": "2.5e-1", "min_half_spread_ticks": "3e0", "estimate_kappa": "yes",
                 "kappa": 7})
    assert s.gamma == 0.25 and isinstance(s.gamma, float)
    assert s.min_half_spread_ticks == 3 and isinstance(s.min_half_spread_ticks, int)
    assert s.estimate_kappa is True
    assert s.kappa == 7.0 and isinstance(s.kappa, float)
    assert s.param_values()["gamma"] == "0.25"


def test_params_schema_validate_and_assignment(example_config):
    class V(Strategy):
        a = Param(1, min=0, max=10, doc="first")
        b = Param(2.5, max=10.0)
        flag = Param(False)

        def validate(self):
            return "a must not exceed b" if self.a > self.b else None

    assert V.schema() == [
        {"name": "a", "type": "int", "default": 1, "min": 0, "max": 10, "doc": "first"},
        {"name": "b", "type": "double", "default": 2.5, "min": None, "max": 10.0, "doc": ""},
        {"name": "flag", "type": "bool", "default": False, "min": False, "max": True, "doc": ""},
    ]
    assert V.strategy_name() == "py:" + V.__qualname__
    cfg = _cfg(example_config)
    msg = _error(cfg, V, {"a": 5})
    assert msg == f"py:{V.__qualname__}: a must not exceed b"
    assert _error(cfg, V, {"b": 11}).endswith("parameter 'b': value 11 outside [-inf, 10]")

    s = V()
    with pytest.raises(ValueError, match="a must not exceed b"):
        s.configure({"a": 5})
    assert s.a == 1  # unchanged after an error
    with pytest.raises(ValueError, match=r"parameter 'a': value 11 outside \[0, 10\]"):
        s.a = 11
    s.a = 2
    assert s.a == 2 and V().a == 1
    with pytest.raises(TypeError):
        Param("text")
    with pytest.raises(ValueError, match="outside"):
        Param(5, min=6)


def test_config_params_and_params_argument_are_merged(example_config):
    class P(Strategy):
        half_spread_bps = Param(1.0)
        levels = Param(1)
        quote_qty = Param(0.01)
        skew_bps_per_unit = Param(0.0)
        max_inventory = Param(0.0)
        requote_threshold_ticks = Param(0)
        pull_on_stale_ms = Param(0)
        level_step_ticks = Param(1)

    s = P()
    r = fastmm.run_backtest(example_config, data=FIXTURE_FMJ, strategy=s, params={"levels": 3})
    assert s.levels == 3 and s.quote_qty == 0.002  # from the TOML file
    assert r.params["levels"] == "3" and r.params["quote_qty"] == "0.002"

    cfg = example_config.copy()
    cfg.set_param("levels", 2)
    by_cfg = fastmm.run_backtest(cfg, data=FIXTURE_FMJ, strategy="basic_mm")
    by_arg = fastmm.run_backtest(example_config, data=FIXTURE_FMJ, strategy="basic_mm",
                                 params={"levels": 2})
    assert by_arg.outbound_sha256 == by_cfg.outbound_sha256
    assert example_config.params["levels"] == "1"  # the caller's config is not modified


# ---- entry point ---------------------------------------------------------------------------------

def test_strategy_argument_forms(example_config):
    class Noop(Strategy):
        def on_start(self, ctx):
            self.started = True

    cfg = _cfg(example_config)
    r = fastmm.run_backtest(cfg, FIXTURE_FMJ, Noop)  # positional, class: fresh instance
    assert r.strategy == "py:" + Noop.__qualname__ and r.outbound_messages == 0
    s = Noop()
    fastmm.run_backtest(cfg, FIXTURE_FMJ, strategy=s)
    assert s.started
    with pytest.raises(ValueError, match="already run"):
        fastmm.run_backtest(cfg, FIXTURE_FMJ, strategy=s)
    with pytest.raises(TypeError, match="registered strategy name"):
        fastmm.run_backtest(cfg, FIXTURE_FMJ, strategy=42)
    with pytest.raises(TypeError, match="registered strategy name"):
        fastmm.run_backtest(cfg, FIXTURE_FMJ, strategy=object)
    with pytest.raises(TypeError):
        fastmm.sweep(cfg, {"x": [1]}, data=FIXTURE_FMJ, strategy=Noop)


# ---- context and views ---------------------------------------------------------------------------

def test_set_quotes_rounds_prices_passively_and_quantities_down(example_config):
    class Rounding(Strategy):
        def on_book(self, ctx, inst, book):
            if hasattr(self, "accepted") or not book.valid:
                return
            bb, ba = book.best_bid_raw[0], book.best_ask_raw[0]
            tick = inst.tick_raw
            self.expected = {BUY: bb - 2 * tick, SELL: ba + 2 * tick}
            self.accepted = ctx.set_quotes(
                inst, [((bb - 1_300_000) / 1e8, 0.0020001)], [((ba + 1_300_000) / 1e8, 0.0020009)])
            bid = ctx.working_quote(inst, BUY)
            self.bid = (bid.price_raw, bid.qty_raw, bid.side, bid.state)

    s = Rounding()
    r = fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=s)
    assert s.accepted
    assert s.bid == (s.expected[BUY], 200_000, BUY, "PendingNew")
    new = r.orders["kind"] == 0
    sent = dict(zip(r.orders["side"][new][:2].tolist(), r.orders["price"][new][:2].tolist()))
    assert sent == s.expected
    assert r.orders["qty"][new][:2].tolist() == [200_000, 200_000]


def test_set_quotes_argument_errors(example_config):
    class Bad(Strategy):
        def on_book(self, ctx, inst, book):
            if hasattr(self, "errors") or not book.valid:
                return
            self.errors = {}
            px = book.best_bid[0] - 1.0

            def capture(key, fn):
                try:
                    self.errors[key] = ("ok", fn())
                except Exception as e:  # noqa: BLE001
                    self.errors[key] = (type(e).__name__, str(e))

            capture("nine", lambda: ctx.set_quotes(inst, [(px - i, 0.001) for i in range(9)]))
            capture("nan_price", lambda: ctx.set_quotes(inst, [(math.nan, 0.001)]))
            capture("inf_qty", lambda: ctx.set_quotes(inst, None, [(px + 3, math.inf)]))
            capture("negative_qty", lambda: ctx.set_quotes(inst, [(px, -0.001)]))
            capture("negative_raw_qty", lambda: ctx.set_quotes_raw(inst, [(int(px * 1e8), -1)]))
            capture("raw_float", lambda: ctx.set_quotes_raw(inst, [(1.5, 100)]))
            capture("not_pair", lambda: ctx.set_quotes(inst, [(px,)]))
            capture("not_sequence", lambda: ctx.set_quotes(inst, 5))
            capture("unknown_instrument", lambda: ctx.set_quotes(7, []))
            capture("bad_side", lambda: ctx.open_qty(inst, 2))
            capture("dropped", lambda: ctx.set_quotes(inst, [(0.0, 0.001), (px, 0.0)], []))
            capture("quote_tag", lambda: ctx.send(inst, BUY, px, 0.001, tag=0x5100_0001))
            capture("unknown_replace", lambda: ctx.replace(123456, px, 0.001))
            capture("unknown_cancel", lambda: ctx.cancel(123456))
            capture("zero_timer", lambda: ctx.every(0))
            capture("randint", lambda: ctx.randint(5, 1))

    s = Bad()
    fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=s)
    e = s.errors
    assert e["nine"] == ("ValueError", "fastmm: bids has 9 levels; at most 8 per side")
    assert e["nan_price"][0] == "ValueError" and "bids[0] price must be finite" in e["nan_price"][1]
    assert e["inf_qty"][0] == "ValueError" and "asks[0] qty must be finite" in e["inf_qty"][1]
    assert e["negative_qty"] == ("ValueError", "fastmm: bids[0] qty is negative")
    assert e["negative_raw_qty"] == ("ValueError", "fastmm: bids[0] qty is negative")
    assert e["raw_float"][0] == "TypeError" and "int raw value" in e["raw_float"][1]
    assert e["not_pair"][0] == "TypeError" and "(price, qty) pair" in e["not_pair"][1]
    assert e["not_sequence"][0] == "TypeError"
    assert e["unknown_instrument"][0] == "ValueError" and "not in the instrument table" in e[
        "unknown_instrument"][1]
    assert e["bad_side"][0] == "ValueError" and "fastmm.BUY" in e["bad_side"][1]
    assert e["dropped"] == ("ok", True)  # like DesiredQuotes::bid: non-positive levels dropped
    assert e["quote_tag"] == ("OrderRejected", "order rejected: InvalidTag")
    assert e["unknown_replace"] == ("OrderRejected", "order rejected: UnknownOrder")
    assert e["unknown_cancel"] == ("ok", False)
    assert e["zero_timer"][0] == "ValueError"
    assert e["randint"][0] == "ValueError"


def test_context_api(example_config):
    class Probe(Strategy):
        def on_start(self, ctx):
            (inst,) = ctx.instruments
            self.inst = inst
            self.start_ns = ctx.now_ns
            self.ids = (ctx.instrument(0) is inst, ctx.instrument(inst) is inst,
                        ctx.contains(0), ctx.contains(inst), ctx.contains(1), ctx.contains(-1))
            self.flags = (ctx.quoting_enabled, ctx.killed)
            self.random = [ctx.random() for _ in range(3)] + [ctx.randint(1, 6) for _ in range(3)]
            self.once = ctx.once(1_000_000, tag=7)
            self.every = ctx.every(500_000_000, tag=2**63)
            self.cancelled = ctx.cancel_timer(ctx.once(5_000_000_000))
            self.timers = []

        def on_timer(self, ctx, timer_id, tag):
            self.timers.append((timer_id, tag))
            if len(self.timers) == 3:
                ctx.cancel_timer(self.every)

        def on_book(self, ctx, inst, book):
            if hasattr(self, "order_id") or not book.valid:
                return
            p = ctx.position(inst)
            self.book = (book.instrument, book.mid, book.mid_raw, book.spread_raw, book.best_bid,
                         book.best_bid_raw, book.level(BUY, 0), book.level_raw(SELL, 0),
                         book.depth(BUY) > 0, book.last_update_ns == ctx.now_ns,
                         book.microprice() > 0, -1.0 <= book.imbalance(5) <= 1.0,
                         book.level(BUY, 1000))
            self.position = (p.qty, p.qty_raw, p.fills, p.instrument)
            price = book.best_bid[0] - 5.0
            self.order_id = ctx.send(inst, BUY, price, 0.001, post_only=True, tag=42)
            o = ctx.order(self.order_id)
            self.order = (o.id, o.side, o.state, o.price, o.qty_raw, o.user_tag, o.post_only)
            self.open_qty = (ctx.open_qty(inst, BUY), ctx.open_qty_raw(inst, SELL))
            self.portfolio = ctx.portfolio().net

        def on_order_update(self, ctx, u):
            if u.order_id == getattr(self, "order_id", None) and u.state == "Live" and \
                    not hasattr(self, "live"):
                self.live = (u.instrument, u.side, u.prev_state, u.user_tag, u.known, u.leaves_raw)
                self.cancel_ok = ctx.cancel(u.order_id)

        def on_stop(self, ctx):
            self.stop_ns = ctx.now_ns

    s = Probe()
    fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=s)
    inst = s.inst
    assert (inst.id, inst.symbol, inst.venue, inst.base, inst.quote) == (0, "BTCUSDT", 0, "BTC",
                                                                         "USDT")
    assert (inst.tick, inst.tick_raw, inst.lot_raw, inst.asset_class) == (0.01, 1_000_000, 1_000,
                                                                          "Spot")
    assert inst == 0 and hash(inst) == 0 and int(inst) == 0 and [10, 20][inst] == 10
    assert inst.round_price(60000.005, BUY) == 60000.0
    assert inst.round_price(60000.005, SELL) == 60000.01
    assert inst.round_qty_raw(200_999) == 200_000
    assert s.ids == (True, True, True, True, False, False)
    assert s.flags == (True, False)
    assert all(0.0 <= x < 1.0 for x in s.random[:3]) and all(1 <= x <= 6 for x in s.random[3:])
    assert s.cancelled is True
    assert (s.once, 7) in s.timers and s.timers.count((s.every, 2**63)) == 2
    mid, mid_raw = s.book[1], s.book[2]
    assert s.book[0] == 0 and mid_raw > 0 and mid == mid_raw / 1e8
    assert s.book[4] == (s.book[5][0] / 1e8, s.book[5][1] / 1e8) and s.book[6] == s.book[4]
    assert s.book[8:12] == (True, True, True, True) and s.book[12] == (0.0, 0.0)
    assert s.position == (0.0, 0, 0, 0)
    assert s.order[1:3] == (BUY, "PendingNew") and s.order[4:] == (100_000, 42, True)
    assert s.open_qty == (0.001, 0)
    assert s.live[:3] == (0, BUY, "PendingNew") and s.live[3:] == (42, True, 100_000)
    assert s.cancel_ok is True
    assert s.stop_ns >= s.start_ns

    t = Probe()
    fastmm.run_backtest(_cfg(example_config), data=FIXTURE_FMJ, strategy=t)
    assert t.random == s.random  # the engine RNG is seeded


def test_fill_view_mirrors_the_cpp_fill(example_config):
    class Taker(Strategy):
        def on_book(self, ctx, inst, book):
            if book.valid and not hasattr(self, "order_id"):
                self.order_id = ctx.send(inst, BUY, book.best_ask[0], 0.002)

        def on_fill(self, ctx, fill):
            if hasattr(self, "fill"):
                return
            u = fill.update
            self.fill = (fill.instrument, fill.side, fill.price, fill.qty_raw,
                         fill.position_delta_raw, fill.fee > 0, fill.fee_converted, fill.liquidity,
                         fill.known, fill.late, fill.order_done, fill.order_id,
                         u is not None and u.order_id)
            self.position_after = ctx.position(fill.instrument).qty_raw

    s = Taker()
    r = fastmm.run_backtest(_cfg(example_config, 30), data="synthetic", strategy=s)
    assert r.stats()["taker_fills"] >= 1
    f = s.fill
    assert f[0] == 0 and f[1] == BUY and f[2] > 0 and 0 < f[3] <= 200_000
    assert f[4] == f[3] and s.position_after == f[4]  # position already updated
    assert f[5] and f[6] and f[7] == fastmm.TAKER
    assert f[8] is True and f[9] is False and f[11] == s.order_id == f[12]
