# Run on a testnet or Binance Demo

This page takes you from a release build to a keyed session on one of the four practice
environments FastMM ships configs for: Binance Spot Demo Mode, the Binance Spot testnet, the Bybit
v5 spot testnet and the Deribit testnet. Each environment has its own keys; keys from one never
work on another, and none of them are live-exchange keys.

Every session follows the same order:

1. Build and run the tests.
2. `--dry-run` for 60 s: public market data only, no keys, no orders.
3. A short keyed run with `--duration`, watched with `fastmm-top`.
4. Stop it with Ctrl-C and check the shutdown line.
5. Reconcile the journal against the account ([Journals, replay and PnL](journals-replay-pnl.md)).

Before a longer session, read [Kill switch and shutdown](kill-switch-and-shutdown.md) and keep
[Troubleshooting](troubleshooting.md) open.

## 1. Build

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release
```

The binaries are in `build/release/bin/`.

## 2. Keys in the environment

Keys are only read from the environment, through `${VAR}` references in `[venues.<name>]`. Copy
`.env.example` to `.env` (it is ignored by git), fill in the variables for your environment and
load it into your shell:

```bash
cp .env.example .env        # then edit .env
set -a && . ./.env && set +a
```

| Environment | Config | Key variables |
|---|---|---|
| Binance Demo Mode | `configs/binance-demo.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Binance testnet | `configs/binance-testnet.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Bybit testnet | `configs/bybit-testnet.toml` | `FASTMM_BYBIT_API_KEY`, `FASTMM_BYBIT_API_SECRET` |
| Deribit testnet | `configs/deribit-testnet.toml` | `FASTMM_DERIBIT_CLIENT_ID`, `FASTMM_DERIBIT_CLIENT_SECRET` |

Binance Demo and the Binance testnet use the same variable names, so only one of them can be loaded
at a time. Never write a key into a config file: a literal value longer than 32 characters under a
key-like name is refused at startup, and `--allow-inline-secrets` exists only for throwaway local
tests (see [Configuration](../../reference/configuration.md#general-rules)).

## 3. The environments

### Binance Spot Demo Mode

- **Keys:** Binance Demo Trading. Switch to Demo Trading on binance.com and create a key under API
  Key Management. They are not testnet keys.
- **Endpoints** (from `configs/binance-demo.toml`): streams `wss://demo-stream.binance.com/stream`,
  WebSocket API `wss://demo-ws-api.binance.com/ws-api/v3`, REST `https://demo-api.binance.com`.
- **Market data** is realistic (it follows the real market), so fills behave like the real
  exchange: a quote away from the touch rarely trades.
- **Fees:** the config books 10 bps maker and taker (`[venues.binance.fees]`), which is what the
  demo account charged in our sessions (0.1% maker commission).
- **Commission asset:** buys are charged in the base asset (BTC) and sells in the quote asset
  (USDT). The engine and `tools/pnl_report.py` account for both; see
  [Journals, replay and PnL](journals-replay-pnl.md#a-real-example-binance-demo).
- **Quirks:** `stale_ms = 10000`, because quiet spells longer than the 2 s default would be reported
  as Stale, clear the engine book and force a resync. The Demo config also raises
  `[engine] min_requote_ticks` to 50 and `min_requote_interval_ms` to 1000 to keep the order rate
  well below Binance's limits.

### Binance Spot testnet

- **Keys:** generate them on <https://testnet.binance.vision> (HMAC keys; Ed25519 keys work with
  `key_type = "ed25519"` and `private_key_file`).
- **Endpoints** (from `configs/binance-testnet.toml`): streams
  `wss://stream.testnet.binance.vision/stream`, WebSocket API
  `wss://ws-api.testnet.binance.vision/ws-api/v3`, REST `https://testnet.binance.vision`.
- **Market data** is the testnet's own book, which is thin and often silent for several seconds;
  `stale_ms = 10000` for the same reason as above. Fills are rare and not representative.

### Bybit v5 spot testnet

- **Keys:** create a system-generated (HMAC) API key on the Bybit testnet site
  (testnet.bybit.com, API Management) with spot trading permission.
- **Endpoints** (from `configs/bybit-testnet.toml`): public
  `wss://stream-testnet.bybit.com/v5/public/spot`, trade `wss://stream-testnet.bybit.com/v5/trade`,
  private `wss://stream-testnet.bybit.com/v5/private` (derived from `ws_url` when `ws_private_url` is
  not set), REST `https://api-testnet.bybit.com`.
- **Quirks:** `supports_replace = false`: amend is implemented, but whether amend `qty` includes the
  filled quantity is not documented, so quotes are replaced with cancel and new. `recv_window_ms`
  is 5000 and `stale_ms` is 10000.

