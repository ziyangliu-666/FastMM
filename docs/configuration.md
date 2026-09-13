# Configuration

Every FastMM binary reads one TOML file passed with `--config path.toml`. The parser lives in
`src/core/config.cpp`; this page describes what it actually accepts.

## General rules

- **Unknown keys are ignored with a warning** that names the key and its line. Check the warnings
  when a setting seems to have no effect.
- **A key with the wrong type is an error**, reported with its line and column.
- **Prices, quantities and notionals are exact.** Keys such as `tick`, `lot`, `max_order_qty` or
  `max_loss` may be written as strings (`"0.01"`) or numbers (`0.01`). Either way the decimal text
  is kept and parsed exactly into fixed point, never through `double`.
- **Secrets come from the environment.** Inside `[venues.<name>]`, the string keys `kind`, `ws_url`,
  `ws_api_url`, `rest_url`, `api_key`, `api_secret` and `ca_file` support `${NAME}` substitution.
  Only exact `${NAME}` tokens are replaced. If a referenced variable is not set, loading fails; it is
  never silently empty.
- **Literal secrets are refused.** A value longer than 32 characters under a key whose name contains
  `key`, `secret`, `token` or `password` is rejected unless it uses `${...}`. Pass
  `--allow-inline-secrets` to override, for throwaway local testing only.
- `Config::redacted()` is what gets logged and written into journals, so credentials never reach disk.

## `[engine]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `"fastmm"` | Session name used in logs and journal headers |
| `cpu` | int | `-1` | Core to pin the engine thread to; `-1` disables pinning |
| `net_cpus` | int array | `[]` | Cores for the network threads, one per venue in order |
| `spin_mode` | string | `"adaptive"` | `busy` spins forever; `adaptive` backs off to short sleeps when idle (use on WSL2 and laptops) |
| `journal` | bool | `true` | Record every inbound event to a `.fmj` journal |
| `journal_dir` | string | `"runs"` | Directory for journals |
| `epoch_file` | string | `"runs/session_epoch"` | Persisted session epoch, which keeps client order ids unique across restarts |
| `rng_seed` | int | `1` | Seed for the strategy random generator, so runs are reproducible |
| `md_ring_bytes` | int | `4194304` | Market-data ring size per venue, power of two |
| `order_ring_bytes` | int | `1048576` | Outbound order ring size per venue, power of two |
| `journal_ring_bytes` | int | `16777216` | Engine-to-journal ring size, power of two |
| `max_events_per_step` | int | `64` | Events processed per engine step before timers are checked |
| `crossed_grace_ms` | int | `100` | How long a crossed book is tolerated before quotes are pulled |
| `latency_publish_ms` | int | `1000` | How often latency histograms are published |
| `min_requote_ticks` | int | `1` | Keep a resting quote whose price is within this many ticks of the desired price |
| `min_requote_interval_ms` | int | `50` | Never change the same quote slot more often than this |
| `min_qty_bps` | int | `8000` | Keep a resting quote whose remaining quantity covers this share of the desired quantity, in bps (8000 = 80%) |
| `post_only` | bool | `true` | Send quotes as post-only (maker only) |
| `supports_replace` | bool | `true` | Allow the quote manager to amend orders in place; it is still disabled for any venue that does not support replace |

## `[venues.<name>]`

One table per venue; `<name>` is how instruments refer to it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `kind` | string | | Connector type |
| `rest_url` | string | | REST base URL |
| `ws_url` | string | | Market-data WebSocket URL |
| `ws_api_url` | string | | WebSocket order-entry URL, if the venue has one |
| `api_key` | string | | Use `"${FASTMM_<VENUE>_API_KEY}"` |
| `api_secret` | string | | Use `"${FASTMM_<VENUE>_API_SECRET}"` |
| `testnet` | bool | `true` | Guard against accidentally pointing a config at production |
| `supports_replace` | bool | `false` | Whether the venue can amend an order in place |
| `ca_file` | string | | Extra CA certificate, e.g. `tests/fixtures/tls/cert.pem` for the local sim over TLS |
| `insecure_tls` | bool | `false` | Skip certificate verification; local testing only |
| `recv_window_ms` | int | `3000` | Signed-request validity window |
| `fees.maker_bps`, `fees.taker_bps` | number | `0` | Fee model used for PnL; negative means rebate |

