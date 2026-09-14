# Run on a testnet or Binance Demo

FastMM ships configs for four practice environments: Binance Spot Demo Mode, the Binance Spot testnet, the Bybit v5 spot testnet and the Deribit testnet. Each environment has its own keys; keys from one never work on another, and none of them are live-exchange keys.

Before a longer session, read [Kill switch and shutdown](kill-switch-and-shutdown.md).

## 1. Build

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release
```

The binaries are in `build/release/bin/`.

## 2. Keys in the environment

Keys are read from the environment through `${VAR}` references in `[venues.<name>]` ([Configuration](../../reference/configuration.md#general-rules)). Copy `.env.example` to `.env` (it is ignored by git), fill in the variables for your environment and load it into your shell:

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

Binance Demo and the Binance testnet use the same variable names, so only one of them can be loaded at a time.

## 3. The environments

The shipped configs raise `stale_ms` for quiet feeds ([Venue connectors](../../reference/venues.md#configuration-keys)).

### Binance Spot Demo Mode

- Keys: Binance Demo Trading. Switch to Demo Trading on binance.com and create a key under API Key Management.
- Endpoints (from `configs/binance-demo.toml`): streams `wss://demo-stream.binance.com/stream`, WebSocket API `wss://demo-ws-api.binance.com/ws-api/v3`, REST `https://demo-api.binance.com`.
- Market data follows the real market; quotes away from the touch rarely fill.
- Fees: the config books 10 bps maker and taker (`[venues.binance.fees]`), the commission the Demo account charged in our sessions.
- Commission asset: buys are charged in the base asset (BTC) and sells in the quote asset (USDT) ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)).
- Config: `stale_ms = 10000`; `[engine] min_requote_ticks = 50` and `min_requote_interval_ms = 1000` keep the order rate below Binance's limits.

### Binance Spot testnet

- Keys: generate them on <https://testnet.binance.vision> (HMAC keys; Ed25519 keys work with `key_type = "ed25519"` and `private_key_file`).
- Endpoints (from `configs/binance-testnet.toml`): streams `wss://stream.testnet.binance.vision/stream`, WebSocket API `wss://ws-api.testnet.binance.vision/ws-api/v3`, REST `https://testnet.binance.vision`.
- Market data is the testnet's own book: thin and often silent for several seconds (`stale_ms = 10000`). Fills are rare and not representative.

### Bybit v5 spot testnet

- Keys: create a system-generated (HMAC) API key on the Bybit testnet site (testnet.bybit.com, API Management) with spot trading permission.
- Endpoints (from `configs/bybit-testnet.toml`): public `wss://stream-testnet.bybit.com/v5/public/spot`, trade `wss://stream-testnet.bybit.com/v5/trade`, private `wss://stream-testnet.bybit.com/v5/private` (derived from `ws_url` when `ws_private_url` is not set), REST `https://api-testnet.bybit.com`.
- Config: `supports_replace = false`, so quotes are replaced with cancel and new (Bybit does not document whether amend `qty` includes the filled quantity). `recv_window_ms = 5000` and `stale_ms = 10000`.

### Deribit testnet

- Keys: create an account on <https://test.deribit.com> and an API key under Account, API. The key's client id and client secret go into `FASTMM_DERIBIT_CLIENT_ID` and `FASTMM_DERIBIT_CLIENT_SECRET` (`api_key` and `api_secret` in the config).
- Endpoints (from `configs/deribit-testnet.toml`): WebSocket `wss://test.deribit.com/ws/api/v2` for market data and, on a second connection, authentication and orders; REST `https://test.deribit.com/api/v2` for reference data and the kill-switch `private/cancel_all_by_instrument`.
- Instruments expire. The shipped config quotes `BTC-25DEC26-*` options with `options_mm`. After that date `load_reference_data` fails on the expired symbols: replace the `[[instruments]]` with live names from `public/get_instruments?currency=BTC&kind=option`.
- Config: `stale_ms = 15000` and `dead_ms = 35000`, above the heartbeat interval (`heartbeat_interval_s = 10`, the minimum). Order requests are rate limited locally by `matching_engine_rate` and `matching_engine_burst`; set them to your account tier. Prices of options are in BTC, so the `[risk]` notional limits are BTC amounts.
- Status: the private payloads follow Deribit's published schemas and a scripted fake exchange, not recorded traffic ([Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code)).

## 4. Dry run

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml --dry-run --duration 60s
```

`--dry-run` drops the key variables, opens market data only and never sends an order. Once a second the log prints one status line per venue. A working dry run shows `md=live` and all books synced, for example `books=1/1`, with `malformed=0` and `dropped=0`.

## 5. Short keyed run

Start with the shipped `[risk]` limits (sized for a few hundred USDT of exposure) and a fixed duration. Write the journal and the log into one directory per session:

```bash
mkdir -p runs/demo-1
./build/release/bin/fastmm-live --config configs/binance-demo.toml --duration 10m \
  --journal runs/demo-1/session.fmj --log runs/demo-1/engine.log
```

In a second terminal, watch the session. The name is `[engine] name` from the config:

```bash
./build/release/bin/fastmm-top --name binance-demo
```

A Demo session one second after connecting:

```text
[binance] md=live user=live order=live books=1/1 md_msgs=4 resyncs=0 malformed=0 dropped=0 orders=2 cancels=0 order_events=4 rest=2/0err reconnects=0 clock_offset_ms=175
```

`clock_offset_ms` is the venue clock minus the local clock; signed requests fail as it nears `recv_window_ms` ([Troubleshooting](troubleshooting.md#clock)).

## 6. Stop and check

Press Ctrl-C or let `--duration` elapse, then read the last log lines ([Reading the last lines](kill-switch-and-shutdown.md#reading-the-last-lines)) and confirm that the venue shows no open orders ([Go-live checklist](go-live-checklist.md#stopping)). Reconcile the journal against the account with [Check PnL](journals-replay-pnl.md#check-pnl).

## 7. Opt-in live connector tests

The `live.*` test cases in `tests/venues/live_binance_test.cpp`, `live_bybit_test.cpp` and `live_deribit_test.cpp` run each connector against its testnet: book sync, then a far post-only order that is placed and cancelled. They are part of the venue test binary with the ctest label `live`: the test presets (`ctest --preset release`, ...) exclude that label, and a plain `ctest` runs them but they pass without doing anything unless `FASTMM_LIVE_TESTS=1` is set; the order steps also need the key variables above. `FASTMM_BINANCE_ENV=demo` points the Binance test at Demo Mode instead of the testnet.

```bash
cmake --build --preset release -j --target fastmm_venues_tests
FASTMM_LIVE_TESTS=1 FASTMM_BINANCE_ENV=demo ctest --test-dir build/release -L live --output-on-failure
# one venue only
FASTMM_LIVE_TESTS=1 ctest --test-dir build/release -L live -R 'venues\.live\.bybit' --output-on-failure
```

## Next

- [Go-live checklist](go-live-checklist.md)
