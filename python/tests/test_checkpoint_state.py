"""A Python strategy's state() in the journal's checkpoints, restored by a backtest from the middle
of the session ([backtest] start_at), and refused for another state_version."""
from __future__ import annotations

import struct

import pytest

import fastmm
from fastmm import Strategy

from conftest import EXAMPLE_TOML


def _config(extra_engine: str = "", extra_backtest: str = "") -> fastmm.BacktestConfig:
    text = EXAMPLE_TOML.read_text()
    text = text.replace("[engine]\n", "[engine]\n" + extra_engine, 1)
    text = text.replace("[backtest]\n", "[backtest]\n" + extra_backtest, 1)
    text = text.replace("duration_s = 60 ", "duration_s = 20 ", 1)
    cfg = fastmm.BacktestConfig.from_toml_string(text)
    cfg.clear_params()
    return cfg


class Counter(Strategy):
    """Counts its books; asks for a checkpoint every 40 of them."""

    state_version = 3
    asked: list = []

    def on_start(self, ctx):
        self.books = 0
        self.restored = None

    def on_book(self, ctx, inst, book):
        self.books += 1
        if self.books % 40 == 0:
            type(self).asked.append((ctx.now_ns, self.books))
            ctx.request_checkpoint()

    def state(self):
        return struct.pack("<q", self.books)

    def restore(self, data):
        self.restored = data
        (self.books,) = struct.unpack("<q", data)
        return True


def test_a_backtest_from_mid_session_restores_the_checkpoint_before_it(tmp_path):
    Counter.asked = []
    first = _config()
    first.journal_out = str(tmp_path / "session.fmj")
    fastmm.run_backtest(first, data="synthetic", strategy=Counter())
    assert len(Counter.asked) >= 4

    at_ns, books = Counter.asked[2]
    start_at = (at_ns + 1_000_000) / 1e9
    cfg = _config(extra_backtest=f'initial_state = "journal"\nstart_at = {start_at}\n')
    second = Counter()
    fastmm.run_backtest(cfg, data=first.journal_out, strategy=second)
    assert second.restored == struct.pack("<q", books)


def test_a_checkpoint_of_another_state_version_is_refused(tmp_path):
    Counter.asked = []
    first = _config()
    first.journal_out = str(tmp_path / "session.fmj")
    fastmm.run_backtest(first, data="synthetic", strategy=Counter())
    start_at = (Counter.asked[1][0] + 1_000_000) / 1e9

    class Newer(Counter):
        state_version = 4

    cfg = _config(extra_backtest=f'initial_state = "journal"\nstart_at = {start_at}\n')
    with pytest.raises(ValueError, match="state_version 3 of .*restores version 4"):
        fastmm.run_backtest(cfg, data=first.journal_out, strategy=Newer())
