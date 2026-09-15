"""fastmm.run_live and python -m fastmm run against fastmm-sim-exchange (ADR-0013, section 3).

Every session runs in a child process on free ports. The simulator and fastmm-replay come from
$FASTMM_BIN_DIR or build/release/bin; without them the tests are skipped.
"""

import os
import re
import signal
import socket
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

pytest.importorskip("numba")

import fastmm  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BIN = Path(os.environ.get("FASTMM_BIN_DIR", REPO / "build" / "release" / "bin"))
SIM = BIN / "fastmm-sim-exchange"
REPLAY = BIN / "fastmm-replay"

pytestmark = pytest.mark.skipif(not SIM.exists() or not REPLAY.exists(),
                                reason=f"fastmm-sim-exchange and fastmm-replay not in {BIN}")


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_port(port, timeout=15.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.05)
    return False


@pytest.fixture(scope="module")
def sim(tmp_path_factory):
    """A simulator with a faster counter-party flow than configs/sim.toml, so fills come quickly."""
    port = _free_port()
    cfg = tmp_path_factory.mktemp("sim") / "sim.toml"
    text = (REPO / "configs" / "sim.toml").read_text()
    text = text.replace("market_rate_per_s = 4.0", "market_rate_per_s = 25.0")
    text = text.replace('tls_cert = "tests/fixtures/tls/cert.pem"',
                        f'tls_cert = "{REPO}/tests/fixtures/tls/cert.pem"')
    text = text.replace('tls_key = "tests/fixtures/tls/key.pem"',
                        f'tls_key = "{REPO}/tests/fixtures/tls/key.pem"')
    cfg.write_text(text)
    log = open(cfg.with_suffix(".log"), "w")
    proc = subprocess.Popen([str(SIM), "--config", str(cfg), "--port", str(port), "--no-tls",
                             "--stats-interval", "0"], stdout=log, stderr=subprocess.STDOUT)
    try:
        assert _wait_port(port), "fastmm-sim-exchange did not open its port"
        yield port
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


def _config(tmp_path, port, strategy="py:BasicMMHot", cpu=None, name="live.toml"):
    text = (REPO / "configs" / "sim-local.toml").read_text()
    text = text.replace("127.0.0.1:9080", f"127.0.0.1:{port}")
    text = text.replace('name = "basic_mm"', f'name = "{strategy}"')
    text = text.replace('journal_dir = "runs"', f'journal_dir = "{tmp_path}"')
    text = text.replace('epoch_file = "runs/session_epoch"', f'epoch_file = "{tmp_path}/epoch"')
    if cpu is not None:
        text = re.sub(r"^cpu = -1 ", f"cpu = {cpu} ", text, count=1, flags=re.M)
        assert f"cpu = {cpu}" in text
    path = tmp_path / name
    path.write_text(text)
    return path


def _env(**extra):
    env = dict(os.environ)
    paths = [str(HERE)] + [p for p in sys.path if p]
    env["PYTHONPATH"] = os.pathsep.join(paths)
    env["FASTMM_SIM_API_KEY"] = "sim-key"
    env["FASTMM_SIM_API_SECRET"] = "sim-secret"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def _python(code, timeout=120, **env):
    return subprocess.run([sys.executable, "-c", textwrap.dedent(code)], env=_env(**env),
                          capture_output=True, text=True, timeout=timeout, cwd=REPO)


def _final(log_text, key):
    m = re.search(rf"fastmm-live: events=\S+ .*\b{key}=(\d+)", log_text)
    assert m, log_text[-3000:]
    return int(m.group(1))


