"""`fastmm init`: the scaffolded project is written, parses and runs."""

import subprocess
import sys

import pytest

import fastmm
from fastmm._scaffold import write_project

FILES = ["config.toml", "strategy.py", "backtest.py", "README.md"]


def test_writes_the_project(tmp_path):
    written = write_project(tmp_path / "my-mm")
    assert sorted(p.name for p in written) == sorted(FILES)
    text = (tmp_path / "my-mm" / "strategy.py").read_text()
    assert "class MyMm(fastmm.Strategy)" in text
    assert "@NAME@" not in text and "@NAME_CLASS@" not in text


def test_refuses_to_overwrite(tmp_path):
    write_project(tmp_path)
    with pytest.raises(FileExistsError):
        write_project(tmp_path)
    write_project(tmp_path, force=True)


def test_config_loads_without_warnings(tmp_path):
    write_project(tmp_path / "mm")
    cfg = fastmm.BacktestConfig.from_toml(tmp_path / "mm" / "config.toml")
    assert not cfg.warnings


def test_the_command_writes_a_project(tmp_path):
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", str(tmp_path / "p")],
                         capture_output=True, text=True, check=True)
    assert "backtest.py" in out.stdout
    assert all((tmp_path / "p" / name).exists() for name in FILES)


def test_the_backtest_runs(tmp_path):
    pytest.importorskip("numba")
    write_project(tmp_path / "mm")
    out = subprocess.run([sys.executable, "backtest.py", "--duration-s", "30"],
                         cwd=tmp_path / "mm", capture_output=True, text=True, check=False)
    assert out.returncode == 0, out.stderr
    assert "fills (maker / taker)" in out.stdout
    assert (tmp_path / "mm" / "runs" / "backtest" / "fills.csv").exists()
