# Install

FastMM comes as a Python package with the engine inside, or as C++ programs built from this repository.

| You want | Install |
|---|---|
| strategies in Python, backtests, the `fastmm` command | `pip install "fastmm-engine[hot]"` ([below](#without-building-it)) |
| the same, trading on venues | add `pip install "fastmm-engine[live]"` ([Python](#python)) |
| the C++ programs from a release | a tarball or a container image ([Deploy a release](../how-to/operations/deploy.md)) |
| strategies in C++, or work on FastMM | [Build from source](#build-from-source) |

What may change between releases: [Versions and compatibility](../reference/compatibility.md).

## Without building it

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"     # CPython 3.10 or later
fastmm init my-mm && cd my-mm        # a config, a strategy and a backtest
python backtest.py
```

On Debian and Ubuntu, `python3 -m venv` needs `sudo apt install python3-venv` first. The [Quick start](quickstart.md) walks through the files `fastmm init` writes.

The wheels are built for Linux x86-64 only. On macOS or Windows, run the same steps in a Linux container; Apple silicon runs it under emulation, and WSL2 takes the Linux steps as they are:

```bash
docker run --rm -it --platform linux/amd64 -v "$PWD":/work -w /work python:3.12-slim \
  sh -c 'pip install "fastmm-engine[hot]" && fastmm init my-mm && cd my-mm && python backtest.py'
```

The project lands in `./my-mm` on the host. `fastmm init` takes a venue and a profile for its `live.toml`:

| Option | `live.toml` trades on |
|---|---|
| none | Binance Spot Demo Mode |
| `--venue binance-testnet` | the Spot testnet at testnet.binance.vision, with its own keys |
| `--venue binance` | the real Binance Spot market; `--dry-run` records it without keys |
| `--profile production` | adds `production.toml`: a dedicated host with busy spinning, the engine and network threads on their own cores, locked memory, an Ed25519 key and SBE market data |

## Build from source

The steps below build the repository; all commands run from its root.

### Requirements

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | gcc 13 or newer, or clang 16 or newer | CI uses gcc 13.3 and clang 18 |
| CMake | 3.25 or newer | `scripts/bootstrap.sh` installs it with pip when it is missing |
| Ninja | any | all presets use it |
| OpenSSL | 3.0 or newer, development headers | networking; not needed with `FASTMM_BUILD_NET=OFF` |
| make | any | liburing's `configure` script calls it; not needed with `FASTMM_BUILD_NET=OFF` |
| Python | 3.10 or newer | the tools in `tools/` and the Python package |

On Ubuntu 24.04, whose `g++` is gcc 13:

```bash
sudo apt install g++ make cmake ninja-build libssl-dev python3 python3-venv
```

CPM downloads and pins every other dependency, from fmt to pybind11 ([Dependencies](../contributing/dependencies.md)).

### Build

```bash
./scripts/bootstrap.sh
cmake --build --preset release -j
ctest --preset release -j"$(nproc)"
```

`bootstrap.sh` checks the toolchain, creates the CPM download cache ([Dependencies](../contributing/dependencies.md)) and configures the `release` preset; `--dev` also configures `debug` and installs the pre-commit hooks. The programs are in `build/release/bin/`.

### Check the build

```bash
./build/release/bin/fastmm-backtest --list-strategies
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --out -
```

The first command lists the built-in strategies with their parameters. The second backtests `basic_mm` for 60 s of simulated time and prints a summary:

<!-- output: fastmm-backtest --config configs/backtest-example.toml --data synthetic --out - -->
```text
backtest basic_mm  seed=1  md_events=15955  steps=16745  wall=0.01s
  net pnl                        -12.1548
  realized / unrealized / fees   0.0086 / 0.0001 / 12.1635
  fills (maker / taker)          147 (147 / 0)
...
```

### Presets

| Preset | Build | Use |
|---|---|---|
| `release` | gcc, `-O3`, LTO, portable x86-64-v2 | everyday work, releases, weekly CI |
| `release-nolto` | as `release` without LTO | CI on pushes and pull requests |
| `release-native` | as `release` with `-march=native` | benchmarks on this machine only |
| `release-dpdk` | as `release` with the DPDK receive backend linked in | `rx_backend = "dpdk"` ([Receive a multicast feed](../how-to/operations/multicast-feeds.md)), `scripts/package-release.sh` |
| `debug` | gcc, `-O0 -g`, no-allocation assertions | debugging, clang-tidy |
| `asan` | gcc, AddressSanitizer and UBSan | `cmake --workflow --preset asan` |
| `tsan`, `clang-tsan` | ThreadSanitizer | `cmake --workflow --preset tsan` |
| `clang-release` | clang, LTO | the second compiler in CI |
| `python` | the Python module only, no networking | wheel builds |

`ctest --preset <name>` runs a preset's tests, without the opt-in labels `live` (real testnets) and `autobahn`. The presets set no job count: pass `-j` or set `CTEST_PARALLEL_LEVEL`.

`ctest --test-dir build/release -L autobahn` runs the [Autobahn|Testsuite](https://github.com/crossbario/autobahn-testsuite) WebSocket cases 1-7, 9 and 10 against the client and the server. It needs Docker or a `wstest` named by `FASTMM_WSTEST`, and skips without either. Reports go to `build/release/autobahn/`.

### Optional codecs

Two wire codecs have no connector behind them, so the default build leaves them out with their tests and benchmarks. An option builds one:

| Option | Builds | Documented in |
|---|---|---|
| `FASTMM_CODEC_FIX=ON` | FIX 4.4 framing, session layer, decoder and encoder | [FIX 4.4](../reference/codecs/fix.md) |
| `FASTMM_CODEC_MDP3=ON` | CME MDP 3.0 (SBE) packet framing, decoder, feed handler | [CME MDP 3.0](../reference/codecs/cme-mdp3.md) |

```bash
cmake --preset release -DFASTMM_CODEC_FIX=ON -DFASTMM_CODEC_MDP3=ON
```

## Docker

```bash
docker compose -f deploy/docker/compose.yml up --build
```

This starts `fastmm-sim-exchange` and `fastmm-live` in two containers, trading `basic_mm` against the simulator for 120 s (`configs/sim-docker.toml`); journals go to `runs/`. The image is a demo that runs as root with test TLS certificates; the production image is `deploy/docker/Dockerfile.production` ([Deploy a release](../how-to/operations/deploy.md#run-the-container)).

## WSL2

| What you see or set | Why |
|---|---|
| `host wall clock stepped by <n> ns ...` and `TSC recalibration stepped the engine clock by <n> ns ...` in the log | WSL2 steps the Linux wall clock to follow Windows, and the engine clock follows it ([Clock calibration](../explanation/architecture.md#clock-calibration)) |
| keep `[engine] spin_mode = "adaptive"` and `cpu = -1`, as the shipped configurations do | a desktop has no cores to dedicate ([Configuration](../reference/configuration.md#engine)) |

## Python

The `fastmm-engine` package (imported as `fastmm`) runs backtests on CPython 3.10 or later; `fastmm-engine-live` adds live trading ([Python](../python.md)).

| Extra | Installs |
|---|---|
| `fastmm-engine[live]` | `fastmm-engine-live` of the same version: networking, venue connectors and OpenSSL 3 inside the extension module |
| `fastmm-engine[hot]` | numba and llvmlite for hot hooks, pandas for `to_pandas()`, `sweep_frame` and `markout_frame` |

```bash
pip install "fastmm-engine[hot]"    # backtests, Python strategies, the fastmm command
pip install "fastmm-engine[live]"   # adds fastmm-engine-live of the same version
```

Both wheels are `manylinux_2_28` x86-64 and must be the same version ([Versions and compatibility](../reference/compatibility.md)). `fastmm` and `python -m fastmm` are the same command; `fastmm run module:Class --config file.toml` runs a strategy live ([Run a Python strategy live](../how-to/strategies/python-live.md)).

### Install from source

From a checkout, in a virtual environment and with the [requirements](#requirements) above installed:

```bash
python3 -m venv .venv
.venv/bin/pip install ".[hot]"
.venv/bin/pip install ./python/live
```

Install `./python/live` from the checkout rather than the `live` extra: the extra fetches `fastmm-engine-live` from PyPI, which has no match for a version that is not a release. It links OpenSSL statically; `FASTMM_OPENSSL_STATIC=OFF` links the shared libraries.

TLS connections, from `fastmm-live` or the Python package, trust the first CA source found:

1. `SSL_CERT_FILE` or `SSL_CERT_DIR`, if either is set;
2. the first of `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/cert.pem`;
3. `certifi`, in the Python package;
4. OpenSSL's built-in paths.

A venue's `ca_file` adds to them.

## Use FastMM from your own project

- The [Quick start in C++](cpp-quickstart.md) pulls FastMM into a CMake project with `FetchContent`; the [Tutorial: your first market maker](../tutorials/first-strategy/README.md) continues from there.
- [Register a strategy](../how-to/strategies/register-a-strategy.md) builds a project against an installed FastMM (`find_package(fastmm)`) or a source tree (`add_subdirectory`).
- Build your project with the compiler that built FastMM: a release install contains GCC LTO objects.