def test_run_live_trades_stops_on_sigint_and_replays(sim, tmp_path):
    cfg = _config(tmp_path, sim)
    journal = tmp_path / "hot.fmj"
    log = tmp_path / "hot.log"
    code = f"""
        import fastmm, live_strategies
        raise SystemExit(fastmm.run_live(live_strategies.BasicMMHot, {str(cfg)!r},
                                         journal={str(journal)!r}, log={str(log)!r},
                                         status={str(tmp_path / 'hot.status')!r}))
    """
    proc = subprocess.Popen([sys.executable, "-c", textwrap.dedent(code)], env=_env(), cwd=REPO,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 60
    ticks = 0
    while time.monotonic() < deadline and proc.poll() is None:
        time.sleep(0.5)
        text = log.read_text() if log.exists() else ""
        counts = re.findall(r"\[sim\] md=live .* orders=(\d+) ", text)
        if counts and int(counts[-1]) >= 3:
            ticks += 1
            if ticks >= 8:  # four more seconds of trading
                break
    assert proc.poll() is None, proc.communicate()[0]
    proc.send_signal(signal.SIGINT)
    out, _ = proc.communicate(timeout=30)
    text = log.read_text()
    assert proc.returncode == 0, out + text[-3000:]
    assert "fastmm-live: shutting down (signal)" in text
    assert "cancel_all ok" in text
    assert "exit code 0" in text
    assert _final(text, "orders") > 0
    assert _final(text, "fills") > 0

    info = fastmm.inspect_journal(journal)
    assert info["strategy"] == "py:BasicMMHot"
    meta = info["strategy_meta"]
    assert meta["class"] == "basic_mm_hot:BasicMMHot"
    assert len(meta["hot_source_sha256"]) == 64
    assert meta["fastmm"] == fastmm.__version__
    import numba

    assert meta["numba"] == numba.__version__
    assert info["outbound_messages"] > 0

    # BasicMMHot sends what C++ basic_mm sends: the C++ strategy with the same parameters replays
    # the journal to the recorded outbound stream.
    replay_cfg = _config(tmp_path, sim, strategy="basic_mm", name="replay.toml")
    r = subprocess.run([str(REPLAY), "--journal", str(journal), "--config", str(replay_cfg),
                        "--strategy", "basic_mm", "--verify"], capture_output=True, text=True,
                       cwd=REPO, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr[-3000:]
    assert "replay MATCH" in r.stdout
    hashes = re.findall(r"(recorded|replayed) outbound (\d+) msgs sha256 ([0-9a-f]{64})", r.stdout)
    assert len(hashes) == 2 and hashes[0][1:] == hashes[1][1:]


def test_python_m_fastmm_run_sets_thread_variables(sim, tmp_path):
    cfg = _config(tmp_path, sim)
    env_out = tmp_path / "env.txt"
    env = _env(FASTMM_TEST_ENV_OUT=env_out)
    for v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS"):
        env.pop(v, None)
    env["MKL_NUM_THREADS"] = "3"
    r = subprocess.run([sys.executable, "-m", "fastmm", "run", "live_strategies:BasicMMHot",
                        "--config", str(cfg), "--duration", "3s", "--journal",
                        str(tmp_path / "cli.fmj"), "--no-status", "--log",
                        str(tmp_path / "cli.log"), "--param", "levels=2"],
                       env=env, capture_output=True, text=True, timeout=120, cwd=REPO)
    text = (tmp_path / "cli.log").read_text()
    assert r.returncode == 0, r.stderr + text[-3000:]
    assert "shutting down (duration elapsed)" in text
    assert env_out.read_text() == "1 1 3"
    embedded = (tmp_path / "cli.fmj").read_bytes().decode("utf-8", "replace")
    assert re.search(r"\n\s*levels = ['\"]?2['\"]?\n", embedded), embedded[:3000]

    bad = subprocess.run([sys.executable, "-m", "fastmm", "run", "live_strategies:Nope", "--config",
                          str(cfg)], env=env, capture_output=True, text=True, timeout=60, cwd=REPO)
    assert bad.returncode == 3
    assert "cannot load strategy 'live_strategies:Nope'" in bad.stderr
    usage = subprocess.run([sys.executable, "-m", "fastmm", "run", "live_strategies:BasicMMHot"],
                           env=env, capture_output=True, text=True, timeout=60, cwd=REPO)
    assert usage.returncode == 2


def test_hot_hook_exception_trips_the_kill_switch_and_exits_6(sim, tmp_path):
    cfg = _config(tmp_path, sim, strategy="basic_mm")  # its [strategy.params] do not apply
    log = tmp_path / "raises.log"
    r = _python(f"""
        import fastmm, live_strategies
        raise SystemExit(fastmm.run_live(live_strategies.RaisesAfter, {str(cfg)!r}, {{"calls": 15}},
                                         duration=60, no_journal=True, no_status=True,
                                         log={str(log)!r}))
    """)
    text = log.read_text()
    assert r.returncode == 6, r.stderr + text[-3000:]
    assert "kill switch: StrategyError" in text
    assert "cancel_all ok" in text
    assert "fastmm: py:RaisesAfter.on_book raised an exception" in r.stderr


def test_compile_failure_and_strategy_errors_exit_3(sim, tmp_path):
    cfg = _config(tmp_path, sim)
    r = _python(f"""
        import fastmm, live_strategies
        print(fastmm.run_live(live_strategies.Allocates, {str(cfg)!r}, no_journal=True),
              fastmm.run_live(live_strategies.NoHotHooks, {str(cfg)!r}, no_journal=True),
              fastmm.run_live(live_strategies.BasicMMHot, {str(cfg)!r}, {{"levels": 99}},
                              no_journal=True),
              fastmm.run_live(live_strategies.BasicMMHot, {str(tmp_path / 'missing.toml')!r}))
    """)
    assert r.returncode == 0, r.stderr
    assert r.stdout.split() == ["3", "3", "3", "3"]
    assert "rejected by the IR check" in r.stderr
    assert "has no @fastmm.hot methods" in r.stderr
    assert "parameter 'levels'" in r.stderr
    assert "connecting" not in r.stderr  # nothing reached a venue


def test_one_session_per_process_publish_and_watchdog_exit_7(sim, tmp_path):
    cfg = _config(tmp_path, sim)
    log = tmp_path / "one.log"
    r = _python(f"""
        import threading, time, fastmm, fastmm_live, live_strategies
        s = live_strategies.BasicMMHot()
        rcs = []
        t = threading.Thread(target=lambda: rcs.append(fastmm.run_live(
            s, {str(cfg)!r}, duration=60, no_journal=True, no_status=True, log={str(log)!r})))
        t.start()
        while not hasattr(s, "_fastmm_live_params"):
            time.sleep(0.01)
        time.sleep(1.0)
        try:
            fastmm.run_live(live_strategies.BasicMMHot, {str(cfg)!r}, duration=1, no_journal=True)
            print("second: ran")
        except RuntimeError as e:
            print("second:", e)
        ch = s._fastmm_live_params
        print("publish:", ch.publish({{"half_spread_bps": 6.0, "levels": 1}}))
        try:
            ch.publish({{"levels": 99}})
        except ValueError as e:
            print("invalid:", e)
        fastmm_live._live._fail_slow_tier("test: slow tier stopped")
        t.join()
        print("rc:", rcs[0])
        print("after:", ch.publish({{"levels": 1}}), ch.closed, ch.published)
    """)
    text = log.read_text()
    assert r.returncode == 0, r.stderr + text[-3000:]
    lines = dict(line.split(": ", 1) for line in r.stdout.splitlines())
    assert "a live session is already running in this process" in lines["second"]
    assert lines["publish"] == "True"
    assert lines["invalid"] == "parameter 'levels': value 99 outside [1, 8]"
    assert lines["rc"] == "7"
    assert lines["after"] == "False True 1"
    assert "fastmm-live: test: slow tier stopped" in text
    assert "cancel_all ok" in text


def test_forked_child_is_inert(sim, tmp_path):
    cfg = _config(tmp_path, sim)
    r = _python(f"""
        import os, threading, time, fastmm, live_strategies
        s = live_strategies.BasicMMHot()
        t = threading.Thread(target=lambda: fastmm.run_live(
            s, {str(cfg)!r}, duration=4, dry_run=True, no_journal=True, no_status=True,
            log=os.devnull))
        t.start()
        while not hasattr(s, "_fastmm_live_params"):
            time.sleep(0.01)
        time.sleep(1.0)
        pid = os.fork()
        if pid == 0:
            ok = s._fastmm_live_params.publish({{"levels": 1}})
            try:
                fastmm.run_live(live_strategies.BasicMMHot, {str(cfg)!r}, no_journal=True)
                code = 1
            except RuntimeError as e:
                code = 0 if (not ok and "forked" in str(e)) else 2
            os._exit(code)
        _, status = os.waitpid(pid, 0)
        t.join()
        print("child", os.waitstatus_to_exitcode(status))
    """)
    assert r.returncode == 0, r.stderr[-3000:]
    assert r.stdout.strip().endswith("child 0"), r.stdout + r.stderr[-2000:]


def _cpus():
    try:
        return sorted(os.sched_getaffinity(0))
    except AttributeError:  # pragma: no cover
        return []


@pytest.mark.skipif(len(_cpus()) < 2, reason="needs 2 CPUs")
def test_affinity_moves_preexisting_threads_off_the_engine_cpu(sim, tmp_path):
    engine_cpu = _cpus()[-1]
    cfg = _config(tmp_path, sim, cpu=engine_cpu)
    r = _python(f"""
        import os, threading, time
        import numpy as np
        a = np.random.default_rng(1).random((400, 400))
        _ = a @ a  # starts the BLAS thread pool
        stop = threading.Event()
        worker = threading.Thread(target=stop.wait)
        worker.start()
        before = sorted(os.listdir("/proc/self/task"))
        import fastmm, live_strategies
        rc = fastmm.run_live(live_strategies.BasicMMHot, {str(cfg)!r}, duration=2, dry_run=True,
                             no_journal=True, no_status=True, log={str(tmp_path / 'aff.log')!r})
        after = {{t: sorted(os.sched_getaffinity(int(t))) for t in os.listdir("/proc/self/task")}}
        stop.set()
        worker.join()
        print("rc", rc)
        print("before", len(before))
        print("kept", sum(1 for t in before if t in after))
        print("on_engine_cpu", sum(1 for cpus in after.values() if {engine_cpu} in cpus))
    """, OPENBLAS_NUM_THREADS=2)
    assert r.returncode == 0, r.stderr[-3000:]
    out = dict(line.split(" ", 1) for line in r.stdout.splitlines())
    text = (tmp_path / "aff.log").read_text()
    assert out["rc"] == "0", text[-3000:]
    assert int(out["before"]) >= 3  # main, worker and at least one BLAS thread
    assert int(out["kept"]) >= 3
    assert out["on_engine_cpu"] == "0"
    assert "thread affinity:" in text
