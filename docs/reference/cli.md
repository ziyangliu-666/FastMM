# Command lines

<!-- The usage blocks are generated from each program's --help by tools/docs_cli_help.py; do not
edit them by hand (docs/contributing/writing-docs.md#generated-pages). -->

Programs are built into `build/<preset>/bin/`. Programs built with `fastmm::cli::live`, `backtest` and `replay` for your own strategies (`tutorial-live`, `mm-live`, ...) accept the same flags and print their own name in messages ([Register a strategy](../how-to/strategies/register-a-strategy.md)). Durations take a unit: `1500ms`, `60s`, `5m`, `2h`.

## fastmm-live

Trades a strategy on the venues of a configuration until SIGINT, SIGTERM or `--duration`.

<!-- BEGIN cli-help fastmm-live -->
```text
Trades a strategy on the venues of a configuration.

usage: fastmm-live [OPTIONS]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --config <file>             engine / venue / strategy configuration (required)
  --strategy <name>           registered strategy (default: [strategy] name); a different
                              strategy ignores [strategy.params]
  --param <key=value>         strategy parameter override (repeatable)
  --duration <t>              stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT)
  --dry-run                   public market data only: no API keys, no orders
  --record-raw <dir>          append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl
  --journal <path>            write the session journal (.fmj) here
  --no-journal                disable journaling even if [engine] journal = true
  --status <path>             live status file for fastmm-top (default
                              /dev/shm/fastmm-<engine>.status)
  --no-status                 do not publish live status
  --control <path>            control socket for fastmm-ctl (default
                              <journal_dir>/<engine>.ctl, mode 0600)
  --no-control                do not open a control socket
  --clear-kill                clear a latched kill switch and the cumulative PnL before
                              starting; arms the whole [risk] max_loss budget again
  --log <path>                write the log to a file (warnings are mirrored to stderr)
  --allow-inline-secrets      accept literal API secrets in the config file
  --list-strategies           print the strategies this binary can run and exit
  --format <text|json>        output format of --list-strategies (default text)

API keys come from the environment through ${VAR} references in [venues.*],
e.g. FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET.
SIGINT/SIGTERM trips the kill switch, cancels all open orders and exits.
SIGHUP clears the kill switch and resumes quoting (on_kill = "stay").
fastmm-ctl talks to the control socket: pull, resume, param, limits, flatten,
kill, unkill, stop and status.
A kill switch the engine trips itself ([risk] max_loss, a full ring, every venue
killed) does the same and exits with code 6, unless [engine] on_kill = "stay".
A max_loss trip is latched in [engine] kill_file: the next start refuses to trade
(exit code 6) until --clear-kill or the file is removed.

Exit codes:
  0  stopped by --duration or SIGINT/SIGTERM, cancel_all ok
  2  bad command line, or a venue has no API keys
  3  bad config, strategy or parameters
  4  venue reference data failed to load
  5  runtime failure: cancel_all failed, journal, ring overflow, uncaught error
  6  kill switch tripped by the engine (on_kill = "exit"), or a latched max_loss trip
  7  a Python strategy's slow tier failed (python -m fastmm run), cancel_all ok
```
<!-- END cli-help -->

### Exit codes

| Exit code | Meaning |
|---:|---|
| 0 | stopped by `--duration` or SIGINT/SIGTERM with `cancel_all ok`, also after a kill with `[engine] on_kill = "stay"`; `--help`, `--version` and `--list-strategies` |
| 2 | bad command line, including a `--log` file that cannot be opened; a `${VAR}` in `[venues.*]` that is not set, except `api_key` and `api_secret` with `--dry-run` |
| 3 | the configuration does not load (including an invalid `on_kill` or a literal secret), no instruments or duplicate symbols, an unknown venue `kind`, a strategy that is unknown or cannot run live, an unknown parameter or invalid value, a strategy name registered twice by different code, a `[storage] backend` that is not registered or cannot be opened ([Storage](storage.md)) |
| 4 | a venue's reference data failed to load |
| 5 | `cancel_all FAILED`, whatever stopped the session; the journal cannot be opened or written (a full filesystem trips the kill switch, [Journal format](journal-format.md#durability)); a venue's order-event ring overflowed; an uncaught error |
| 6 | the engine tripped the kill switch itself (`[risk] max_loss`, a full outbound or journal ring, every venue killed, a failing hot hook of a Python strategy) with `on_kill = "exit"`, and `cancel_all ok`; also a start refused because a `max_loss` trip is latched in `[engine] kill_file` ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md#the-latched-loss-budget)) |
| 7 | a Python strategy's slow tier failed, and `cancel_all ok` (`python -m fastmm run` and `fastmm.run_live`; `fastmm-live` does not return it) |

- The journal records the configuration after `--strategy` and `--param`.
- `python -m fastmm run` and `fastmm.run_live` run the same session for a Python strategy with these exit codes ([Live sessions](python-api.md#live-sessions)).
- `fastmm::cli::live` installs process-wide SIGINT and SIGTERM handlers. The first signal starts the shutdown ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)).

## fastmm-backtest

Backtests a registered strategy on synthetic data, a journal, a CSV file or a public archive ([Market-data sources](data-sources.md)).

<!-- BEGIN cli-help fastmm-backtest -->
```text
Backtests a registered strategy.

usage: fastmm-backtest [OPTIONS]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --config <file.toml>        engine / strategy / backtest configuration (required)
  --data <spec>               market data: 'synthetic', a *.fmj / *.csv path, or
                              <source>:<args> (default: [backtest] source/path).
                              `fastmm-data list` prints the sources
  --strategy <name>           registered strategy (default: [strategy] name)
  --param <key=value>         strategy parameter override (repeatable)
  --out <dir>                 write equity.csv fills.csv orders.csv summary.json (default:
                              [backtest] output_dir; '-' = don't write)
  --seed <n>                  synthetic market / latency model seed
  --duration <seconds>        synthetic horizon
  --journal-out <file>        record the session for fastmm-replay
  --list-strategies           print registered strategies and their parameters
  --format <text|json>        output format of --list-strategies (default text)
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the backtest ran |
| 2 | bad command line |
| 3 | bad configuration, unknown strategy or parameter, a strategy that cannot run in the simulator |
| 4 | the market data cannot be read |
| 5 | the run failed |

The flags override `[backtest]` ([Configuration](configuration.md#backtest)).

## fastmm-data

Lists the market-data sources a backtest can read and packs any of them into a journal ([Market-data sources](data-sources.md)). Downloading the files a source reads is `python3 -m fastmm.data fetch`.

<!-- BEGIN cli-help fastmm-data -->
```text
Lists the market-data sources a backtest can read and packs them into journals.

usage: fastmm-data [OPTIONS] [SUBCOMMAND]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --data <spec>               convert: source, e.g. binance:BTCUSDT,2024-03-27
  --config <file.toml>        convert: backtest config supplying the instruments
  --out <file.fmj>            convert: output journal
  --seed <n>                  convert: session id stamped in the journal (default 1)

SUBCOMMANDS:
  list                        registered data sources, their options and what each one
                              carries
  convert                     decode a source into an .fmj journal, the format a backtest
                              replays fastest

Downloading what a source reads: python3 -m fastmm.data fetch --help
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the command ran |
| 2 | bad command line |
| 3 | bad configuration |
| 4 | the source is unknown, its options are wrong, or its files are missing |
| 5 | the journal could not be written |

## fastmm-replay

Replays a journal through the same engine and strategy and, with `--verify`, compares the order messages it sends with the recorded ones.

<!-- BEGIN cli-help fastmm-replay -->
```text
Replays a journal through the same engine and strategy.

usage: fastmm-replay [OPTIONS]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --journal <in.fmj>          session or market-data journal to replay (required)
  --config <file.toml>        configuration to replay with (default: the one embedded in a
                              session journal; configs/backtest-example.toml for a
                              market-data journal)
  --strategy <name>           strategy to run (default: journal header / config)
  --out <file.fmj>            keep the re-simulated session journal (market-data input)
  --expect <sha256>           expected outbound hash (default: <journal>.sha256)
  --verify                    fail (exit 1) unless every hash and message matches
  --allow-incomplete          replay a journal the writer never closed; its tail is missing,
                              so the outbound comparison proves nothing
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the replay matched, or no verification was requested |
| 1 | mismatch: the first differing message is printed |
| 2 | bad command line, including a session journal without an embedded configuration and no `--config` |
| 3 | unreadable configuration or journal, unknown strategy |

[Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md#replay) explains what-if replays and journals without outbound copies.

## fastmm-pnl

Answers the daily questions from the [store](storage.md) a session wrote: what it traded, its PnL by day and instrument, and what the last session left behind. It reads the store, never a journal.

<!-- BEGIN cli-help fastmm-pnl -->
```text
What a deployment traded, read from the store.

usage: fastmm-pnl [OPTIONS] [SUBCOMMAND]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --store <path>              store file (default runs/<engine>.db)
  --backend <name>            storage backend (default sqlite)
  --engine <name>             [engine] name to filter on
  --session <id>              one session id
  --instrument <sym>          one symbol
  --since <day>               inclusive UTC day, YYYY-MM-DD, or today|yesterday
  --until <day>               inclusive UTC day, YYYY-MM-DD, or today|yesterday
  --day <day>                 shorthand for --since <day> --until <day>
  --limit <n>                 at most n rows
  --csv                       comma-separated output instead of an aligned table

SUBCOMMANDS:
  sessions                    one row per session: when it ran, what it made, how it ended
  fills                       one row per execution
  orders                      one row per order, in its last known state
  pnl                         realised, fees and net by UTC day and instrument
  positions                   the last position snapshot of each session and instrument
  recover                     what the newest session left behind
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the query ran |
| 2 | bad command line, unknown backend, or a store that cannot be opened or read |
| 3 | `recover` found no session |

[Query what you traded](../how-to/operations/query-trading-records.md) works through the questions.

## fastmm-sim-exchange

A local exchange that speaks the Binance Spot API ([Simulated exchange](sim-exchange.md)).

<!-- BEGIN cli-help fastmm-sim-exchange -->
```text
A Binance Spot-compatible simulated exchange.

usage: fastmm-sim-exchange [OPTIONS]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --config <file>             [[instruments]] + [sim] configuration (default: built-in
                              BTCUSDT)
  --bind <ip>                 listen address (default 127.0.0.1; 0.0.0.0 for containers)
  --port <n>                  plain HTTP/WebSocket port (default 9080, 0 = ephemeral)
  --tls-port <n>              TLS port (default 9443, 0 = ephemeral)
  --no-tls                    do not open the TLS listener
  --tls-cert <pem>            certificate chain (default tests/fixtures/tls/cert.pem)
  --tls-key <pem>             private key (default tests/fixtures/tls/key.pem)
  --seed <n>                  generator seed (overrides [sim] seed)
  --duration <t>              stop after t (e.g. 60s, 5m, 1500ms; default: until
                              SIGINT/SIGTERM)
  --stats-interval <t>        print statistics every t (default 5s, 0 = only at exit)

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

## fastmm-sim-itch

A Nasdaq-style exchange: ITCH 5.0 over MoldUDP64 multicast, re-requests, GLIMPSE 5.0 and OUCH 5.0, with a wire-to-wire histogram ([fastmm-sim-itch](sim-itch.md)).

<!-- BEGIN cli-help fastmm-sim-itch -->
```text
A Nasdaq-style simulated exchange: TotalView-ITCH 5.0, GLIMPSE and OUCH.

usage: fastmm-sim-itch [OPTIONS]

OPTIONS:
  -h, --help                  print this help and exit
  --version                   print the version and exit
  --config <file>             [[instruments]] + [sim] configuration (default: FMAA, FMBB)
  --bind <ip>                 re-request, GLIMPSE and OUCH servers (default 127.0.0.1)
  --rerequest-port <n>        MoldUDP64 re-request server, UDP (default 31000, 0 =
                              ephemeral)
  --glimpse-port <n>          GLIMPSE 5.0 over SoupBinTCP (default 31010)
  --ouch-port <n>             OUCH 5.0 over SoupBinTCP (default 31020)
  --line-a <addr:port>        line A: multicast group or unicast address (default
                              239.192.0.1:31001)
  --line-b <addr:port>        line B (default 239.192.0.2:31002, off = none)
  --interface <if>            multicast interface, name or IPv4 address (default lo)
  --source <ip>               local address of the multicast socket
  --ttl <n>                   multicast TTL (default 1)
  --drop-a <p>                probability of not sending a data datagram on line A
  --drop-b <p>                the same on line B
  --drop-seed <n>             seed of the drop draws
  --rate <n>                  datagrams per second per line (default 0 = unpaced)
  --burst <n>                 datagrams per sendmmsg call (default 32)
  --speed <x>                 generator time per wall-clock time (default 1)
  --seed <n>                  generator seed
  --busy-poll                 never block waiting for I/O
  --cpu <n>                   pin the simulator thread to a CPU
  --duration <t>              stop after t (e.g. 60s, 5m, 1500ms; default: until
                              SIGINT/SIGTERM)
  --stats-interval <t>        print statistics every t (default 5s, 0 = only at exit)
  --summary-json <file>       write the wire-to-wire summary at exit

Orders time wire-to-wire when their OUCH ClOrdID is 'T' + 13 digits of the triggering
ITCH sequence number. See docs/reference/sim-itch.md.
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | stopped by `--duration` or a signal |
| 2 | bad command line |
| 3 | bad configuration, CPU pinning failed, or the summary file cannot be written |
| 4 | cannot open a socket |

## fastmm-ctl

Sends one command to a running `fastmm-live` session over its control socket
([Operating a running session](../how-to/operations/operate-a-running-session.md)).

<!-- BEGIN cli-help fastmm-ctl -->
```text
usage: fastmm-ctl [--name <engine> | --path <socket> | --config <file.toml>] <command>

  --name <engine>     talk to <dir>/<engine>.ctl ([engine] name in the config)
  --dir <directory>   where --name looks, default runs ([engine] journal_dir)
  --path <socket>     talk to this socket (fastmm-live --control <path>)
  --config <file>     take the engine name and journal_dir from a configuration file
  --timeout <ms>      how long to wait for the reply, default 2000
  --help

commands (one per datagram; the reply starts with ok or error)
  pull [--instrument SYM | --venue NAME]   stop quoting: everywhere, or in that scope
  resume [--instrument SYM | --venue NAME] quote again; without a scope it also clears
                                           every scoped pull and stops a running flatten
  param <name>=<value> ... [--instrument SYM]  new strategy parameters, validated here
  limits <key>=<value> ...                 new risk limits (the [risk] keys)
  flatten [--instrument SYM] [--max-slippage-bps N]  work the position off, reduce-only
  kill                                     trip the kill switch (quotes pulled, all
                                           orders cancelled; the position stays)
  unkill                                   clear it and quote again (SIGHUP)
  stop                                     shut the session down (SIGTERM)
  status                                   one line per topic
  help                                     this text

examples:
  fastmm-ctl --name mm status
  fastmm-ctl --name mm pull --instrument BTCUSDT
  fastmm-ctl --name mm param half_spread_bps=8
  fastmm-ctl --name mm limits max_position=0.5 orders_per_sec=10
  fastmm-ctl --name mm flatten --max-slippage-bps 15

Exit codes: 0 the session answered ok, 1 it answered error, 2 bad command line,
3 no session answered (no socket, or it is not running).
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | the session answered `ok`, or printed its `status` or `help` |
| 1 | the session answered `error` (an unknown command, a bad argument, a parameter the schema refuses, a full control ring) |
| 2 | bad command line |
| 3 | no session answered: no socket at that path, nobody listening, or no reply within `--timeout` |

## fastmm-top

A terminal dashboard of a running `fastmm-live` session ([Status file](status-file.md)).

<!-- BEGIN cli-help fastmm-top -->
```text
usage: fastmm-top [--name <engine name> | --path <status file>] [options]

  --name <engine>     read /dev/shm/fastmm-<engine>.status ([engine] name in the config)
  --path <file>       read this status file (fastmm-live --status <file>)
  --interval <ms>     refresh period, default 500
  --once              print one frame and exit (exit code 3 if no status is available)
  --json              print the snapshot as one JSON object and exit (implies --once)
  --no-color          plain output
  --metrics <[host:]port>  serve the snapshot at /metrics in Prometheus text format
                      until SIGINT, instead of drawing (default host 127.0.0.1; off
                      unless given). Scraping costs the engine nothing: this process
                      reads the status file, the engine never sees the request.
```
<!-- END cli-help -->

| Exit code | Meaning |
|---:|---|
| 0 | quit, or `--once` printed a frame |
| 2 | bad command line |
| 3 | with `--once`: no status file, or a file written by a different FastMM build |
