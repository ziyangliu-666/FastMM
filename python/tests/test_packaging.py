import importlib.metadata

import fastmm


def test_live_extra_pins_the_same_version():
    requires = importlib.metadata.requires("fastmm-engine") or []
    live = [r for r in requires if r.startswith("fastmm-engine-live")]
    assert live == [f'fastmm-engine-live=={fastmm.__version__}; extra == "live"']


def test_the_type_stub_is_what_pybind11_stubgen_makes_of_the_bindings(tmp_path):
    """python/fastmm/_core.pyi is generated (docs/contributing/python-packages.md) and is what the
    API reference renders; a binding changed without regenerating it fails here."""
    import re
    import subprocess
    import sys

    import pytest

    from conftest import REPO

    pytest.importorskip("pybind11_stubgen")
    subprocess.run([sys.executable, "-m", "pybind11_stubgen", "fastmm._core", "-o", str(tmp_path)],
                   check=True, capture_output=True)

    def text(path):  # the version is the build's, not part of the interface
        return re.sub(r"^__version__: str = .*$", "__version__: str", path.read_text(), flags=re.M)

    generated = text(tmp_path / "fastmm" / "_core.pyi")
    assert generated == text(REPO / "python" / "fastmm" / "_core.pyi"), (
        "regenerate: pybind11-stubgen fastmm._core -o /tmp/stubs && "
        "cp /tmp/stubs/fastmm/_core.pyi python/fastmm/")
