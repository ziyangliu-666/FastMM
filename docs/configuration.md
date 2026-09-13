# Configuration

FastMM reads a single TOML file (`--config path.toml`). Secrets come **only** from environment
variables through `${VAR}` substitution, which is applied to string values inside `[venues.*]`.

```toml
[engine]
strategy   = "basic_mm"          # registered strategy name
venues     = ["binance"]
engine_cpu = 3                    # -1 = no pinning
net_cpu    = 2
spin_mode  = "adaptive"           # busy | adaptive (WSL2 default)
journal    = "runs/session.fmj"

[venues.binance]
api_key     = "${FASTMM_BINANCE_API_KEY}"
api_secret  = "${FASTMM_BINANCE_API_SECRET}"
rest        = "https://testnet.binance.vision"
ws          = "wss://stream.testnet.binance.vision"
ws_api      = "wss://ws-api.testnet.binance.vision/ws-api/v3"
order_transport = "ws_api"        # ws_api | rest
ca_file     = ""                  # set to tests/fixtures/tls/cert.pem for the local sim
insecure_tls = false
stale_ms    = 2000
dead_ms     = 10000
recv_window_ms = 3000

[venues.binance.fees]
maker_bps = 1.0
taker_bps = 1.0

[[instruments]]
venue  = "binance"
symbol = "BTCUSDT"
id     = "BTC-USDT"

[strategy]                        # keys validated against the strategy's ParamSchema
half_spread_bps = 5
skew_bps_per_unit = 2
quote_qty = 0.001
max_inventory = 0.01

[risk]
max_order_qty       = 0.005
max_order_notional  = 2000
max_position        = 0.02
max_open_orders     = 10
collar_bps          = 50
fat_finger_bps      = 200
max_md_age_ms       = 1500
msgs_per_sec        = 20
kill_switch_loss    = -50         # quote currency

[logging]
level = "info"
file  = "runs/engine.log"

[sim]                             # used by fastmm-sim-exchange and in-process backtests
seed = 42
mid0 = 60000
vol_bps = 5
arrival_rate = 200

[backtest]
source = "journal"                # journal | csv | synthetic
path = "tests/fixtures/journals/sample_1000.fmj"
fill_model = "l2_queue"           # matching | l2_queue
latency_ns = 2_000_000
jitter_ns  = 500_000
```

Rules:

- A literal that looks like a secret (32+ alphanumerics without `${`) is rejected unless
  `--allow-inline-secrets` is passed.
- `Config::redacted()` is what gets logged and journaled; secrets never reach disk.
- Unknown keys are errors; every error carries the TOML line number.

## Failure handling

| Failure | Detection | Action |
|---|---|---|
| Market-data disconnect | EPOLLRDHUP / read 0 / TLS error | quotes pulled, backoff reconnect, fresh snapshot |
| Sequence gap | book sync FSM | `Resyncing` state, re-snapshot (rate limited), quotes pulled meanwhile |
| Stale feed | `now - last_rx > stale_ms` | `Stale` event; `dead_ms` forces reconnect |
| Order channel loss | connection FSM | immediate `cancel_all` via REST, reconcile open orders after reconnect |
| Kill switch | risk engine / SIGINT | `cancel_all` on every venue, stop quoting, exit after acks or 5 s |
| Rate limit | headers / error codes | cooldown until reset; HTTP 418 halts REST for the ban |
| Clock skew | `-1021` / `10002` | resync offset from the venue's time endpoint |
| Ring overflow | `push` fails | drop market-data delta and resync; order events are never dropped |
