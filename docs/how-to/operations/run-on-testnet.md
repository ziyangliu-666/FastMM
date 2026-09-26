# Run on a testnet or Binance Demo

FastMM ships configs for seven practice environments: Binance Spot Demo Mode, Binance USDⓈ-M futures Demo Trading, the Binance Spot testnet, the Bybit v5 testnet (spot and linear perpetuals), OKX demo trading and the Deribit testnet. The two Binance Demo environments share one set of keys; each other environment has its own. None of them accepts live-exchange keys.

Before a longer session, read [Kill switch and shutdown](kill-switch-and-shutdown.md).

## 1. Build

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release -j"$(nproc)"
```

The binaries are in `build/release/bin/`.

## 2. Keys in the environment

Keys are read from the environment through `${VAR}` references in `[venues.<name>]` ([Configuration](../../reference/configuration.md#general-rules)). Copy `.env.example` to `.env` (it is ignored by git), fill in the variables and load it:

```bash
cp .env.example .env        # then edit .env
set -a && . ./.env && set +a
```

| Environment | Config | Key variables |
|---|---|---|
| Binance Demo Mode | `configs/binance-demo.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Binance USDⓈ-M Demo Trading | `configs/binance-usdm-demo.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Binance testnet | `configs/binance-testnet.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Bybit testnet | `configs/bybit-testnet.toml` | `FASTMM_BYBIT_API_KEY`, `FASTMM_BYBIT_API_SECRET` |
| Bybit testnet, linear perpetuals | `configs/bybit-linear-testnet.toml` | `FASTMM_BYBIT_API_KEY`, `FASTMM_BYBIT_API_SECRET` |
| OKX demo trading | `configs/okx-demo.toml` | `FASTMM_OKX_API_KEY`, `FASTMM_OKX_API_SECRET`, `FASTMM_OKX_API_PASSPHRASE` |
| Deribit testnet | `configs/deribit-testnet.toml` | `FASTMM_DERIBIT_CLIENT_ID`, `FASTMM_DERIBIT_CLIENT_SECRET` |

The Binance configs share variable names, so one set of Binance keys is loaded at a time.

## 3. The environments

The shipped configs raise `stale_ms` for quiet feeds ([Venue connectors](../../reference/venues.md#configuration-keys)).

### Binance Spot Demo Mode

- Keys: Binance Demo Trading. Switch to Demo Trading on binance.com and create a key under API Key Management.
- Endpoints (from `configs/binance-demo.toml`): streams `wss://demo-stream.binance.com/stream`, WebSocket API `wss://demo-ws-api.binance.com/ws-api/v3`, REST `https://demo-api.binance.com`.
- Market data follows the real market; quotes away from the touch rarely fill.
- Fees: the config books 10 bps maker and taker (`[venues.binance.fees]`), what the Demo account charged.
- Commission asset: buys are charged in the base asset (BTC) and sells in the quote asset (USDT) ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)).
- Config: `stale_ms = 10000`; `[engine] min_requote_ticks = 50` and `min_requote_interval_ms = 1000` keep the order rate below Binance's limits.
- Ed25519 key (unsigned orders after `session.logon`, and SBE market data):
  1. `openssl genpkey -algorithm ed25519 -out ed25519-private.pem` and `openssl pkey -in ed25519-private.pem -pubout -out ed25519-public.pem`; keep the private key out of the repository (`chmod 600`).
  2. In Demo Trading, API Key Management: create an API key of type "Self-generated" (Ed25519), paste the contents of `ed25519-public.pem`, enable Spot trading (and Futures for USDⓈ-M). Binance shows the API key.
  3. Export `FASTMM_BINANCE_API_KEY=<that key>` and either keep the PEM file or export it (`export FASTMM_BINANCE_ED25519_PEM="$(cat ed25519-private.pem)"`).
  4. In `[venues.binance]`: `key_type = "ed25519"`, `private_key_file = "/path/ed25519-private.pem"` or `private_key_env = "FASTMM_BINANCE_ED25519_PEM"`, remove `api_secret`, and optionally `md_format = "sbe"` (derives `wss://demo-stream-sbe.binance.com/stream`).
  5. `fastmm-live --config configs/binance-demo.toml --dry-run` first: the SBE market-data channel goes Live and the book syncs. In a live run the order channel goes Live only after the `session.logon` reply; `session.logon failed` in the log means the key, its permissions or the IP whitelist are wrong.

