# Run on a testnet or Binance Demo

Run a keyed session against a practice environment: a dry run first, then ten minutes with real orders and fake money. Prerequisites: a C++ build or the [release tarball](deploy.md), and an account on one of the environments below. Read [Kill switch and shutdown](kill-switch-and-shutdown.md) before a longer session.

## 1. Get the programs

The shipped configs trade built-in C++ strategies (`basic_mm`, and `options_mm` on Deribit) with `fastmm-live`. Build it, or take it from the release tarball, which also holds `configs/`:

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release -j"$(nproc)"
```

The binaries are in `build/release/bin/`. The pip package does not install them.

With `pip install "fastmm-engine[hot,live]"` alone, a Python strategy runs on the same environments: copy a config, set `[strategy] name = "py:<Class>"` and run `fastmm run <module>:<Class> --config <copy> --dry-run` ([Run a Python strategy live](../strategies/python-live.md)). `fastmm init` writes `live.toml` for Binance Spot Demo Mode.

## 2. Keys in the environment

Keys are read from the environment through `${VAR}` in `[venues.<name>]` ([Configuration](../../reference/configuration.md#general-rules)). Copy `.env.example` to `.env`, which git ignores, fill it in and load it:

```bash
cp .env.example .env        # then edit .env
set -a && . ./.env && set +a
```

| Environment | Config | Key variables |
|---|---|---|
| Binance Spot Demo Mode | `configs/binance-demo.toml` | `FASTMM_BINANCE_API_KEY`, `FASTMM_BINANCE_API_SECRET` |
| Binance USDⓈ-M Demo Trading | `configs/binance-usdm-demo.toml` | the same Binance variables |
| Binance Spot testnet | `configs/binance-testnet.toml` | the same Binance variables |
| Bybit testnet, spot | `configs/bybit-testnet.toml` | `FASTMM_BYBIT_API_KEY`, `FASTMM_BYBIT_API_SECRET` |
| Bybit testnet, linear perpetuals | `configs/bybit-linear-testnet.toml` | the same Bybit variables |
| OKX demo trading | `configs/okx-demo.toml`, `configs/okx-spot-demo.toml` | `FASTMM_OKX_API_KEY`, `FASTMM_OKX_API_SECRET`, `FASTMM_OKX_API_PASSPHRASE` |
| Coinbase Exchange sandbox | `configs/coinbase-sandbox.toml` | `FASTMM_COINBASE_API_KEY`, `FASTMM_COINBASE_API_SECRET`, `FASTMM_COINBASE_API_PASSPHRASE` |
| Deribit testnet | `configs/deribit-testnet.toml` | `FASTMM_DERIBIT_CLIENT_ID`, `FASTMM_DERIBIT_CLIENT_SECRET` |
| Gate futures TestNet | `configs/gate-usdt.toml` with testnet URLs | `FASTMM_GATE_API_KEY`, `FASTMM_GATE_API_SECRET` |
| Hyperliquid testnet | `configs/hyperliquid.toml` with `testnet = true` | `FASTMM_HL_ACCOUNT`, `FASTMM_HL_AGENT_KEY` |
| Lighter testnet | `configs/lighter.toml` with `testnet = true` | `FASTMM_LIGHTER_ACCOUNT`, `FASTMM_LIGHTER_API_KEY` |
| `xmm`: Spot Demo quotes, USDⓈ-M Demo hedges | `configs/xmm-binance-demo.toml` | the Binance variables |
| `xmm`: USDⓈ-M Demo quotes, Bybit testnet hedges | `configs/xmm-demo.toml` | the Binance and Bybit variables |

The two Binance Demo environments share one set of keys; every other environment has its own, and none accepts live-exchange keys. Since the Binance configs share variable names, load one set of Binance keys at a time. The `xmm` configs quote on the first venue and hedge every fill on the second ([Quote on one venue, hedge on another](../strategies/xmm.md#run-it)). Coinbase Advanced Trade has no practice environment ([Venue connectors](../../reference/venues/coinbase.md#advanced-trade)).

## 3. The environments

Each environment's endpoints are in its config. The shipped configs raise `stale_ms` for quiet feeds ([Venue connectors](../../reference/venues.md#configuration-keys)).

### Binance Spot Demo Mode

Switch to Demo Trading on binance.com and create a key under API Key Management.

| | |
|---|---|
| market data | follows the real market; quotes away from the touch rarely fill |
| fees | 10 bps maker and taker, as the Demo account charged; buys pay in BTC, sells in USDT ([Check PnL](journals-replay-pnl.md#check-pnl)) |
| config | `stale_ms = 10000`; `min_requote_ticks = 50` and `min_requote_interval_ms = 1000` keep the order rate under Binance's limits |

To use an Ed25519 key, which signs once at `session.logon` and allows SBE market data:

1. Make the key pair, and keep the private key out of the repository (`chmod 600`):

    ```bash
    openssl genpkey -algorithm ed25519 -out ed25519-private.pem
    openssl pkey -in ed25519-private.pem -pubout -out ed25519-public.pem
    ```

2. In API Key Management, create a "Self-generated" key, paste `ed25519-public.pem` and enable Spot trading, plus Futures for USDⓈ-M.
3. Export the key Binance shows, and the private key if you do not use the file:

    ```bash
    export FASTMM_BINANCE_API_KEY=<that key>
    export FASTMM_BINANCE_ED25519_PEM="$(cat ed25519-private.pem)"
    ```

4. In `[venues.binance]` set `key_type = "ed25519"` and `private_key_file = "/path/ed25519-private.pem"` or `private_key_env = "FASTMM_BINANCE_ED25519_PEM"`, and remove `api_secret`. `md_format = "sbe"` switches market data to SBE.
5. Dry-run first: the SBE channel goes live and the book syncs. In a keyed run, `session.logon failed` means the key, its permissions or the IP whitelist are wrong.

### Binance USDⓈ-M futures Demo Trading

Use the Demo Trading HMAC keys. The futures wallet needs USDT, and the account must be in one-way position mode; the connector refuses hedge mode.

| | |
|---|---|
| contract | BTCUSDT perpetual, tick 0.10, lot 0.0001, minimum notional 50 USDT |
| config | quotes 0.001 BTC about 1 bps from mid, `max_position = "0.003"`, `max_loss = "20"` |
| fees | 2 bps maker, 4 bps taker, in USDT |
| leverage, margin mode | account settings, shown in the start-up log, never changed by the connector |
| funding | booked from the income history ([Venues](../../reference/venues/binance-usdm.md#reconciliation-and-fills)) |

After a session, flatten any position with a reduce-only order.

### Binance Spot testnet

Generate keys on <https://testnet.binance.vision>; HMAC and Ed25519 both work. The market data is the testnet's own book, thin and often silent for seconds, so fills are rare and not representative.

### Bybit v5 testnet

Create a system-generated (HMAC) key on testnet.bybit.com, under API Management, with spot trading permission.

- The config sets `supports_replace = false`, so quotes are replaced with cancel and new: Bybit does not document whether an amend's `qty` includes the filled quantity.
- For linear perpetuals, `configs/bybit-linear-testnet.toml` sets `category = "linear"`. The key needs contract trading permission and USDT in the unified account. The symbol must be in one-way mode, or `fastmm-live` exits 3 ([Venue connectors](../../reference/venues/bybit.md#linear-perpetuals)).

### OKX demo trading

On okx.com switch to Demo trading and create a demo API key with trade permission. The passphrase you choose is the third credential, `api_passphrase`. The demo hosts refuse live keys (50101).

| | Swaps (`okx-demo.toml`) | Spot (`okx-spot-demo.toml`) |
|---|---|---|
| account | net position mode, USDT in the trading account; other modes exit 3 | any mode |
| quantities | contracts; one BTC-USDT-SWAP contract is 0.01 BTC, the config quotes 0.1 | BTC, the minimum 0.00002 |
| hosts | `wspap.okx.com:8443`, REST `www.okx.com` with `x-simulated-trading: 1` | the EEA site: `eea.okx.com`, `wseeapap.okx.com:8443` (`region = "eea"`) |

Keys work only on the domain of the site they were made on. The connector refuses a demo host with `testnet = false`, and the reverse. The swap config sets `supports_replace = false` and `dead_mans_switch_s = 60`. Demo instrument data, tick 0.01 and `instIdCode`, differs from production ([Venue connectors](../../reference/venues/okx.md)).

### Coinbase Exchange sandbox

Sign up at <https://public.sandbox.exchange.coinbase.com>, an account of its own, and create an API key with View and Trade. The sandbox shows the secret once and asks for a passphrase. Add fake funds to the profile there.

The sandbox lists fewer products than production, and its thin books do not follow production prices, so the config quotes 50 bps from the mid. The connector refuses a sandbox host with `testnet = false`, and the reverse ([Venue connectors](../../reference/venues/coinbase.md#exchange)).

### Deribit testnet

Create an account on <https://test.deribit.com> and an API key under Account, API. Its client id and secret go into `FASTMM_DERIBIT_CLIENT_ID` and `FASTMM_DERIBIT_CLIENT_SECRET`.

- Instruments expire. The config quotes `BTC-25DEC26-*` options with `options_mm`; after that date reference data fails to load (exit 4). Replace the `[[instruments]]` with names from `public/get_instruments?currency=BTC&kind=option`.
- `stale_ms = 15000` and `dead_ms = 35000` sit above the 10 s heartbeat.
- Set `matching_engine_rate` and `matching_engine_burst` to your account tier.
- Option prices are in BTC, so the `[risk]` notional limits are BTC amounts.

### Gate USDT perpetual futures

`configs/gate-usdt.toml` points at production. For the futures TestNet, set `wss://ws-testnet.gate.com/v4/ws/futures/usdt` and `https://fx-api-testnet.gateio.ws`, with keys from the "Futures TestNet APIKeys" tab. The account must be in single position mode with USDT in its futures account ([Venue connectors](../../reference/venues/gate.md)).

