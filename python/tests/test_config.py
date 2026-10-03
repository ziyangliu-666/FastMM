import math

import pytest

import fastmm
from conftest import REPO


def test_from_toml_reads_sections(example_config):
    cfg = example_config
    assert cfg.strategy == "basic_mm"
    assert cfg.fill_model == "l2_queue"
    assert cfg.duration_s == 60.0
    assert cfg.latency_fixed_us == 200 and cfg.latency_jitter_us == 50
    assert math.isclose(cfg.queue_conservatism, 0.5)
    assert cfg.maker_fee_bps == 10.0 and cfg.taker_fee_bps == 10.0  # Binance spot VIP 0
    assert cfg.instrument_fees(0) == (10.0, 10.0)
    assert cfg.markout_horizons_s == [1.0, 10.0, 60.0]
    assert cfg.params["half_spread_bps"] == "0.01"
    assert cfg.start_mid == "60000"


def test_unknown_keys_are_errors_with_their_line(example_config, tmp_path):
    assert example_config.warnings == []
    text = (REPO / "configs" / "backtest-example.toml").read_text()
    for section, typo in (("[risk]\n", 'max_postion = "0.05"'), ("[backtest]\n", "duraton_s = 5")):
        bad = text.replace(section, section + typo + "\n")
        path = tmp_path / "typos.toml"
        path.write_text(bad)
        line = bad.splitlines().index(typo) + 1
        key = section.strip("[]\n") + "." + typo.split(" ")[0]
        with pytest.raises(fastmm.ConfigError, match=f"unknown key '{key}'.*line {line}"):
            fastmm.BacktestConfig.from_toml(path)


def test_single_instrument_and_setters():
    cfg = fastmm.BacktestConfig.single_instrument("ETHUSDT", "0.01", "0.0001")
    cfg.strategy = "basic_mm"
    cfg.seed = 9
    cfg.duration_s = 2.5
    cfg.fill_model = "l2_queue"
    cfg.output_dir = "runs/x"
    cfg.set_param("half_spread_bps", 2)
    cfg.set_param("quote_qty", 0.01)
    cfg.set_param("pull_on_stale_ms", 0)
    cfg.set_param("max_inventory", "0.05")
    assert cfg.seed == 9 and cfg.duration_s == 2.5 and cfg.output_dir == "runs/x"
    assert cfg.params == {
        "half_spread_bps": "2",
        "quote_qty": "0.01",
        "pull_on_stale_ms": "0",
        "max_inventory": "0.05",
    }
    cfg.params = {"levels": 2, "estimate": True}
    assert cfg.params == {"levels": "2", "estimate": "true"}


def test_config_errors(tmp_path):
    with pytest.raises(fastmm.ConfigError):
        fastmm.BacktestConfig.from_toml(tmp_path / "missing.toml")
    with pytest.raises(ValueError):
        fastmm.BacktestConfig.single_instrument("X", "abc", "1")
    cfg = fastmm.BacktestConfig.single_instrument("X", "0.01", "0.001")
    with pytest.raises(ValueError):
        cfg.fill_model = "nope"
    with pytest.raises(ValueError):
        cfg.duration_s = 0
    with pytest.raises(TypeError):
        cfg.set_param("x", [1, 2])
    with pytest.raises(ValueError):
        cfg.set_param("x", float("nan"))


def test_copy_is_independent(example_config):
    import copy

    a = example_config
    b = copy.deepcopy(a)
    b.seed = 1234
    b.set_param("half_spread_bps", 1)
    assert a.seed != 1234 and a.params["half_spread_bps"] == "0.01"
