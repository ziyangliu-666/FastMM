"""fastmm-live against the installed wheels: metadata, the TLS self-test, CA lookup, and both
extension modules in one process."""

import importlib.metadata
import os
import subprocess
import sys

import certifi
import fastmm
import fastmm_live
import pytest


def test_version_pin_and_build_info():
    assert fastmm_live.__version__ == fastmm.__version__
    assert f"fastmm=={fastmm.__version__}" in importlib.metadata.requires("fastmm-live")
    info = fastmm_live.build_info()
    assert info["version"] == fastmm.__version__
    assert info["openssl"].startswith("OpenSSL 3.")
    assert info["python"].startswith(f"{sys.version_info.major}.{sys.version_info.minor}.")


def test_self_test_handshake():
    r = fastmm_live.self_test()
    assert r["protocol"] == "TLSv1.3"
    assert r["cipher"]
    assert r["ca"]["source"] in ("environment", "system", "fallback", "openssl-default")


def test_certifi_is_the_fallback():
    assert fastmm_live._live.ca_fallback_file() == certifi.where()


def _run(code, **env):
    full_env = {k: v for k, v in os.environ.items() if k not in ("SSL_CERT_FILE", "SSL_CERT_DIR")}
    full_env.update(env)
    return subprocess.run(
        [sys.executable, "-c", code], env=full_env, capture_output=True, text=True, check=False
    )


def test_ssl_cert_file_comes_first(tmp_path):
    code = "import fastmm_live as m; print(m.ca_locations()['source'], m.self_test()['protocol'])"
    ok = _run(code, SSL_CERT_FILE=certifi.where())
    assert ok.returncode == 0, ok.stderr
    assert ok.stdout.split() == ["environment", "TLSv1.3"]

    missing = str(tmp_path / "missing.pem")
    bad = _run(code, SSL_CERT_FILE=missing)
    assert bad.returncode != 0
    assert f"SSL_CERT_FILE={missing}" in bad.stderr


def test_core_and_live_in_one_process():
    fastmm_live.self_test()
    cfg = fastmm.BacktestConfig.single_instrument("BTCUSDT", tick="0.01", lot="0.00001")
    cfg.strategy = "basic_mm"
    cfg.duration_s = 5
    r = fastmm.run_backtest(cfg, data="synthetic")
    assert len(r.outbound_sha256) == 64
    assert fastmm_live.self_test()["protocol"] == "TLSv1.3"


@pytest.mark.skipif(sys.platform != "linux", reason="ELF symbol tables")
def test_no_openssl_symbols_exported():
    so = fastmm_live._live.__file__
    nm = subprocess.run(["nm", "-D", "--defined-only", so], capture_output=True, text=True)
    if nm.returncode != 0:
        pytest.skip("nm is not available")
    names = [line.split()[-1] for line in nm.stdout.splitlines() if line.strip()]
    assert names == ["PyInit__live"]
