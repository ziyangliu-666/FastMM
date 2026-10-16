# FastMM for agents and new contributors

FastMM is a C++20 market-making engine with Python bindings. One strategy file runs in a deterministic backtest, in a replay of a recorded session and live against exchanges. Documentation: <https://ziy.bio/FastMM/> (source in `docs/`, index for tools in `docs/llms.txt`).

## Two ways in

| | Python | C++ |
|---|---|---|
| Install | `pip install "fastmm-engine[hot]"`, plus `[live]` for venues | build this repository with CMake |
| Strategies | `fastmm.Strategy` subclasses, hooks marked `@fastmm.hot` compiled by Numba | structs on `StrategyBase<Params>` (`include/fastmm/strategy.hpp`) |
| Programs | the `fastmm` command: `init`, `report`, `sim`, `run` | `fastmm-live`, `fastmm-backtest`, `fastmm-replay`, `fastmm-gateway`, `fastmm-ctl`, `fastmm-top`, `fastmm-pnl`, `fastmm-sim-exchange`, `fastmm-sim-itch` in `build/<preset>/bin/` |
| First page | `docs/getting-started/quickstart.md` | `docs/getting-started/cpp-quickstart.md` |

The PyPI package is `fastmm-engine`, imported as `fastmm`. `pip install fastmm` installs an unrelated project. The wheels are Linux x86-64, CPython 3.10 or later.

## First backtest

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

From a C++ build: `./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic`.

## Where things are

| Path | What |
|---|---|
| `include/fastmm/`, `src/` | the engine; `strategies/` the built-in strategies (`basic_mm`, `avellaneda_stoikov`, `lead_mm`, `options_mm`, `xmm`), `venues/` the exchange connectors |
| `python/fastmm/` | the `fastmm` package; `_core.pyi` is the typed surface of the bindings in `python/src/` |
| `python/live/` | the `fastmm-engine-live` package: connectors and `fastmm-sim-exchange` for Python |
| `apps/` | the command-line programs |
| `examples/` | `quickstart/` (C++), `python/strategies/`, `cpp/tutorial/`, `external-project/`, `external-venue/` |
| `configs/` | one TOML per venue environment, the simulators and backtests |
| `docs/reference/` | strategy API (`strategy-api.md`, `python-api.md`), configuration keys, command lines, venues, file formats |
| `docs/how-to/`, `docs/explanation/` | task guides; how the engine works and the benchmarks |
| `docs/adr/` | design records |
| `tests/`, `python/tests/` | doctest suites under ctest; pytest |

## Build and test

```bash
./scripts/bootstrap.sh                         # checks the toolchain, configures the release preset
cmake --build --preset release -j
ctest --preset release -j"$(nproc)"
./scripts/lint.sh build/release                # format, generated code, docs snippets and links, CLI reference
.venv/bin/pip install -e ".[dev]" && .venv/bin/python -m pytest python/tests -q
./scripts/docs-serve.sh --build                # the docs site, mkdocs build --strict
```

Requirements and presets: `docs/getting-started/install.md`. More on the Python packages: `docs/contributing/python-packages.md`.

## Conventions

- Hot-path code does not allocate or throw; `tests/hotpath/noalloc_test.cpp` checks it with `NoAllocScope`.
- Prices and quantities are int64 fixed point (`docs/reference/fixed-point.md`).
- Keys come from environment variables through `${VAR}` in TOML; a literal secret is refused.
- Commit messages: `area: sentence`, as in `git log` (`core:`, `venues:`, `docs:`).
- Docs follow `docs/contributing/writing-docs.md`. A code block marked `<!-- snippet: <file>#<region> -->` is a copy of that file: edit the source, then run `python3 tools/doc_snippets.py`.
- A new page under `docs/` needs an entry in the `nav` of `mkdocs.yml`.