Any other key in a venue table is passed through to that connector (and produces an "unknown key"
warning from the generic parser). Connector-specific keys are listed in `docs/venues.md`.

## `[[instruments]]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `venue` | string | required | Must name a configured venue, otherwise loading fails |
| `symbol` | string | required | Venue symbol, e.g. `BTCUSDT` |
| `base`, `quote` | string | | Currencies |
| `asset_class` | string | `"spot"` | `spot`, `perpetual` (`perp`), `future` (`futures`), `option`, `fx`, `equity` |
| `tick` | decimal | | Price increment |
| `lot` | decimal | | Quantity increment |
| `min_qty`, `max_qty` | decimal | | Order size bounds |
| `min_notional` | decimal | | Smallest order value the venue accepts |
| `contract_multiplier` | decimal | `"1"` | Futures and options multiplier; PnL and notional use it |
| `enabled` | bool | `true` | Load the instrument but do not trade it when `false` |
| `price_decimals` | int | `8` | Display precision |
| `expiry`, `strike`, `option_type` | string / decimal / string | | Derivatives reference data |

Live connectors fill `tick`, `lot` and the size bounds from the venue's reference data when they
connect; values given here are used by backtests and the simulator.

## `[strategy]`

```toml
[strategy]
name = "basic_mm"            # a registered strategy

[strategy.params]            # scalars only; validated against the strategy's parameter schema
half_spread_bps = 5
skew_bps_per_unit = 1
quote_qty = 0.001
max_inventory = 0.01
```

Parameter names, defaults and bounds come from the strategy's `FASTMM_PARAM` declarations, for
example `include/fastmm/strategies/basic_mm.hpp`. An unknown parameter or an out-of-range value is an
error at startup.

## `[risk]`

Every limit is off when it is `0` or omitted.

| Key | Type | Meaning |
|---|---|---|
| `max_order_qty` | decimal | Largest single order |
| `max_order_notional` | decimal | Largest single order value, quote currency |
| `max_position` | decimal | Largest absolute position per instrument, counting same-side open orders |
| `max_open_orders` | int | Open orders per instrument |
| `price_collar_bps` | int | Reject limit prices further than this from mid |
| `fat_finger_bps` | int | Reject limit prices further than this from the last trade |
| `stale_md_ms` | int | Reject orders when the book is older than this |
| `max_loss` | decimal | Positive number. The kill switch trips when net PnL falls to `-max_loss` or below |
| `orders_per_sec` | int | Token-bucket order rate |
| `burst` | int | Token-bucket capacity; defaults to `orders_per_sec` |
| `stp` | bool | Self-trade prevention against our own resting orders; default `true` |

Cancels are always allowed, including after the kill switch trips.

## `[logging]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `level` | string | `"info"` | `trace`, `debug`, `info`, `warn` (or `warning`), `error`, `off` |
| `file` | string | | Log file; empty logs to stderr only |
| `mirror_level` | string | `"warn"` | Records at or above this level are also written to stderr |

## `[backtest]`