### Hyperliquid perpetuals

The keys are the account's address (`api_key`) and a signing key (`api_secret`). Use an agent wallet: on app.hyperliquid.xyz, More > API, Generate, then Authorize API Wallet. An agent can trade but cannot move funds; the connector checks it against the account's `extraAgents` at start-up.

| | |
|---|---|
| testnet | `wss://api.hyperliquid-testnet.xyz/ws`, `https://api.hyperliquid-testnet.xyz`, `testnet = true`; USDC from the faucet; a mainnet agent is not valid there |
| collateral | in a unified account, spot USDC backs every dex; otherwise move USDC to the perp balance |
| HIP-3 dexes | named with their prefix, `xyz:NVDA` |
| request budget | each order and modify spends one request of 10000 plus one per USDC traded; the account can buy more with `reserveRequestWeight`; the connector stops at `address_reserve` |
| dead man's switch | `scheduleCancel` is refused until the address has traded enough; the connector logs it once and runs without |

After a session, run `fastmm-ctl flatten` and check `openOrders` and `clearinghouseState` of each dex.

### Lighter perpetuals

1. Connect a wallet at app.lighter.xyz and deposit USDC; the first deposit creates the account. On the testnet the faucet funds it.
2. Register an API key in a slot (4 to 254) on the trading host. The wallet's key comes from the environment; without `--send` the tool only prints the account, the slot and the new public key:

    ```bash
    pip install lighter-sdk eth-account
    LIGHTER_ETH_PRIVATE_KEY=0x... python3 tools/lighter_api_key.py --network testnet --slot 4 --send
    ```

