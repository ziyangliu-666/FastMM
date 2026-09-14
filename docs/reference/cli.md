# Command lines

The five programs in `build/<preset>/bin/`, with their usage text and exit codes. The usage blocks
are generated from each program's `--help` by `tools/docs_cli_help.py`; do not edit them by hand
([Writing docs](../contributing/writing-docs.md#generated-pages)).

Programs built with `fastmm::cli::live`, `backtest` and `replay` for your own strategies
(`tutorial-live`, `mm-live`, ...) accept the same flags and print their own name in messages
([Register a strategy](../how-to/strategies/register-a-strategy.md)). Durations take a unit:
`1500ms`, `60s`, `5m`, `2h`.

## fastmm-live

Trades a strategy on the venues of a configuration until SIGINT, SIGTERM or `--duration`.

<!-- BEGIN cli-help fastmm-live -->
```text
usage: fastmm-live --config <file.toml> [options]
  --config <file>          engine / venue / strategy configuration (required)
  --strategy <name>        registered strategy (default: [strategy] name); a different
                           strategy ignores [strategy.params]
  --param <key=value>      strategy parameter override (repeatable)
  --duration <t>           stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT)
  --dry-run                public market data only: no API keys, no orders
  --record-raw <dir>       append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl
  --journal <path>         write the session journal (.fmj) here
  --no-journal             disable journaling even if [engine] journal = true
  --status <path>          live status file for fastmm-top (default /dev/shm/fastmm-<engine>.status)
  --no-status              do not publish live status
  --log <path>             write the log to a file (warnings are mirrored to stderr)
  --allow-inline-secrets   accept literal API secrets in the config file
  --list-strategies        print the strategies this binary can run and exit
  --format <text|json>     output format of --list-strategies (default text)
  --version | --help

API keys come from the environment through ${VAR} references in [venues.*],
e.g. FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET.
SIGINT/SIGTERM trips the kill switch, cancels all open orders and exits.
A kill switch the engine trips itself ([risk] max_loss, a full ring, every venue
killed) does the same and exits with code 6, unless [engine] on_kill = "stay".

Exit codes:
  0  stopped by --duration or SIGINT/SIGTERM, cancel_all ok
  2  bad command line, or a venue has no API keys
  3  bad config, strategy or parameters
  4  venue reference data failed to load
  5  runtime failure: cancel_all failed, journal, ring overflow, uncaught error
  6  kill switch tripped by the engine (on_kill = "exit"), cancel_all ok
```
<!-- END cli-help -->

### Exit codes

| Exit code | Meaning |
|---:|---|
| 0 | stopped by `--duration` or SIGINT/SIGTERM (also after a kill with `on_kill = "stay"`), `cancel_all ok` |
| 2 | bad command line, or a venue has no keys and there is no `--dry-run` |
| 3 | bad configuration (including an invalid `on_kill`), unknown strategy or parameter, a strategy name registered twice |
| 4 | a venue's reference data failed to load |
| 5 | runtime failure: `cancel_all FAILED`, the journal cannot be opened, a ring overflowed, an uncaught error |
| 6 | the engine tripped the kill switch itself (`max_loss`, a full ring, every venue killed) with `on_kill = "exit"`, and `cancel_all ok` |

A failed cancel-all takes precedence: code 5 whenever orders may still be resting. A test
(`apps.fastmm-live.exit_codes_documented`) checks that this table lists the codes of `--help`.

- `--strategy` with a strategy other than the configuration's ignores `[strategy.params]` and says
  so; `--param key=value` then sets the new strategy's parameters.
- The journal records the configuration after these overrides.
- The program installs process-wide SIGINT and SIGTERM handlers: the first signal trips the kill
  switch and starts the shutdown ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)).

## fastmm-backtest

Backtests a registered strategy on synthetic data, a journal or a CSV file.

