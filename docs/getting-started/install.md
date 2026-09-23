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
ctest --preset release -j"$(nproc)"
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
| `release-dpdk` | as `release` with the DPDK receive backend linked in | `rx_backend = "dpdk"` ([Receive a multicast feed](../how-to/operations/multicast-feeds.md)), `scripts/package-release.sh` |
| `debug` | gcc, `-O0 -g`, no-allocation assertions | debugging, clang-tidy |
| `asan` | gcc, AddressSanitizer and UBSan | `cmake --workflow --preset asan` |
| `tsan`, `clang-tsan` | ThreadSanitizer | `cmake --workflow --preset tsan` |
| `clang-release` | clang, LTO | the second compiler in CI |
| `python` | the Python module only, no networking | wheel builds |

`ctest --preset <name>` runs the tests of a preset; the test presets leave out the opt-in `live` label (tests against real testnets). They set no job count: without `-j`, ctest runs one test at a time unless `CTEST_PARALLEL_LEVEL` is set.

## Docker

```bash
docker compose up --build
```

This builds one image and starts two containers, `sim-exchange` and `engine`, which run `fastmm-sim-exchange` and `fastmm-live` trading `basic_mm` against it for 120 s (`configs/sim-docker.toml`). Journals go to `runs/`. The image runs as root and carries test TLS certificates; [Running this in production](../how-to/operations/running-in-production.md#10-what-the-repository-does-not-ship) lists what a deployment has to add.

## WSL2

- The log of a live session shows `host wall clock stepped by <n> ns relative to CLOCK_MONOTONIC_RAW ...` and `TSC recalibration stepped the engine clock by <n> ns ...` warnings: WSL2 steps the Linux wall clock to follow Windows, and the engine clock follows it ([Troubleshooting](../how-to/operations/troubleshooting.md), [Architecture](../explanation/architecture.md)).
- Keep `[engine] spin_mode = "adaptive"` and `cpu = -1`, as the shipped configurations do. `spin_mode`, `cpu` and `net_cpus` are described in [Configuration](../reference/configuration.md#engine), and CPU pinning on dedicated machines in the [Go-live checklist](../how-to/operations/go-live-checklist.md).

## Python

The `fastmm-engine` package (imported as `fastmm`) runs backtests on CPython 3.9 or later, and `fastmm-engine-live` adds live trading; see [Python](../python.md).

| Extra | Installs | Needs |
|---|---|---|
| `fastmm-engine[live]` | `fastmm-engine-live` of the same version: networking, venue connectors and OpenSSL 3 inside the extension module | CPython 3.10 or later, Linux x86-64 |
| `fastmm-engine[hot]` | numba and llvmlite | CPython 3.10 or later |

### Install from source

The packages are not published on PyPI yet, so `pip install fastmm-engine` fails. From a checkout, in a virtual environment and with the [requirements](#requirements) above installed:

```bash
python3 -m venv .venv
.venv/bin/pip install ".[hot]"
.venv/bin/pip install ./python/live
```

`pip install ".[hot,live]"` fails: the `live` extra asks PyPI for `fastmm-engine-live`. `./python/live` builds that package from the same checkout and links OpenSSL statically; with the `libssl-dev` of Ubuntu 24.04 that works as it is, and `FASTMM_OPENSSL_STATIC=OFF` links the shared libraries instead.

TLS connections, from the `fastmm-live` program or the Python package, trust the CA certificates in `SSL_CERT_FILE` and `SSL_CERT_DIR` if either is set, otherwise in the first existing file of `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/cert.pem`, otherwise in `certifi` (Python package only), otherwise in OpenSSL's built-in paths. A venue's `ca_file` adds to them.

## Use FastMM from your own project

- The [Quick start](quickstart.md) pulls FastMM into a CMake project with `FetchContent`; the [Tutorial: your first market maker](../tutorials/first-strategy/README.md) continues from there.
- [Register a strategy](../how-to/strategies/register-a-strategy.md) builds a project against an installed FastMM (`find_package(fastmm)`) or a source tree (`add_subdirectory`).
- Build your project with the compiler that built FastMM: a release install contains GCC LTO objects.
