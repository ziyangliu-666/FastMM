# Python packages

`fastmm-engine` (`pyproject.toml`, the `fastmm` module) and `fastmm-engine-live` (`python/live/pyproject.toml`, the `fastmm_live` module) build from this CMake tree with scikit-build-core (ADR-0011). User installation: [Install](../getting-started/install.md#python).

## Develop

```bash
python -m venv .venv
.venv/bin/pip install -e ".[dev]"              # editable build (scikit-build-core)
.venv/bin/python -m pytest python/tests -q
.venv/bin/pip install ./python/live
FASTMM_BIN_DIR=build/release/bin .venv/bin/python -m pytest python/live/tests -q
```

The live tests start `fastmm-sim-exchange` from `FASTMM_BIN_DIR` (default `build/release/bin`) on free ports and are skipped without it.

Regenerate the type stub after changing the bindings:

```bash
.venv/bin/pybind11-stubgen fastmm._core -o /tmp/stubs && cp /tmp/stubs/fastmm/_core.pyi python/fastmm/
```

## Building and publishing wheels

`.github/workflows/wheels.yml` builds manylinux_2_28 x86_64 wheels of `fastmm-engine` for CPython 3.9-3.14 and of `fastmm-engine-live` for CPython 3.10-3.14 (each one tested with its test suite) and the `fastmm-engine` sdist, for a `v*` tag or when run by hand, and keeps them as workflow artifacts; publishing is manual. `fastmm-engine-live` has no sdist.

`fastmm-engine-live` links OpenSSL statically, built by `scripts/wheels/build-openssl.sh` from a pinned, checksum-verified release; every OpenSSL security release needs a new `fastmm-engine-live` release. To build it locally:

```bash
./scripts/wheels/build-openssl.sh "$HOME/.cache/fastmm-openssl"
OPENSSL_ROOT_DIR="$HOME/.cache/fastmm-openssl" .venv/bin/pip wheel ./python/live --no-deps -w dist
./scripts/wheels/check-live-wheel.sh dist/fastmm_engine_live-*.whl
```

`FASTMM_OPENSSL_STATIC=OFF` links the system's shared OpenSSL instead; the check script then fails.

Publishing to PyPI makes the package and its source public. It happens on a `v*` tag, after the three build jobs pass, with the repository secret `PYPI_API_TOKEN` in the `pypi` environment; a run without a tag publishes only when started by hand with **publish** checked. The whole procedure is [Cutting a release](releasing.md).

Locally, `python -m build --sdist && python -m twine check dist/*` checks the sdist metadata.
