import fastmm


def test_import_and_version():
    assert fastmm.__version__.count(".") == 2
    assert "fastmm" in fastmm.build_info()


def test_strategies_expose_parameter_schemas():
    s = fastmm.strategies()
    assert "basic_mm" in s and "avellaneda_stoikov" in s
    basic = {p["name"]: p for p in s["basic_mm"]}
    assert {"half_spread_bps", "skew_bps_per_unit", "quote_qty", "max_inventory"} <= set(basic)
    for p in basic.values():
        assert set(p) == {"name", "type", "default", "min", "max", "doc"}
        assert p["type"] in ("int", "double", "bool", "decimal", "bps", "ms")
        assert p["min"] <= p["default"] <= p["max"]
    assert basic["levels"]["type"] == "int" and isinstance(basic["levels"]["default"], int)
    assert basic["quote_qty"]["type"] == "decimal"
    assert isinstance(basic["quote_qty"]["default"], float) and basic["quote_qty"]["default"] == 0.01
    assert basic["half_spread_bps"]["type"] == "bps"
    assert isinstance(basic["half_spread_bps"]["default"], float)
    assert basic["half_spread_bps"]["default"] == 5.0 and basic["half_spread_bps"]["max"] == 10000.0
    assert basic["pull_on_stale_ms"]["type"] == "ms"
    assert isinstance(basic["pull_on_stale_ms"]["default"], int)
    assert basic["pull_on_stale_ms"]["default"] == 2000
    avs = {p["name"]: p for p in s["avellaneda_stoikov"]}
    assert avs["gamma"]["type"] == "double"
    assert avs["infinite_horizon"]["type"] == "bool"
    assert isinstance(avs["infinite_horizon"]["default"], bool)
    assert avs["max_inventory"]["type"] == "decimal"
    opts = {p["name"]: p for p in s["options_mm"]}
    assert opts["pull_on_stale_ms"]["type"] == "ms" and opts["max_position"]["type"] == "decimal"


def test_inspect_journal():
    from conftest import FIXTURE_FMJ

    info = fastmm.inspect_journal(FIXTURE_FMJ)
    assert info["market_data_messages"] == 1000
    assert info["outbound_messages"] == 0
    assert info["rng_seed"] == 42