3. Keep the printed private key as `FASTMM_LIGHTER_API_KEY`.

| Tier | Latency | Rate | Cost |
|---|---|---|---|
| Standard | orders and cancels wait 300 ms | 60 transactions a minute | free |
| Premium | no wait for makers and cancels | 4000 a minute | each order spends volume quota; the connector stops at `quota_reserve` |

Switch tier with `/api/v1/changeAccountTier`, once a day per address, for every sub-account. After a session, run `fastmm-ctl flatten` and check `accountActiveOrders` and the positions of `account`.

## 4. Dry run

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml --dry-run --duration 60s
```

A dry run needs no keys, opens market data only and sends no order. The strategy quotes as it would; each order is acknowledged at once on paper and none fills. Every 5 s the log shows what would rest:

```text
[binance] dry run BTCUSDT: bid 83985.83 x 0.0003 ask 84238.18 x 0.0003 would send new=2 cancel=0 replace=6
```

At the end: `dry run: would have sent 2 orders, 2 cancels, 6 replaces (none was sent)`. A working dry run's 10 s status line shows `md=live`, `books=1/1`, `malformed=0` and `dropped=0`.

The dry run starts flat and runs as engine `<name>-dryrun` (`fastmm-top --name <name>-dryrun`). It writes no store, kill file or strategy state, so the next keyed session continues from the last keyed one.

## 5. Short keyed run

1. Start with the shipped `[risk]` limits, a few hundred USDT of exposure, and a fixed duration, with the journal and log in one directory:

    ```bash
    mkdir -p runs/demo-1
    ./build/release/bin/fastmm-live --config configs/binance-demo.toml --duration 10m \
      --journal runs/demo-1/session.fmj --log runs/demo-1/engine.log
    ```

2. Watch it from a second terminal; the name is `[engine] name`:

    ```bash
    ./build/release/bin/fastmm-top --name binance-demo
    ```

    A Demo session one second after connecting logs:

    ```text
    [binance] md=live user=live order=live books=1/1 md_msgs=4 resyncs=0 malformed=0 dropped=0 orders=2 cancels=0 order_events=4 rest=2/0err reconnects=0 clock_offset_ms=175 rate_refused(weight/10s/1m/1d/paused)=0/0/0/0/0
    ```

    `clock_offset_ms` is the venue clock minus the local one; signed requests fail as it nears `recv_window_ms` ([Troubleshooting](troubleshooting.md#clock)).

## 6. Stop and check

1. Press Ctrl-C, or let `--duration` elapse.
2. Read the last log lines ([Reading the last lines](kill-switch-and-shutdown.md#reading-the-last-lines)).
3. Confirm the venue shows no open orders ([Go-live checklist](go-live-checklist.md#stopping)).
4. Reconcile the journal against the account ([Check PnL](journals-replay-pnl.md#check-pnl)).

## 7. Opt-in live connector tests

The tests labelled `live` run each connector against its practice environment: book sync, then a far post-only order placed and cancelled. They skip unless `FASTMM_LIVE_TESTS=1`, and the order steps need the key variables above.

```bash
cmake --build --preset release -j --target fastmm_venues_tests
FASTMM_LIVE_TESTS=1 FASTMM_BINANCE_ENV=demo ctest --test-dir build/release -L live --output-on-failure
# one venue only
FASTMM_LIVE_TESTS=1 ctest --test-dir build/release -L live -R 'venues\.live\.bybit' --output-on-failure
```

| Test file | Runs against |
|---|---|
| `tests/venues/live_binance_test.cpp` | Binance Spot testnet, or Demo Mode with `FASTMM_BINANCE_ENV=demo` |
| `live_binance_usdm_test.cpp` | Binance USDⓈ-M Demo |
| `live_bybit_test.cpp` | Bybit testnet |
| `live_deribit_test.cpp` | Deribit testnet |
| `live_coinbase_test.cpp` | Coinbase Advanced Trade production, read only, with `COINBASE_API_KEY_NAME` and `COINBASE_API_PRIVATE_KEY` |

The test presets exclude the `live` label.

Next: the [Go-live checklist](go-live-checklist.md).