### Binance USDⓈ-M futures Demo Trading

- Keys: the Binance Demo Trading HMAC keys, the same as for Spot Demo Mode. The USDⓈ-M futures wallet needs a USDT balance, and the account must be in one-way position mode (the connector refuses hedge mode).
- Endpoints (from `configs/binance-usdm-demo.toml`): streams `wss://demo-fstream.binance.com` (`/public`, `/market`, `/private`), WebSocket API `wss://testnet.binancefuture.com/ws-fapi/v1`, REST `https://demo-fapi.binance.com`.
- Contract: the BTCUSDT perpetual, tick 0.10, lot 0.0001, minimum notional 50 USDT. The config quotes 0.001 BTC about 1 bps from mid with `max_position = "0.003"` and `max_loss = "20"`.
- Fees: 2 bps maker and 4 bps taker, charged in USDT (`GET /fapi/v1/commissionRate` on the Demo account).
- Leverage and margin mode are account settings: the log shows them at startup; the connector does not change them. Funding payments are booked from the income history ([Venues](../../reference/venues.md#positions-and-reconciliation)).
- After a session, flatten any remaining position with a reduce-only order.

### Binance Spot testnet

- Keys: generate them on <https://testnet.binance.vision> (HMAC keys; Ed25519 keys work with `key_type = "ed25519"` and `private_key_file`).
- Endpoints (from `configs/binance-testnet.toml`): streams `wss://stream.testnet.binance.vision/stream`, WebSocket API `wss://ws-api.testnet.binance.vision/ws-api/v3`, REST `https://testnet.binance.vision`.
- Market data is the testnet's own book: thin and often silent for several seconds (`stale_ms = 10000`). Fills are rare and not representative.

### Bybit v5 spot testnet

- Keys: create a system-generated (HMAC) API key on the Bybit testnet site (testnet.bybit.com, API Management) with spot trading permission.
- Endpoints (from `configs/bybit-testnet.toml`): public `wss://stream-testnet.bybit.com/v5/public/spot`, trade `wss://stream-testnet.bybit.com/v5/trade`, private `wss://stream-testnet.bybit.com/v5/private` (derived from `ws_url` when `ws_private_url` is not set), REST `https://api-testnet.bybit.com`.
- Config: `supports_replace = false`: quotes are replaced with cancel and new, because Bybit does not document whether amend `qty` includes the filled quantity. `recv_window_ms = 5000`, `stale_ms = 10000`.
- Linear perpetuals: `configs/bybit-linear-testnet.toml` sets `category = "linear"` and the public stream `wss://stream-testnet.bybit.com/v5/public/linear`. The key needs contract trading permission and USDT in the unified account. The symbol must be in one-way position mode: in hedge mode `fastmm-live` exits 3 at start-up. Not run against the testnet yet ([Venue connectors](../../reference/venues.md#linear-perpetuals)).

### OKX demo trading

- Keys: on okx.com switch to Demo trading and create a demo API key (Personal center, Demo trading API) with trade permission. OKX asks for a passphrase when the key is made: it is the third credential, `api_passphrase`. Live-trading keys are refused by the demo hosts (50101).
- Account: net position mode, not the spot account mode, USDT in the trading account. Long/short mode or the spot mode make `fastmm-live` exit 3 at start-up.
- Endpoints (from `configs/okx-demo.toml`): public `wss://wspap.okx.com:8443/ws/v5/public`, private and orders `wss://wspap.okx.com:8443/ws/v5/private` (derived), REST `https://www.okx.com` with `x-simulated-trading: 1` (`testnet = true`). Production is `wss://ws.okx.com:8443` with `testnet = false`; the connector refuses a demo host with `testnet = false` and the reverse.
- Units: quantities are contracts. One BTC-USDT-SWAP contract is 0.01 BTC; the config quotes 0.1 contracts (0.001 BTC). On demo the tick is 0.01 and `instIdCode` differs from production; both come from `GET /api/v5/public/instruments` with the demo header.
- Config: `supports_replace = false` until amend has been seen on the demo; `dead_mans_switch_s = 60` arms `cancel-all-after` (whether demo trading honours it is not documented).
- Status: the public stream was run against the demo and production hosts (book synced, no resync); orders, the private channels and REST account calls are covered by a scripted fake exchange only ([Venue connectors](../../reference/venues.md#okx-v5-usdt-margined-swaps)).

### Deribit testnet

- Keys: create an account on <https://test.deribit.com> and an API key under Account, API. The key's client id and client secret go into `FASTMM_DERIBIT_CLIENT_ID` and `FASTMM_DERIBIT_CLIENT_SECRET` (`api_key` and `api_secret` in the config).
- Endpoints (from `configs/deribit-testnet.toml`): WebSocket `wss://test.deribit.com/ws/api/v2` for market data and, on a second connection, authentication and orders; REST `https://test.deribit.com/api/v2` for reference data and the kill-switch `private/cancel_all_by_instrument`.
- Instruments expire. The shipped config quotes `BTC-25DEC26-*` options with `options_mm`; after that date reference data fails to load (exit 4). Replace the `[[instruments]]` with live names from `public/get_instruments?currency=BTC&kind=option`.
- Config: `stale_ms = 15000` and `dead_ms = 35000`, above the heartbeat interval (`heartbeat_interval_s = 10`, the minimum). `matching_engine_rate` and `matching_engine_burst` rate-limit order requests locally; set them to your account tier. Option prices are in BTC, so the `[risk]` notional limits are BTC amounts.
- Status: the private payloads follow Deribit's published schemas and a scripted fake exchange, not recorded traffic ([Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code)).

## 4. Dry run

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml --dry-run --duration 60s
```

`--dry-run` needs no keys, opens market data only and sends no order. The log prints one status line per venue every second; a working dry run shows `md=live`, all books synced (`books=1/1`), `malformed=0` and `dropped=0`.

## 5. Short keyed run

Start with the shipped `[risk]` limits (a few hundred USDT of exposure) and a fixed duration, with the journal and log in one directory per session:

```bash
mkdir -p runs/demo-1
./build/release/bin/fastmm-live --config configs/binance-demo.toml --duration 10m \
  --journal runs/demo-1/session.fmj --log runs/demo-1/engine.log
```

Watch it from a second terminal (the name is `[engine] name`):

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

The `live.*` test cases in `tests/venues/live_binance_test.cpp`, `live_binance_usdm_test.cpp`, `live_bybit_test.cpp` and `live_deribit_test.cpp` run each connector against its testnet or Demo environment: book sync, then a far post-only order placed and cancelled. They carry the ctest label `live`, which the test presets exclude; a plain `ctest` runs them, but they skip unless `FASTMM_LIVE_TESTS=1` is set, and the order steps need the key variables above. `FASTMM_BINANCE_ENV=demo` points the Binance test at Demo Mode instead of the testnet.

```bash
cmake --build --preset release -j --target fastmm_venues_tests
FASTMM_LIVE_TESTS=1 FASTMM_BINANCE_ENV=demo ctest --test-dir build/release -L live --output-on-failure
# one venue only
FASTMM_LIVE_TESTS=1 ctest --test-dir build/release -L live -R 'venues\.live\.bybit' --output-on-failure
```

## Next

- [Go-live checklist](go-live-checklist.md)