Read by `fastmm-backtest`, `fastmm-replay`, the tests and the Python module
(`src/backtest/backtest_config.cpp`). The engine, risk and strategy settings come from the sections
above; fees come from the first venue's `fees` table, self-trade prevention follows `[risk] stp`,
and in-place replace follows `[engine] supports_replace`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `source` | string | `""` | `synthetic`, `journal` or `csv`. When empty, the format is inferred from `path` |
| `path` | string | `""` | Data file. `.fmj` is a journal, `.csv` is CSV; empty means synthetic data |
| `seed` | int | `[sim] seed`, else `1` | Seed for the simulator and the strategy random generator |
| `duration_s` | int | `[sim] duration_s`, else `60` | Simulated horizon for synthetic data; must be positive |
| `fill_model` | string | `"matching"` | `matching` matches our orders against the simulated order flow. `l2_queue` estimates queue position on recorded L2 data, which has no counterparties |
| `queue_conservatism` | number | `1.0` | For `l2_queue`, from 0 to 1: at `0` cancellations ahead of us always move our order up the queue, at `1` they never do |
| `latency_fixed_us` | int | `200` | Fixed latency for orders to the venue and acknowledgements back |
| `latency_jitter_us` | int | `50` | Random jitter added to that latency, seeded |
| `latency_md_us` | int | `0` | Fixed market-data latency |
| `latency_md_jitter_us` | int | `0` | Market-data latency jitter |
| `p_drop` | number | `0.0` | Probability, below 1, that an outbound order message is lost; exercises reconciliation |
| `equity_bar_s` | int | `1` | Bar length for the equity curve and the Sharpe ratio |
| `initial_capital` | number | `0` | Starting capital, used for percentage drawdown |
| `output_dir` | string | `"runs/backtest"` | Where `equity.csv`, `fills.csv`, `orders.csv` and `summary.json` are written |
| `journal_out` | string | `""` | When set, the backtest session is also recorded as a `.fmj` journal |

Command-line flags of `fastmm-backtest` (`--data`, `--strategy`, `--param key=value`, `--seed`,
`--out`) override these values; run it with `--help` for the full list.

## `[sim]`

Parameters of the synthetic market used when the data source is synthetic. Prices and sizes use the
first instrument's `tick` and `lot`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `seed` | int | `1` | Used when `[backtest] seed` is not set |
| `duration_s` | int | `60` | Used when `[backtest] duration_s` is not set |
| `start_mid` | decimal | `"60000"` | Initial mid price |
| `seed_levels` | int | `20` | Price levels populated before the run starts |
| `limit_rate_per_s` | number | `200` | Limit-order arrivals per second |
| `cancel_rate_per_order_s` | number | `0.5` | Cancellation hazard of each resting order, per second |
| `market_rate_per_s` | number | `10` | Market-order arrivals per second |
| `mid_step_rate_per_s` | number | `2` | Steps of one tick in the latent mid price, per second |
| `offset_p` | number | `0.35` | In (0, 1]. Limit orders land k ticks from the touch with probability p(1-p)^k |
| `base_spread_ticks` | int | `1` | Distance of the touch from the latent mid, at least 1 |
| `limit_qty_median_lots` | number | `200` | Median limit-order size in lots (log-normal) |
| `market_qty_median_lots` | number | `100` | Median market-order size in lots (log-normal) |
| `regimes` | bool | `true` | Switch between a calm and a volatile regime |
| `volatile_mult` | number | `4.0` | Multiplier on mid steps and market orders in the volatile regime |
| `depth_update_ms` | int | `100` | Book changes are aggregated into one depth diff per interval, like Binance `@depth@100ms` |
| `book_ticker` | bool | `true` | Also publish top-of-book updates |

`configs/backtest-example.toml` is a complete, tuned example of both sections.

## Failure handling

| Failure | Detection | Action |
|---|---|---|
| Market-data disconnect | EPOLLRDHUP, read of 0 bytes, TLS error | Quotes pulled, reconnect with backoff, fresh snapshot |
| Sequence gap | Book sync state machine | Resync state, re-snapshot (rate limited), quotes pulled meanwhile |
| Stale feed | No traffic for the connector's stale threshold | Stale event; a longer dead threshold forces reconnect |
| Order channel loss | Connection state machine | Immediate cancel-all through REST, reconcile open orders after reconnect |
| Kill switch | Risk engine or SIGINT | Cancel-all on every venue, stop quoting, exit after acks or 5 s |
| Rate limit | Response headers and error codes | Cool down until reset; HTTP 418 halts REST for the ban period |
| Clock skew | Timestamp rejection codes | Re-measure offset from the venue's time endpoint |
| Ring overflow | Push fails | Drop the market-data delta and resync; order events are never dropped |
