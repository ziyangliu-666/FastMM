"""`fastmm init`: the scaffolded project is written, parses and runs."""

import subprocess
import sys

import pytest

import fastmm
from fastmm._scaffold import write_project

FILES = ["config.toml", "sim.toml", "live.toml", "strategy.py", "backtest.py", "README.md"]


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


def test_live_config_names_the_class_and_its_params(tmp_path, monkeypatch):
    # No keys in the environment: a backtest of the live config needs none.
    monkeypatch.delenv("FASTMM_BINANCE_API_KEY", raising=False)
    monkeypatch.delenv("FASTMM_BINANCE_API_SECRET", raising=False)
    write_project(tmp_path / "my-mm")
    cfg = fastmm.BacktestConfig.from_toml(tmp_path / "my-mm" / "live.toml")
    assert cfg.strategy == "strategy:MyMm"
    text = (tmp_path / "my-mm" / "strategy.py").read_text()
    for name in cfg.params:
        assert f"    {name} = Param(" in text
    readme = (tmp_path / "my-mm" / "README.md").read_text()
    assert "strategy:MyMm --config live.toml --dry-run" in readme


def test_the_command_writes_a_project(tmp_path):
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", str(tmp_path / "p")],
                         capture_output=True, text=True, check=True)
    assert "backtest.py" in out.stdout
    assert all((tmp_path / "p" / name).exists() for name in FILES)


def test_the_backtest_runs(tmp_path):
    pytest.importorskip("numba")
    write_project(tmp_path / "mm")
    out = subprocess.run([sys.executable, "backtest.py"],
                         cwd=tmp_path / "mm", capture_output=True, text=True, check=False)
    assert out.returncode == 0, out.stderr
    fills = next(ln for ln in out.stdout.splitlines() if ln.strip().startswith("fills (maker"))
    assert int(fills.split()[4]) > 100  # the quotes sit inside a book a few bps wide
    captured = next(ln for ln in out.stdout.splitlines() if "spread captured (bps)" in ln)
    assert float(captured.split()[-1]) > 0.3
    assert "stopped" not in out.stdout  # the default run does not reach [risk] max_loss
    assert (tmp_path / "mm" / "runs" / "backtest" / "fills.csv").exists()


def test_the_production_profile_adds_a_tuned_live_config(tmp_path, monkeypatch):
    monkeypatch.delenv("FASTMM_BINANCE_API_KEY", raising=False)
    written = write_project(tmp_path / "my-mm", profile="production")
    assert sorted(p.name for p in written) == sorted(FILES + ["production.toml"])
    cfg = fastmm.BacktestConfig.from_toml(tmp_path / "my-mm" / "production.toml")
    assert cfg.strategy == "strategy:MyMm"
    text = (tmp_path / "my-mm" / "production.toml").read_text()
    for line in ('spin_mode = "busy"', "lock_memory = true", "cpu_dma_latency_us = 0",
                 'key_type = "ed25519"', 'md_format = "sbe"', "net_cpus = [2]"):
        assert line in text
    assert "@NAME@" not in text and "@NAME_CLASS@" not in text
    readme = (tmp_path / "my-mm" / "README.md").read_text()
    assert "strategy:MyMm --config production.toml --dry-run" in readme


def test_an_unknown_profile_is_refused(tmp_path):
    with pytest.raises(ValueError):
        write_project(tmp_path, profile="fast")
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", str(tmp_path / "p"),
                          "--profile", "fast"], capture_output=True, text=True, check=False)
    assert out.returncode == 2


def test_the_sim_config_trades_the_local_simulator(tmp_path):
    write_project(tmp_path / "my-mm")
    cfg = fastmm.BacktestConfig.from_toml(tmp_path / "my-mm" / "sim.toml")
    assert cfg.strategy == "strategy:MyMm"
    text = (tmp_path / "my-mm" / "sim.toml").read_text()
    assert 'ws_api_url = "ws://127.0.0.1:9080/ws-api/v3"' in text
    readme = (tmp_path / "my-mm" / "README.md").read_text()
    assert "fastmm sim &" in readme
    assert "strategy:MyMm --config sim.toml --duration 60s" in readme


@pytest.mark.parametrize("venue, host, testnet", [
    ("binance-demo", "demo-api.binance.com", "true"),
    ("binance-testnet", "testnet.binance.vision", "true"),
    ("binance", "api.binance.com", "false"),
])
def test_the_venue_picks_live_toml(tmp_path, monkeypatch, venue, host, testnet):
    monkeypatch.delenv("FASTMM_BINANCE_API_KEY", raising=False)
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", str(tmp_path / "p"),
                          "--venue", venue], capture_output=True, text=True, check=True)
    assert "live.toml" in out.stdout
    text = (tmp_path / "p" / "live.toml").read_text()
    assert f'rest_url = "https://{host}"' in text
    assert f"testnet = {testnet} " in text
    assert f"--venue {venue}" in text
    assert "@" not in text.replace("${", "")
    fastmm.BacktestConfig.from_toml(tmp_path / "p" / "live.toml")


def test_an_unknown_venue_is_refused(tmp_path):
    with pytest.raises(ValueError, match="unknown venue"):
        write_project(tmp_path, venue="kraken")
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", str(tmp_path / "p"),
                          "--venue", "kraken"], capture_output=True, text=True, check=False)
    assert out.returncode == 2


def test_the_help_lists_every_file(tmp_path):
    out = subprocess.run([sys.executable, "-m", "fastmm", "init", "--help"],
                         capture_output=True, text=True, check=True)
    text = " ".join(out.stdout.split())
    for name in FILES + ["production.toml"]:
        assert name in text
    written = write_project(tmp_path / "p", profile="production")
    assert sorted(p.name for p in written) == sorted(FILES + ["production.toml"])


def test_the_config_names_a_strategy_that_resolves(tmp_path):
    pytest.importorskip("numba")
    write_project(tmp_path / "mm")
    script = ("import fastmm; cfg = fastmm.BacktestConfig.from_toml('config.toml'); "
              "cfg.duration_s = 5; r = fastmm.run_backtest(cfg, 'synthetic'); print(r.strategy)")
    out = subprocess.run([sys.executable, "-c", script], cwd=tmp_path / "mm",
                         capture_output=True, text=True, check=False)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "py:Mm"
