from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXAMPLE_TOML = REPO / "configs" / "backtest-example.toml"
FIXTURE_FMJ = REPO / "tests" / "fixtures" / "journals" / "sample_1000.fmj"
FIXTURE_SHA = REPO / "tests" / "fixtures" / "journals" / "sample_1000.sha256"


@pytest.fixture(scope="session")
def repo() -> Path:
    return REPO


@pytest.fixture()
def example_config():
    import fastmm

    return fastmm.BacktestConfig.from_toml(EXAMPLE_TOML)


@pytest.fixture(scope="session")
def synthetic_csv(tmp_path_factory) -> Path:
    """A 30 s CSV from tools/gen_synthetic_data.py (same seed, same file)."""
    out = tmp_path_factory.mktemp("data") / "synthetic.csv"
    subprocess.run(
        [
            sys.executable,
            str(REPO / "tools" / "gen_synthetic_data.py"),
            "--out",
            str(out),
            "--seed",
            "7",
            "--duration-s",
            "30",
        ],
        check=True,
        capture_output=True,
    )
    return out