### Deribit testnet

- **Keys:** create an account on <https://test.deribit.com> and an API key under Account, API. The
  key's client id and client secret go into `FASTMM_DERIBIT_CLIENT_ID` and
  `FASTMM_DERIBIT_CLIENT_SECRET` (`api_key` and `api_secret` in the config).
- **Endpoints** (from `configs/deribit-testnet.toml`): WebSocket `wss://test.deribit.com/ws/api/v2`
  for market data and, on a second connection, authentication and orders; REST
  `https://test.deribit.com/api/v2` for reference data and the kill-switch
  `private/cancel_all_by_instrument`.
- **Instruments expire.** The shipped config quotes `BTC-25DEC26-*` options with `options_mm`.
  After that date `load_reference_data` fails on the expired symbols: replace the
  `[[instruments]]` with live names from `public/get_instruments?currency=BTC&kind=option`.
- **Quirks:** `stale_ms = 15000` and `dead_ms = 35000`, because option books can be quiet for
  longer than the heartbeat interval (`heartbeat_interval_s = 10`, the minimum). Order requests
  are rate limited locally by `matching_engine_rate` and `matching_engine_burst`; set them to your
  account tier. Prices of options are in BTC, so the `[risk]` notional limits are BTC amounts.
- **Status:** the private payloads follow Deribit's published schemas and a scripted fake exchange,
  not recorded traffic ([Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code)). Watch
  the first keyed session closely.

## 4. Dry run

Run the public market data for 60 s without keys. Use the config of your environment:

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml --dry-run --duration 60s
```

`--dry-run` drops the key variables, opens market data only and never sends an order. Once a
second the log prints one status line per venue. A healthy dry run shows `md=live` and all books
synced, for example `books=1/1`, with `malformed=0` and `dropped=0`. The user and order channels are
not opened in a dry run. If the run exits instead, look up the message in
[Troubleshooting](troubleshooting.md).

## 5. Short keyed run

Start with the shipped `[risk]` limits (they are sized for a few hundred USDT of exposure) and a
fixed duration. Write the journal and the log into one directory per session:

```bash
mkdir -p runs/demo-1
./build/release/bin/fastmm-live --config configs/binance-demo.toml --duration 10m \
  --journal runs/demo-1/session.fmj --log runs/demo-1/engine.log
```

In a second terminal, watch the session. The name is `[engine] name` from the config:

```bash
./build/release/bin/fastmm-top --name binance-demo
```

A keyed session logs `md=live user=live order=live` on the status line within a few seconds. For
example, a Demo session one second after connecting logged:

```text
[binance] md=live user=live order=live books=1/1 md_msgs=4 resyncs=0 malformed=0 dropped=0 orders=2 cancels=0 order_events=4 rest=2/0err reconnects=0 clock_offset_ms=175
```

`clock_offset_ms` is the venue clock minus the local clock; it matters only when it approaches
`recv_window_ms` (WSL2 hosts drift, see [Troubleshooting](troubleshooting.md)).

## 6. Stop and check

Press Ctrl-C (or let `--duration` elapse). The last log line must read
`shutdown took <n> ms (cancel_all ok)`:

```text
fastmm-live: shutdown took 697 ms (cancel_all ok)
```

Then confirm on the venue's website that no orders are left open. If the line says
`cancel_all FAILED`, follow [Kill switch and shutdown](kill-switch-and-shutdown.md#when-cancel_all-failed).

## 7. Opt-in live connector tests

The `live.*` test cases in `tests/venues/live_binance_test.cpp`, `live_bybit_test.cpp` and
`live_deribit_test.cpp` run each connector against its testnet: book sync, then a far post-only
order that is placed and cancelled. They are part of the venue test binary with the ctest label
`live`: the test presets (`ctest --preset release`, ...) exclude that label, and a plain `ctest`
runs them but they pass without doing anything unless `FASTMM_LIVE_TESTS=1` is set; the order steps
also need the key variables above. `FASTMM_BINANCE_ENV=demo` points the Binance test at Demo Mode
instead of the testnet.

```bash
cmake --build --preset release -j --target fastmm_venues_tests
FASTMM_LIVE_TESTS=1 FASTMM_BINANCE_ENV=demo ctest --test-dir build/release -L live --output-on-failure
# one venue only
FASTMM_LIVE_TESTS=1 ctest --test-dir build/release -L live -R 'venues\.live\.bybit' --output-on-failure
```

## Next

- [Go-live checklist](go-live-checklist.md)
- [Journals, replay and PnL](journals-replay-pnl.md)
- [Venue connectors](../../reference/venues.md) for what each connector implements
