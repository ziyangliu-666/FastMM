import importlib.metadata

import fastmm


def test_live_extra_pins_the_same_version():
    requires = importlib.metadata.requires("fastmm") or []
    live = [r for r in requires if r.startswith("fastmm-live")]
    assert live == [f'fastmm-live=={fastmm.__version__}; extra == "live"']