<!-- BEGIN cli-help fastmm-backtest -->
```text
usage: fastmm-backtest --config <file.toml> [options]
  --data <path|synthetic>  market data: *.fmj journal, *.csv, or the synthetic
                           generator (default: [backtest] source/path)
  --strategy <name>        registered strategy (default: [strategy] name)
  --param <key=value>      strategy parameter override (repeatable)
  --out <dir>              write equity.csv fills.csv orders.csv summary.json
                           (default: [backtest] output_dir; '-' = don't write)
  --seed <n>               synthetic market / latency model seed
  --duration <seconds>     synthetic horizon
  --journal-out <file>     record the session for fastmm-replay
  --list-strategies        print registered strategies and their parameters
  --format <text|json>     output format of --list-strategies (default text)
  --version | --help
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the backtest ran |
| 2 | bad command line |
| 3 | bad configuration, unknown strategy or parameter, a strategy that cannot run in the simulator |
| 4 | the market data cannot be read |
| 5 | the run failed |

`--out` writes `equity.csv`, `fills.csv`, `orders.csv` and `summary.json`
([Configuration](configuration.md#backtest) describes `[backtest]`).

## fastmm-replay

Replays a journal through the same engine and strategy and, with `--verify`, compares the order
messages it sends with the recorded ones.

<!-- BEGIN cli-help fastmm-replay -->
```text
usage: fastmm-replay --journal <in.fmj> [options]
  --config <file.toml>  configuration to replay with (default: the one embedded
                        in a session journal; configs/backtest-example.toml for
                        a market-data journal)
  --strategy <name>     strategy to run (default: journal header / config)
  --out <file.fmj>      keep the re-simulated session journal (market-data input)
  --expect <sha256>     expected outbound hash (default: <journal>.sha256)
  --verify              fail (exit 1) unless every hash and message matches
  --version | --help
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the replay matched, or no verification was requested |
| 1 | mismatch: the first differing message is printed |
| 2 | bad command line, including a session journal without an embedded configuration and no `--config` |
| 3 | unreadable configuration or journal, unknown strategy |

[Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md#replay) explains what-if
replays and journals without outbound copies.

## fastmm-sim-exchange

A local exchange that speaks the Binance Spot API ([Simulated exchange](sim-exchange.md)).

<!-- BEGIN cli-help fastmm-sim-exchange -->
```text
usage: fastmm-sim-exchange [--config <file.toml>] [options]
  --config <file>        [[instruments]] + [sim] configuration (default: built-in BTCUSDT)
  --bind <ip>            listen address (default 127.0.0.1; 0.0.0.0 for containers)
  --port <n>             plain HTTP/WebSocket port (default 9080, 0 = ephemeral)
  --tls-port <n>         TLS port (default 9443, 0 = ephemeral)
  --no-tls               do not open the TLS listener
  --tls-cert <pem>       certificate chain (default tests/fixtures/tls/cert.pem)
  --tls-key <pem>        private key (default tests/fixtures/tls/key.pem)
  --seed <n>             generator seed (overrides [sim] seed)
  --duration <t>         stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT/SIGTERM)
  --stats-interval <t>   print statistics every t (default 5s, 0 = only at exit)
  --version | --help

Endpoints: REST /api/v3/*, market data /stream?streams=... and /ws/<stream>,
WebSocket API /ws-api/v3. The API key/secret come from [sim.account] or
FASTMM_SIM_API_KEY / FASTMM_SIM_API_SECRET. See docs/reference/sim-exchange.md.
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | stopped by `--duration` or a signal |
| 2 | bad command line |
| 3 | bad configuration |
| 4 | cannot listen on a port |

## fastmm-top

A terminal dashboard of a running `fastmm-live` session ([Status file](status-file.md)).

<!-- BEGIN cli-help fastmm-top -->
```text
usage: fastmm-top [--name <engine name> | --path <status file>] [options]

  --name <engine>     read /dev/shm/fastmm-<engine>.status ([engine] name in the config)
  --path <file>       read this status file (fastmm-live --status <file>)
  --interval <ms>     refresh period, default 500
  --once              print one frame and exit (exit code 3 if no status is available)
  --no-color          plain output
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | quit, or `--once` printed a frame |
| 2 | bad command line |
| 3 | with `--once`: no status file, or a file written by a different FastMM build |
