# Install

All commands run from the repository root.

## Requirements

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | gcc 13 or newer, or clang 16 or newer | CI uses gcc 13.3 and clang 18 |
| CMake | 3.25 or newer | `scripts/bootstrap.sh` installs it with pip when it is missing |
| Ninja | any | all presets use it |
| OpenSSL | 3.0 or newer, development headers | networking; not needed with `FASTMM_BUILD_NET=OFF` |
| zlib | development headers | networking |
| Python | 3.9 or newer | the tools in `tools/` and the Python package |

On Ubuntu 24.04: `sudo apt install g++-13 cmake ninja-build libssl-dev zlib1g-dev python3`. Every other dependency (fmt, toml++, simdjson, doctest, Google Benchmark, pybind11) is downloaded and pinned by CPM; [Dependencies](../contributing/dependencies.md) lists the versions.

## Build

```bash
./scripts/bootstrap.sh
cmake --build --preset release -j
ctest --preset release
```

`bootstrap.sh` checks the toolchain, creates the CPM download cache ([Dependencies](../contributing/dependencies.md)) and configures the `release` preset; `--dev` also configures `debug` and installs the pre-commit hooks. The programs are in `build/release/bin/`.

## Check the build

```bash
./build/release/bin/fastmm-backtest --list-strategies
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
```

The first command lists the built-in strategies with their parameters. The second backtests `basic_mm` for 60 s of simulated time and prints a summary.

## Presets

| Preset | Build | Use |
|---|---|---|
| `release` | gcc, `-O3`, LTO, portable x86-64-v2 | everyday work, CI |
| `release-native` | as `release` with `-march=native` | benchmarks on this machine only |
| `debug` | gcc, `-O0 -g`, no-allocation assertions | debugging, clang-tidy |
| `asan` | gcc, AddressSanitizer and UBSan | `cmake --workflow --preset asan` |
| `tsan`, `clang-tsan` | ThreadSanitizer | `cmake --workflow --preset tsan` |
| `clang-release` | clang, LTO | the second compiler in CI |
| `python` | the Python module only, no networking | wheel builds |

`ctest --preset <name>` runs the tests of a preset; the test presets leave out the opt-in `live` label (tests against real testnets).

## Docker

```bash
docker compose up --build
```

This builds one image and starts two containers: `fastmm-sim-exchange` and `fastmm-live` trading `basic_mm` against it for 120 s (`configs/sim-docker.toml`). Journals go to `runs/`.

## Python

The `fastmm-engine` package (`pip install fastmm-engine`, imported as `fastmm`) runs backtests on CPython 3.9 or later; see [Python research bindings](../python.md).

| Extra | Installs | Needs |
|---|---|---|
| `fastmm-engine[live]` | `fastmm-engine-live` of the same version: networking, venue connectors and OpenSSL 3 inside the extension module | CPython 3.10 or later, Linux x86-64 |
| `fastmm-engine[hot]` | numba and llvmlite | CPython 3.10 or later |

TLS connections, from the `fastmm-live` program or the Python package, trust the CA certificates in `SSL_CERT_FILE` and `SSL_CERT_DIR` if either is set, otherwise in the first existing file of `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/cert.pem`, otherwise in `certifi` (Python package only), otherwise in OpenSSL's built-in paths. A venue's `ca_file` adds to them.

## Use FastMM from your own project

- The [Quick start](quickstart.md) pulls FastMM into a CMake project with `FetchContent`; the [Tutorial: your first market maker](../tutorials/first-strategy/README.md) continues from there.
- [Register a strategy](../how-to/strategies/register-a-strategy.md) builds a project against an installed FastMM (`find_package(fastmm)`) or a source tree (`add_subdirectory`).
- Build your project with the compiler that built FastMM: a release install contains GCC LTO objects.
