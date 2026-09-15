import importlib.metadata

import fastmm


def test_live_extra_pins_the_same_version():
    requires = importlib.metadata.requires("fastmm-engine") or []
    live = [r for r in requires if r.startswith("fastmm-engine-live")]
    assert live == [f'fastmm-engine-live=={fastmm.__version__}; extra == "live"']
