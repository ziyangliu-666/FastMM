# Configuration

<!-- The key tables are generated from include/fastmm/config/schema.hpp by tools/docs_config_ref.py
(docs/contributing/writing-docs.md#generated-pages); the parser is src/core/config.cpp. -->

Every FastMM program reads one TOML file passed with `--config <file.toml>`. Examples: `configs/`.

## General rules

- Unknown keys are ignored with a warning that names the key and its line.
- A key with the wrong type is an error, reported with its line and column.
- Decimal keys (`tick`, `lot`, `max_order_qty`, `max_loss`, ...) accept `"0.01"` or `0.01`; both are parsed as decimal text into fixed point, not through a `double`.
- Inside `[venues.<name>]`, the string keys `kind`, `ws_url`, `ws_api_url`, `rest_url`, `api_key`, `api_secret` and `ca_file` support `${NAME}` substitution. Only exact `${NAME}` tokens are replaced, and a variable that is not set is an error. `fastmm-live --dry-run` drops unset `api_key` and `api_secret` variables instead.
- Under `[venues.<name>]`, a literal value longer than 32 characters under a key whose name contains `key`, `secret`, `token` or `password` is refused with `venues.<name>.<key> looks like an inline secret; use ${ENV_VAR} or --allow-inline-secrets`. `fastmm-live --allow-inline-secrets` turns this check off.
- Logs and journals contain the configuration without `api_key` and `api_secret`.
- Types: `decimal` values are marked in the meaning; `any` keys accept a string or a number.

## `[engine]`

<!-- BEGIN config-keys engine -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `name` | string |  | session name, used in logs, journal file names and the status file (default "fastmm") |
| `cpu` | integer |  | CPU core of the engine thread; -1 = not pinned (default -1) |
| `net_cpus` | integer array |  | CPU cores of the network threads, one per venue in order (default []) |
| `spin_mode` | string |  | busy (spin forever) \| adaptive (back off to short sleeps when idle; use on WSL2 and laptops) (default adaptive) |
| `net_backend` | string |  | event loop of fastmm-live and fastmm-sim-exchange: epoll \| io_uring (Linux 5.13 or newer; falls back to epoll with a warning) (default epoll) |
| `threading` | string |  | fastmm-live: split (engine and network threads, rings between them) \| single (one venue; its network loop, the engine and order sending run on the engine thread, cpu; net_cpus is ignored) (default split) |
| `journal` | boolean |  | record every consumed event to a .fmj journal (default true) |
| `journal_dir` | string |  | directory for journals (default "runs") |
| `journal_sync` | string |  | how far a journal write is pushed: async (msync every 100 ms; survives the process dying) \| fdatasync (msync + fdatasync on the same tick; survives power loss) (default async) |
| `journal_max_bytes` | integer |  | roll the journal over to the next part at this size, bytes; each part is a complete journal ("x.fmj", "x.1.fmj", ...); 0 = one file per session (default 0) |
| `journal_retention_days` | integer |  | delete .fmj files in journal_dir last written more than this many days ago, at start; 0 = keep everything (default 0) |
| `epoch_file` | string |  | session epoch file, keeps client order ids unique across restarts (default "runs/session_epoch") |
| `kill_file` | string |  | latched kill switch and cumulative PnL, so [risk] max_loss is a budget across restarts (default "<journal_dir>/<name>.kill") |
| `ack_timeout_ms` | integer |  | force-cancel an order whose ack has not arrived within this long, ms; 0 = off (default 0) |
| `rng_seed` | integer |  | seed of the strategy random generator ctx.rng() (default 1) |
| `md_ring_bytes` | integer |  | market-data ring per venue, bytes, a power of two (default 4194304) |
| `order_ring_bytes` | integer |  | order-event ring per venue, bytes, a power of two (default 1048576) |
| `journal_ring_bytes` | integer |  | engine-to-journal ring, bytes, a power of two (default 16777216) |
| `max_events_per_step` | integer |  | events taken from each ring per engine iteration (default 64) |
| `crossed_grace_ms` | integer |  | tolerate a crossed book this long before pulling its quotes, ms (default 100) |
| `latency_publish_ms` | integer |  | latency histogram publish interval, ms (default 1000) |
| `tsc_recalibrate_s` | integer |  | fastmm-live: TSC recalibration period, s; 0 = off (default 10) |
| `timer_slack_ns` | integer |  | fastmm-live: timer slack of its threads, ns; how late a sleep may end (adaptive spin_mode sleeps 50 us when idle); 0 = the kernel's, 50000 (default 0) |
| `lock_memory` | boolean |  | fastmm-live: mlockall() the process, so no page is swapped out or faulted in on the hot path; needs ulimit -l above the process size, a warning otherwise (default false) |
| `min_requote_ticks` | integer |  | keep a resting quote whose price is within this many ticks of the desired price (default 1) |
| `min_requote_interval_ms` | integer |  | change the same quote slot at most this often, ms (default 50) |
| `min_qty_bps` | integer |  | keep a resting quote whose remaining quantity covers this share of the desired quantity, bps (default 8000 = 80 %) |
| `post_only` | boolean |  | send quotes as post-only orders (default true) |
| `supports_replace` | boolean |  | let the quote manager amend orders in place where the venue supports it (default true) |
| `reject_backoff_ms` | integer |  | after a venue reject other than a post-only cross, no new orders on that side for this long, doubling with each further reject, ms; 0 = off (default 1000) |
| `reject_backoff_max_ms` | integer |  | cap of the doubling reject backoff, ms (default 60000) |
| `on_kill` | string |  | fastmm-live after a kill switch the engine trips itself ([risk] max_loss, a full ring, every venue killed): exit (normal shutdown, exit code 6) \| stay (keep running with quoting off) (default exit) |
<!-- END config-keys -->

`io_uring` is blocked by some seccomp profiles, including Docker's default ([Network reactor](../explanation/architecture.md#network-reactor)). `tsc_recalibrate_s`: the engine clock stays continuous and slews each measured offset away over one period (at most 500 ppm); it steps, with a warning, only when it was more than 1 ms off. `on_kill`: [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself).

## `[venues.<name>]`

One table per venue; `<name>` is how instruments refer to it.

<!-- BEGIN config-keys venues.* -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `kind` | string | yes | connector: binance_spot (alias binance) \| binance_usdm \| bybit (alias bybit_spot) \| deribit \| nasdaq_itch \| sim (fastmm-sim-exchange and backtest configs) |
| `ws_url` | string |  | market-data WebSocket URL |
| `ws_api_url` | string |  | order-entry WebSocket API URL, where the venue has one |
| `rest_url` | string |  | REST base URL |
| `api_key` | string |  | API key, written as "${VARIABLE}" |
| `api_secret` | string |  | API secret, written as "${VARIABLE}" |
| `testnet` | boolean |  | the endpoints are a testnet or demo environment (default true) |
| `supports_replace` | boolean |  | the venue can amend an order in place (default false) |
| `insecure_tls` | boolean |  | skip TLS certificate verification; local simulator only (default false) |
| `ca_file` | string |  | extra CA certificate, for example tests/fixtures/tls/cert.pem for the local simulator |
| `recv_window_ms` | integer |  | validity window of signed requests, ms (default 3000) |
| `fees` | table |  | [venues.<name>.fees] table: maker_bps and taker_bps, used for PnL |
<!-- END config-keys -->

### `[venues.<name>.fees]`

<!-- BEGIN config-keys venues.*.fees -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `maker_bps` | number |  | maker fee, bps; negative = rebate (default 0) |
| `taker_bps` | number |  | taker fee, bps (default 0) |
<!-- END config-keys -->

### Connector-specific keys

These are validated like the keys above and handed to the connector unchanged; a key a connector does not use has no effect. Keys per connector: [Venue connectors](venues.md#configuration-keys).

<!-- BEGIN config-keys venues.*:connector -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `stale_ms` | integer |  | no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default 2000; deribit 10000) |
| `dead_ms` | integer |  | no traffic for this long forces a reconnect, ms; raised to a per-connector minimum (binance and bybit 45000, deribit 30000 by default; binance_usdm 45000 for market data, 240000 for the other channels) |
| `order_api` | string |  | order entry: ws (default) \| rest |
| `allow_offline_reference_data` | boolean |  | start without REST reference data, using the configured tick and lot (default false) |
| `cancel_on_order_channel_loss` | boolean |  | cancel all orders over REST when order entry drops (default true) |
| `emit_ack_from_response` | boolean |  | acknowledge orders from the request response, not the event stream (default true) |
| `depth_limit` | integer |  | binance: REST snapshot depth, 5 to 5000; binance_usdm: 5, 10, 20, 50, 100, 500 or 1000 |
| `key_type` | string |  | binance, binance_usdm: hmac (default) \| ed25519 |
| `private_key_file` | string |  | binance, binance_usdm: Ed25519 private key file (PKCS#8 PEM), with key_type = ed25519 |
| `private_key_env` | string |  | binance, binance_usdm: environment variable holding the Ed25519 private key PEM (instead of private_key_file) |
| `md_format` | string |  | binance: json (default) \| sbe (binary market data; needs an Ed25519 api_key) |
| `sbe_ws_url` | string |  | binance: SBE stream URL; empty = ws_url with stream. -> stream-sbe. |
| `user_stream` | string |  | binance: ws_api (default) \| listen_key \| none |
| `position_from_balance` | boolean |  | binance: derive positions from account balances |
| `position_from_account_update` | boolean |  | binance_usdm: correct the engine position from ACCOUNT_UPDATE when it differs from the fills (default true) |
| `depth` | integer |  | bybit: order book subscription depth, 1 to 1000; nasdaq_itch: price levels per side sent to the engine, 1 to 256 (default 20) |
| `ws_private_url` | string |  | bybit, deribit, binance_usdm: private WebSocket URL; empty = derived from ws_url |
| `ping_interval_ms` | integer |  | bybit: application ping interval, ms, at least 1000 |
| `orders_per_second` | integer |  | bybit: client-side order rate cap, orders/s |
| `position_from_wallet` | boolean |  | bybit: derive positions from the wallet |
| `currencies` | any |  | deribit: currencies for reference data, user channels and reconciliation, "BTC" or ["BTC", "ETH"] (default "BTC") |
| `book_interval` | string |  | deribit: book channel interval, 100ms (default) \| agg2 |
| `ticker_interval` | string |  | deribit: ticker channel interval, 100ms (default) \| agg2 |
| `trades_interval` | string |  | deribit: trades channel interval, 100ms (default) \| agg2 |
| `heartbeat_interval_s` | integer |  | deribit: public/set_heartbeat interval, s, at least 10 |
| `reject_post_only` | boolean |  | deribit: reject crossing post-only orders instead of repricing them (default true) |
| `cancel_on_disconnect` | boolean |  | deribit: cancel-on-disconnect on the order connection (default true) |
| `matching_engine_rate` | integer |  | deribit: order requests per second of the account tier (default 5) |
| `matching_engine_burst` | integer |  | deribit: order request burst of the account tier (default 20) |
| `rx_backend` | string |  | nasdaq_itch: multicast receive, kernel (UDP sockets) \| af_xdp (needs CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF, CAP_IPC_LOCK) \| dpdk (a -DFASTMM_WITH_DPDK=ON build, spin_mode = "busy") (default kernel) |
| `interface` | string |  | nasdaq_itch: interface of both lines, name or IPv4 address; af_xdp needs a name (default: routing table) |
| `line_a` | string |  | nasdaq_itch: line A, "<multicast group or local unicast address>:<port>" (required) |
| `line_b` | string |  | nasdaq_itch: line B, "<multicast group or local unicast address>:<port>"; absent = one line |
| `line_a_interface` | string |  | nasdaq_itch: interface of line A, overrides interface |
| `line_b_interface` | string |  | nasdaq_itch: interface of line B, overrides interface |
| `line_a_source` | string |  | nasdaq_itch: source address of line A: a source-specific join (default any source) |
| `line_b_source` | string |  | nasdaq_itch: source address of line B |
| `queues` | any |  | nasdaq_itch, af_xdp: RX queues to bind on every line interface, [0, 1] or "0,1" (default 0) |
| `dpdk_eal_args` | string |  | nasdaq_itch, dpdk: rte_eal_init arguments, space-separated (e.g. "--no-huge --no-pci --in-memory --vdev=net_af_packet0,iface=eth1") |
| `dpdk_port` | string |  | nasdaq_itch, dpdk: ethdev name, e.g. net_af_packet0 or a PCI address (default: the first port) |
| `dpdk_exception_port` | string |  | nasdaq_itch, dpdk: ethdev name of a net_tap vdev that carries the kernel's traffic on the port (ARP, GLIMPSE, re-requests, IGMP, kernel TCP); default none |
| `dpdk_exception_ip` | string |  | nasdaq_itch, dpdk: "a.b.c.d/len" given to the exception port's interface |
| `dpdk_exception_interval_us` | integer |  | nasdaq_itch, dpdk: how often the exception port is read, microseconds; 0 = every poll (default 20) |
| `xdp_mode` | string |  | nasdaq_itch, af_xdp: auto \| zerocopy \| native_copy \| generic (default auto: the first that works in that order) |
| `rcvbuf` | integer |  | nasdaq_itch, kernel: SO_RCVBUF, bytes; 0 = system default (default 0) |
| `batch` | integer |  | nasdaq_itch: datagrams per recvmmsg (kernel) or RX descriptors per poll (af_xdp), 1 to 1024 (default 32) |
| `rerequest` | string |  | nasdaq_itch: MoldUDP64 re-request server, "<IPv4 address>:<port>"; absent = gaps are unrecoverable |
| `glimpse_url` | string |  | nasdaq_itch: GLIMPSE 5.0 server, "<IPv4 address>:<port>"; absent = start at sequence 1 (before the directory spin) |
| `glimpse_username` | string |  | nasdaq_itch: GLIMPSE login, at most 6 characters (default glimps, the simulator's) |
| `glimpse_password` | string |  | nasdaq_itch: GLIMPSE password, at most 10 characters (default glimpse) |
| `reorder_packets` | integer |  | nasdaq_itch: packets held ahead of a gap (default 256) |
| `gap_timeout_ns` | integer |  | nasdaq_itch: a missing sequence awaited this long on every line is a gap, ns; size it from the A/B skew in the status (default 2000000) |
| `max_request_attempts` | integer |  | nasdaq_itch: re-requests of one gap before it is unrecoverable; 0 = no limit (default 4) |
| `request_timeout_ns` | integer |  | nasdaq_itch: re-send an unanswered re-request after this long, ns (default 250000000) |
| `recovery_buffer_packets` | integer |  | nasdaq_itch: datagrams buffered while a GLIMPSE snapshot is taken, allocated at start (default 65536) |
| `price_window_ticks` | integer |  | nasdaq_itch: L3 book price window per side, in 0.0001 steps, a multiple of 64; orders outside go to an overflow store (default 65536) |
| `max_orders` | integer |  | nasdaq_itch: resting orders per instrument the L3 book holds (default 262144) |
| `hw_timestamps` | boolean |  | nasdaq_itch, kernel: enable NIC receive timestamps on the line interfaces (SIOCSHWTSTAMP, CAP_NET_ADMIN) (default false) |
| `hw_clock` | string |  | nasdaq_itch: none \| phc_synced: use the NIC timestamp as recv_ts (only when the PHC is synchronised to CLOCK_REALTIME) (default none) |
| `order_entry` | string |  | nasdaq_itch: none (every order is rejected) \| sim_ouch (OUCH 5.0 to fastmm-sim-itch) (default none) |
| `ouch_url` | string |  | nasdaq_itch, sim_ouch: OUCH 5.0 server, "<IPv4 address>:<port>" |
| `order_transport` | string |  | nasdaq_itch, sim_ouch: kernel (TCP socket) \| user_tcp (experimental user-space TCP over the rx_backend's device: AF_PACKET ring, XDP socket or DPDK port; needs user_tcp_ip) (default kernel) |
| `user_tcp_ip` | string |  | nasdaq_itch, user_tcp: the connection's own IPv4 address on the interface's subnet, not assigned to any kernel interface (af_xdp, dpdk: or the host's own with user_tcp_port) |
| `user_tcp_port` | integer |  | nasdaq_itch, user_tcp: fixed local TCP port; 0 = random per connection (default 0) |
| `user_tcp_interface` | string |  | nasdaq_itch, user_tcp: netdev (default: interface) |
| `user_tcp_gateway` | string |  | nasdaq_itch, user_tcp: next hop IPv4 address when the OUCH server is not on-link |
| `ouch_username` | string |  | nasdaq_itch, sim_ouch: login, at most 6 characters (default fmouch, the simulator's) |
| `ouch_password` | string |  | nasdaq_itch, sim_ouch: password, at most 10 characters (default ouch) |
<!-- END config-keys -->

## `[[instruments]]`

<!-- BEGIN config-keys instruments[] -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `venue` | string | yes | name of a [venues.<name>] table |
| `symbol` | string | yes | venue symbol, for example BTCUSDT |
| `base` | string |  | base asset, for example BTC |
| `quote` | string |  | quote asset, for example USDT |
| `asset_class` | string |  | spot \| perpetual (perp) \| future (futures) \| option \| fx \| equity (default spot) |
| `tick` | any | yes | price increment, decimal |
| `lot` | any | yes | quantity increment, decimal |
| `min_qty` | any |  | smallest order quantity, decimal |
| `max_qty` | any |  | largest order quantity, decimal |
| `min_notional` | any |  | smallest order value, settlement currency (base coin for an inverse contract), decimal |
| `contract_multiplier` | any |  | units of the underlying per contract, decimal; PnL and notional use it (default 1) |
| `enabled` | boolean |  | trade this instrument; false loads it without trading (default true) |
| `price_decimals` | integer |  | price display precision (default 8) |
| `expiry` | string |  | expiry of a derivative, ISO-8601 |
| `strike` | any |  | option strike, decimal |
| `option_type` | string |  | call \| put |
| `maker_bps` | number |  | maker fee of this instrument, bps; negative = rebate. Overrides the venue's fees table |
| `taker_bps` | number |  | taker fee of this instrument, bps. Overrides the venue's fees table |
<!-- END config-keys -->

Live connectors replace `tick`, `lot` and the size bounds with the venue's reference data when they connect; the configured values are used by backtests and the simulator.

## `[strategy]`

<!-- BEGIN config-keys strategy -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `name` | string | yes | registered strategy name (see --list-strategies) |
| `max_param_age_ms` | integer |  | disable quoting before the first parameter update and while none was applied for this long, ms of engine time (default 0: off) |
| `params` | table |  | [strategy.params] table: the strategy's parameters |
<!-- END config-keys -->

`[strategy.params]` holds scalars, validated against the strategy's parameter schema:

```toml
[strategy]
name = "first_mm"

[strategy.params]
edge_bps = 5.0
quote_qty = 0.001
```

- Names, types, defaults and bounds come from the strategy's `FASTMM_PARAM` declarations; `--list-strategies` prints them (`--format json` for tools).
- `decimal` values take up to 8 decimals and `bps` values up to 4; exponent notation is accepted ([Fixed point](fixed-point.md#parsing-and-formatting)). `ms` and `int` values are whole numbers.
- An unknown parameter, a value with too many decimals or an out-of-range value is an error at startup.
- `--strategy` and `--param` override this section ([Command lines](cli.md#fastmm-live)).
- `max_param_age_ms` bounds the age of parameter updates sent while the session runs ([Strategy API](strategy-api.md#parameter-updates)).

## `[risk]`

Every limit is off when it is `0` or omitted. [Risk model](../explanation/risk-model.md) describes the checks and their order.

<!-- BEGIN config-keys risk -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `max_order_qty` | any |  | largest order quantity, decimal |
| `max_order_notional` | any |  | largest order value, settlement currency (base coin for an inverse contract), decimal |
| `max_position` | any |  | largest absolute position per instrument, counting same-side open orders, decimal |
| `max_open_orders` | integer |  | open orders per instrument |
| `price_collar_bps` | integer |  | refuse limit prices further than this from the mid, bps |
| `fat_finger_bps` | integer |  | refuse limit prices further than this from the last trade, bps |
| `stale_md_ms` | integer |  | refuse orders when the instrument's book is older than this, ms |
| `max_loss` | any |  | trip the kill switch when net PnL falls to -max_loss, settlement currency, decimal; latched across restarts in kill_file |
| `orders_per_sec` | integer |  | token-bucket order rate, orders/s |
| `burst` | integer |  | token-bucket capacity, orders (default orders_per_sec) |
| `stp` | boolean |  | self-trade prevention against our own resting orders (default true) |
<!-- END config-keys -->

## `[logging]`

<!-- BEGIN config-keys logging -->
| Key | Type | Required | Meaning |
|---|---|---|---|
| `level` | string |  | trace \| debug \| info \| warn (or warning) \| error \| off (default info) |
| `file` | string |  | log file; empty = stderr only |
| `mirror_level` | string |  | records at or above this level are also written to stderr (default warn) |
<!-- END config-keys -->

## `[backtest]`

Read by `fastmm-backtest`, `fastmm-replay`, the tests and the Python module (`src/backtest/backtest_config.cpp`); the section is free-form in the schema. The engine, risk and strategy settings come from the sections above; each instrument pays its own venue's `fees` table (or its own `maker_bps` / `taker_bps`), self-trade prevention follows `[risk] stp`, and in-place replace follows `[engine] supports_replace`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `source` | string | `""` | Market-data source: a name (`synthetic`, `journal`, `csv`, `binance`, `tardis`) or a whole spec with its options (`"binance:BTCUSDT,2024-03-27"`). When empty, the format is inferred from `path` ([Market-data sources](data-sources.md)) |
| `path` | string | `""` | Data file, the positional argument of a bare `source` name. `.fmj` is a journal, `.csv` is CSV; empty means synthetic data |
| `seed` | int | `[sim] seed`, else `1` | Seed for the synthetic market and the simulated venue; the engine's random generator uses `[engine] rng_seed` |
| `duration_s` | int | `[sim] duration_s`, else `60` | Simulated horizon for synthetic data, s; must be positive |
| `fill_model` | string | `"matching"` | `matching` matches our orders against the simulated order flow. `l2_queue` estimates queue position on recorded L2 data, which has no counterparties; it checks post-only orders against the same book the strategy saw, so it never produces the post-only rejects that stale market data causes under `matching`. Treat its results as optimistic |
| `queue_conservatism` | number | `1.0` | For `l2_queue`, from 0 to 1: at `0` cancellations ahead of us always move our order up the queue, at `1` they never do |
| `latency_fixed_us` | int | `200` | Fixed latency for orders to the venue and acknowledgements back, µs |
| `latency_jitter_us` | int | `50` | Random jitter added to that latency, µs, seeded |
| `latency_md_us` | int | `0` | Fixed market-data latency, µs |
| `latency_md_jitter_us` | int | `0` | Market-data latency jitter, µs |
| `p_drop` | number | `0.0` | Probability, below 1, that an outbound order message is lost |
| `equity_bar_s` | int | `1` | Bar length for the equity curve and the Sharpe ratio, s. The annualised Sharpe ratio is reported only for runs of at least 1 day (86,400 s); shorter runs report `n/a` (NaN in Python, `null` in `summary.json`) |
| `initial_capital` | number | `0` | Starting capital, quote currency. The drawdown percentage is the largest fall from peak equity divided by this value; with `0` it is not reported (NaN in Python, `null` in `summary.json`) |
| `markout_horizons_s` | string | `"1,10,60"` | Post-fill markout horizons in seconds, comma separated (`"0.5,5"` is allowed); `""` turns markouts off. The run stops the simulated clock at every fill time plus horizon to read the venue mid there, so the shortest horizon also bounds how often the run loop is entered ([Backtesting](../explanation/backtesting.md#markouts)) |
| `output_dir` | string | `"runs/backtest"` | Where `equity.csv`, `fills.csv`, `orders.csv` and `summary.json` are written |
| `journal_out` | string | `""` | When set, the backtest session is also recorded as a `.fmj` journal |

Command-line flags of `fastmm-backtest` (`--data`, `--strategy`, `--param key=value`, `--seed`, `--duration`, `--out`, `--journal-out`) override these values ([Command lines](cli.md#fastmm-backtest)).

## `[sim]`

Parameters of the synthetic market used when the data source is synthetic; free-form in the schema. Prices and sizes use the first instrument's `tick` and `lot`. `fastmm-sim-exchange` and `fastmm-sim-itch` read their own keys from `[sim]` too ([Simulated exchange](sim-exchange.md#configuration-configssimtoml), [fastmm-sim-itch](sim-itch.md#configuration-configssim-itchtoml)).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `seed` | int | `1` | Used when `[backtest] seed` is not set |
| `duration_s` | int | `60` | Used when `[backtest] duration_s` is not set, s |
| `start_mid` | decimal | `"60000"` | Initial mid price |
| `seed_levels` | int | `20` | Price levels populated before the run starts |
| `limit_rate_per_s` | number | `200` | Limit-order arrivals per second |
| `cancel_rate_per_order_s` | number | `0.5` | Cancellation hazard of each resting order, per second |
| `market_rate_per_s` | number | `10` | Market-order arrivals per second |
| `mid_step_rate_per_s` | number | `2` | Steps of one tick in the latent mid price, per second |
| `offset_p` | number | `0.35` | In (0, 1]. Limit orders land k ticks from the touch with probability p(1-p)^k |
| `base_spread_ticks` | int | `1` | Distance of the touch from the latent mid, ticks, at least 1 |
| `limit_qty_median_lots` | number | `200` | Median limit-order size in lots (log-normal) |
| `market_qty_median_lots` | number | `100` | Median market-order size in lots (log-normal) |
| `regimes` | bool | `true` | Switch between a calm and a volatile regime |
| `volatile_mult` | number | `4.0` | Multiplier on mid steps and market orders in the volatile regime |
| `depth_update_ms` | int | `100` | Book changes are aggregated into one depth diff per interval, ms, like Binance `@depth@100ms` |
| `book_ticker` | bool | `true` | Also publish top-of-book updates, at the same flush as the depth diff |

Example: `configs/backtest-example.toml`.

