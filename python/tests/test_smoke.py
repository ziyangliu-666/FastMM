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
        assert p["type"] in ("int", "double", "bool")
        assert p["min"] <= p["default"] <= p["max"]
    assert basic["levels"]["type"] == "int" and isinstance(basic["levels"]["default"], int)
    avs = {p["name"]: p for p in s["avellaneda_stoikov"]}
    assert avs["gamma"]["type"] == "double"
    assert avs["infinite_horizon"]["type"] == "bool"
    assert isinstance(avs["infinite_horizon"]["default"], bool)


def test_inspect_journal():
    from conftest import FIXTURE_FMJ

    info = fastmm.inspect_journal(FIXTURE_FMJ)
    assert info["market_data_messages"] == 1000
    assert info["outbound_messages"] == 0
    assert info["rng_seed"] == 42
