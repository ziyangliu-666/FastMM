# Python packages

Two packages build from this CMake tree with scikit-build-core ([ADR-0011](../adr/0011-scikit-build-core.md)). User installation: [Install](../getting-started/install.md#python).

| Package | Module | Project file | sdist |
|---|---|---|---|
| `fastmm-engine` | `fastmm` | `pyproject.toml` | yes |
| `fastmm-engine-live` | `fastmm_live` | `python/live/pyproject.toml` | no |

## Develop

```bash
python -m venv .venv
.venv/bin/pip install -e ".[dev]"              # editable build (scikit-build-core)
.venv/bin/python -m pytest python/tests -q
.venv/bin/pip install ./python/live
FASTMM_BIN_DIR=build/release/bin .venv/bin/python -m pytest python/live/tests -q
```

The live tests start `fastmm-sim-exchange` from `FASTMM_BIN_DIR` (default `build/release/bin`) on free ports and are skipped without it.

After changing the bindings, regenerate the type stub; `python/tests/test_packaging.py` fails until you do:

```bash
.venv/bin/pybind11-stubgen fastmm._core -o /tmp/stubs
cp /tmp/stubs/fastmm/_core.pyi python/fastmm/
```

## Build the live wheel locally

`fastmm-engine-live` links OpenSSL statically, built by `scripts/wheels/build-openssl.sh` from a pinned, checksum-verified release. Every OpenSSL security release needs a new `fastmm-engine-live` release.

```bash
./scripts/wheels/build-openssl.sh "$HOME/.cache/fastmm-openssl"
OPENSSL_ROOT_DIR="$HOME/.cache/fastmm-openssl" \
  .venv/bin/pip wheel ./python/live --no-deps -w dist
./scripts/wheels/check-live-wheel.sh dist/fastmm_engine_live-*.whl
```

`FASTMM_OPENSSL_STATIC=OFF` links the system's shared OpenSSL instead, and the check script then fails.

To check the sdist metadata:

```bash
python -m build --sdist && python -m twine check dist/*
```

## Wheels in CI

`.github/workflows/wheels.yml` builds the sdist and manylinux_2_28 x86_64 wheels of both packages for CPython 3.10 to 3.14, and tests each wheel with its test suite.

| Trigger | Builds | Publishes to PyPI |
|---|---|---|
| a `v*` tag | yes | yes, after the three build jobs pass |
| run by hand, **publish** unchecked | yes, kept as workflow artifacts | no |
| run by hand, **publish** checked | yes | yes |

Publishing uses the repository secret `PYPI_API_TOKEN` in the `pypi` environment. The release steps are in [Cutting a release](releasing.md).
