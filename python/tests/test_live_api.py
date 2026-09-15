"""fastmm.run_live and python -m fastmm without the live runtime; the journal metadata helpers."""

import importlib.util
import subprocess
import sys

import pytest

import fastmm
from conftest import REPO


@pytest.mark.skipif(importlib.util.find_spec("fastmm_live") is not None,
                    reason="fastmm_live is installed")
def test_run_live_without_the_live_runtime_names_the_extra():
    with pytest.raises(ImportError, match=r'pip install "fastmm-engine\[live\]"'):
        fastmm.run_live(fastmm.Strategy, "configs/sim-local.toml")


def test_python_m_fastmm_usage():
    r = subprocess.run([sys.executable, "-m", "fastmm"], capture_output=True, text=True, cwd=REPO)
    assert r.returncode == 2
    assert "usage: python -m fastmm" in r.stderr
    h = subprocess.run([sys.executable, "-m", "fastmm", "run", "--help"], capture_output=True,
                       text=True, cwd=REPO)
    assert h.returncode == 0
    assert "--duration" in h.stdout and "--dry-run" in h.stdout


def test_meta_format_round_trips_and_param_fields_follow_the_record():
    pytest.importorskip("numba")
    from fastmm._hot import meta

    class Mm(fastmm.Strategy):
        half = fastmm.Param(5.0, min=0.0, max=100.0, doc="bps")
        levels = fastmm.Param(2, min=1, max=8)
        on = fastmm.Param(True)
        count = fastmm.State(0)

        @fastmm.hot
        def on_book(self, ctx, book):
            self.count += 1

    text = meta.format_meta({"class": "m:Mm", "numba": "0.67.0"})
    assert text == "class=m:Mm\nnumba=0.67.0\n"
    assert meta.parse_meta(text) == {"class": "m:Mm", "numba": "0.67.0"}
    with pytest.raises(ValueError):
        meta.format_meta({"bad": "a\nb"})

    fields = meta.param_fields(Mm._fastmm_hot)
    dtype, param_bytes = Mm._fastmm_hot.record_dtype()
    assert [f["name"] for f in fields] == ["half", "levels", "on"]
    assert [f["type"] for f in fields] == ["double", "int", "bool"]
    assert fields[0]["raw_offset"] == dtype.fields["half_raw"][1]
    assert fields[1]["raw_offset"] == -1 and fields[1]["min"] == 1 and fields[1]["max"] == 8
    assert fields[2]["min"] is None
    assert all(f["offset"] < param_bytes for f in fields)

    digest = meta.hot_source_sha256(Mm._fastmm_hot)
    assert len(digest) == 64 and digest == meta.hot_source_sha256(Mm._fastmm_hot)
