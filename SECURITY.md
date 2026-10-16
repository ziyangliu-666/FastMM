# Security

## Reporting a vulnerability

Report it privately through GitHub: <https://github.com/ziyangliu-666/FastMM/security/advisories/new>. Do not open a public issue for it. Include the version (`fastmm-live --version` or `fastmm.__version__`), what an attacker needs and what they get.

## Supported versions

Fixes go into `main` and the next release of the latest minor version. Older releases are not patched ([Versions and compatibility](docs/reference/compatibility.md)).

## Scope

In scope: the engine and its programs, the venue connectors (request signing, key handling, TLS), the `fastmm-engine` and `fastmm-engine-live` wheels, the release tarball and the production container image (`deploy/docker/Dockerfile.production`).

Out of scope: the exchanges' own APIs; the simulators `fastmm-sim-exchange` and `fastmm-sim-itch`, and the demo image of `deploy/docker/compose.yml`, which runs as root with test TLS certificates by design; losses from a strategy or a configuration; bugs in a dependency that do not depend on how FastMM uses it, which belong upstream.

## How keys are handled

- API keys come from environment variables (`FASTMM_*_API_KEY/SECRET`) through `${VAR}` substitution in TOML; literal secrets are refused ([Configuration](docs/reference/configuration.md#general-rules)). Never commit keys; `.env` is git-ignored.
- The logger redacts values wrapped in `Secret<T>`; journals record the configuration without `api_key`, `api_secret` and `api_passphrase`.

## Configs that reach real accounts

Most configs in `configs/` point at a testnet, Binance Demo Mode or a local simulator. Four point at production hosts: `coinbase-advanced.toml` (Coinbase Advanced Trade has none), `gate-usdt.toml`, `hyperliquid.toml` and `lighter.toml`; the comment at the top of each of the last three gives the testnet hosts. `fastmm init --profile production` writes `production.toml` for a Binance Spot account. Start any of them with `--dry-run`, which sends no orders.
