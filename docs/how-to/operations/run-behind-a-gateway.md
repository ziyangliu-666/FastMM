# Run strategies behind a gateway

`fastmm-gateway` holds the venue connections and the API keys; strategy processes attach to it. A strategy can stop, crash or be replaced while the venue sessions stay up and the other strategies keep trading. When a strategy process goes away, `kill -9` included, the gateway cancels its orders at once.

![fastmm-gateway holds the venue connection and the keys; each strategy process attaches with three shared-memory rings per venue, and its orders pass the account guards](../../assets/diagrams/gateway.svg#only-light){ .fastmm-diagram }
![fastmm-gateway holds the venue connection and the keys; each strategy process attaches with three shared-memory rings per venue, and its orders pass the account guards](../../assets/diagrams/gateway-dark.svg#only-dark){ .fastmm-diagram }

Prerequisites: a release build ([Install](../../getting-started/install.md)), commands run from the repository root.

## Start a gateway and attach a strategy

1. Start the simulated exchange and the gateway. The gateway needs the API keys:

    ```bash
    ./build/release/bin/fastmm-sim-exchange --config configs/sim.toml &
    export FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret
    ./build/release/bin/fastmm-gateway --config configs/sim-local.toml &
    ```

    ```text
    INFO  sim: reference data loaded for 1 symbols (4 rate limit rules)
    INFO  status: /dev/shm/fastmm-sim-local.gw.status (watch with fastmm-top --path /dev/shm/fastmm-sim-local.gw.status)
    INFO  control socket: runs/sim-local.gw.ctl (fastmm-ctl --path runs/sim-local.gw.ctl status)
    INFO  fastmm-gateway: 1 venue(s), 1 instrument(s), dry_run=false, net=epoll, ...
    ```

2. Attach a strategy. It needs no keys, and its `[engine] name` must differ from the gateway's and every other strategy's; here a copy of the configuration with `name = "mm-btc"`:

    ```bash
    ./build/release/bin/fastmm-live --config configs/mm-btc.toml --gateway runs/sim-local.gw
    ```

    ```text
    INFO  gateway: attached to runs/sim-local.gw (attachment 1, session epoch 1): 1 venue(s), 1 of 1 instrument(s) traded here, with the gateway's reference data
    ```

3. List what is attached:

    ```bash
    ./build/release/bin/fastmm-ctl --gateway sim-local attachments
    ```

    ```text
    attachment=1 epoch=1 engine=mm-btc pid=42911 up=8s instruments=sim:BTCUSDT md_dropped=0 refused=0
    ```

4. Watch the account: `fastmm-top --gateway sim-local` ([Monitor](#monitor)).

The socket is `<journal_dir>/<engine name>.gw` of the gateway's configuration, mode 0600; `--socket` moves it.

## Split the configuration

Both processes can read the same file; each uses its own part of it.

| | Gateway | Strategy |
|---|---|---|
| `[engine]` | `name`, `journal_dir`, `epoch_file`, `kill_file`, `spin_mode`, `net_cpus`, `net_backend`, ring sizes | everything else; its own `name`, so its own store, journal and kill file |
| `[venues.*]` | yes, with the API keys | no keys; venues and ids come from the gateway |
| `[[instruments]]` | every instrument any strategy trades | the ones it trades (venue and symbol); tick, lot and limits come from the gateway |
| `[gateway]`, `[gateway.shared]`, `[gateway.underlying]` | yes | no |
| `[strategy]`, `[risk]` | no | yes |

A strategy's `[engine] epoch_file` is not used: the gateway hands out the epochs. A session with `[engine] threading = "single"` cannot attach.

## What happens on attach

The gateway refuses an attach that:

- names an instrument the gateway does not have,
- names one a live attachment trades, unless `[gateway.shared]` lists it,
- uses the `[engine] name` of an attached strategy,
- would be the 17th (at most 16 at a time),
- arrives while the account's kill switch is latched ([Account risk](#account-risk)).

Otherwise the gateway:

1. gives the strategy a session epoch, the high 16 bits of every client order id, unique across strategies and gateway restarts;
2. creates three rings per venue under `/dev/shm`, which the strategy unlinks once mapped, so a `kill -9` of either side leaves nothing behind;
3. sends a snapshot of every book from its own copy ([Books and routing](#books-and-routing));
4. reconciles the account's executions since the strategy's last stored fill, then the open orders ([Recovery at start-up](../../reference/storage.md#recovery-at-start-up));
5. answers with the epoch, the instrument table, the ring paths and the wake-up descriptors.

The strategy restores its position from its own store, as on a restart ([What survives a restart](running-in-production.md#restarts)). Its attach request carries that position, which seeds the gateway's account, and its earlier sessions' open orders with their venue ids. Instruments of other strategies stay in its table, disabled: their market data arrives, but it does not quote them.

## Account guards

Every order passes the gateway on its way to the venue. These checks run in order, over every attached strategy; a refused order goes back to its sender as an `OrderReject` ([Reject reasons](../../reference/errors.md#gateway)) and its quote manager backs that side off.

| Check | Key ([Configuration](../../reference/configuration.md#gateway)) | Refused as |
|---|---|---|
| the instrument is one the sender claimed | | `GatewayNotOwner` |
| the account's kill switch is off | `max_loss` | `GatewayAccountKilled` |
| it would not trade with another strategy's resting order on a shared instrument | `[gateway.shared]` | `GatewaySelfTrade` |
| notional working at the venue, both sides, every strategy; a replace counts the difference | `max_open_notional` | `GatewayOpenNotional` |
| the account's positions at the marks plus this order; reducing orders pass | `max_gross_notional`, `max_net_notional` | `GatewayGrossNotional`, `GatewayNetNotional` |
| net position in the base asset over every venue, plus working orders on that side; orders towards zero pass | `[gateway.underlying.<BASE>] max_net` | `GatewayUnderlyingNet`, `GatewayUnderlyingMarkUnknown` |
| the venue's last balance, moved by every strategy's orders and fills since, covers the order | `check_balance` | `GatewayBalanceShort` |
| new orders and replaces of every strategy together, per venue; cancels always go | `orders_per_sec`, `burst` | `GatewayRateLimit` |
| the currency's rate source is fresh | `[accounting]` | `GatewayFxRateUnknown` |

An inverse contract counts at the mid of the gateway's own book. While one with a position or working orders has no book newer than `[risk] stale_md_ms`, orders on that underlying are refused. `check_balance` stays off for a venue until it reports balances. Each strategy's own `[risk]` still applies to it ([Risk model](../../explanation/risk-model.md#net-position-per-underlying)).

## Account risk

The gateway books every execution that passes through it, once, whichever strategy it goes to. It marks positions at the mid of its own book copy, after the strategies have been woken.

| Moment | What the account does |
|---|---|
| fresh gateway | knows no positions; the first strategy that attaches owning an instrument seeds it with the position from its store |
| replayed execution older than that seed | already in the position, not booked |
| strategy detaches | the position stays with the account; the next owner finds it |
| net PnL at or below `-max_loss` | trips the account's kill switch |

`max_loss` counts realized plus unrealized PnL minus fees, over every strategy, plus what earlier runs carried. Realized PnL and fees persist in the gateway's kill file (`[engine] kill_file`, default `<journal_dir>/<name>.kill`, [format](kill-switch-and-shutdown.md#the-latched-loss-budget)). Give the gateway an `[engine] name` of its own: with `max_loss` set it refuses a strategy with the same name.

When the account trips, the gateway:

1. refuses new orders and replaces with `GatewayAccountKilled`;
2. sends each strategy `TripVenueKill` (`GatewayMaxLoss`), so its engine pulls quotes, cancels its orders and trips its own kill switch;
3. cancels every order it knows, asks each venue for its open orders and cancels each, and calls each venue's `cancel_all`;
4. refuses every attach.

The trip is latched: a restart exits 6 until `fastmm-gateway --clear-kill`, removing the kill file, or `fastmm-ctl --gateway <name> clear-kill` ([Control](#control)).

Once a second the gateway logs the account and each position that changed:

```text
gateway: account net_pnl=0.04858381 realized=0.0481 unrealized=0.018485 fees=0.01800119 carried=0 gross_exposure=60.005585 net_exposure=-60.005585 kill=off max_loss=0
gateway: account position sim:BTCUSDT -0.001
gateway: account underlying BTC net=0.004
```

### More than one settlement currency

The limits are one number in one currency. When instruments settle in more than one, set `[accounting]` in the gateway's configuration ([Configuration](../../reference/configuration.md#accounting)). Every instrument must settle in `reporting_currency` or name a source instrument whose mid gives the rate.

Positions stay in their own currencies; PnL, exposure, the kill file and the log line (`in=USDT`) are converted at current rates. Without `[accounting]`, a gateway with any limit set refuses a mixed-currency table (exit 3). `max_open_notional` stays per venue, unconverted.

A strategy's own `[risk]` converts with its own `[accounting]`. Its source may be another strategy's instrument, listed in its `[[instruments]]` with `enabled = false`.

## Shared instruments

Several strategies can trade one instrument: two cross-venue pairs hedging into the same perpetual, a market maker next to a hedger, or an old and a new version side by side. List it in the gateway's configuration ([Configuration](../../reference/configuration.md#gatewayshared)):

```toml
[gateway.shared."bybit:BTCUSDT"]
primary = "hedger"    # [engine] name; leave it out for none
```

Each strategy names it in its own `[[instruments]]` and keeps its own store, position and `[risk]`. The gateway handles the instrument differently in these ways:

| Event | Goes to |
|---|---|
| fill of a live order | the strategy whose epoch the client order id carries |
| fill of an earlier session's order | that session's strategy, also after a restart of either side; a strategy sends the epochs of its last 64 sessions on attach |
| fill whose order the venue still has to be asked for (Binance, no ack came) | nobody until the lookup resolves, then its strategy ([Venue connectors](../../reference/venues/connector-behaviour.md#execution-replay)) |
| liquidation, ADL, outside orders, funding | the `primary`; while it is away its replay delivers them later. Without a primary, the account books them unattributed |
| venue position records | nobody; the gateway compares them with the strategies' shares |

The account's position is the venue's. The gateway splits it into each strategy's share, an unattributed part, and an unexplained part, which is zero when the books agree. Each strategy's share should equal its own store's position.

```text
gateway: account position bybit:BTCUSDT 0.004 (shared: mm-a=0.003 hedger=0.002 unattributed=-0.001)
gateway: account position bybit:BTCUSDT 0.005 does not match its strategies' (shared: mm-a=0.003 hedger=0.002 unattributed=-0.001): off by 0.001
```

An order that would trade with another strategy's resting order goes back as `GatewaySelfTrade` before it reaches the venue; the check takes 1.3 ns with 2 resting orders and 14 ns with 32. IOC, FOK and market orders never rest, so they never block another strategy. The venue's own self-trade prevention stays as a second line.

`fastmm-ctl --gateway <name> attachments` adds each strategy's share (`share=bybit:BTCUSDT=0.003`).

## Monitor

The gateway publishes its state every 250 ms to `/dev/shm/fastmm-<name>.gw.status`; `--status <path>` moves it and `--no-status` turns it off.

```bash
./build/release/bin/fastmm-top --gateway sim-local
```

```text
fastmm-top  gateway=sim-local pid=42870
state      running  uptime=13s  updated 0.1s ago

account    net_pnl=0.53489389 realized=0.6821 unrealized=0.02679 fees=0.17399611 carried=0 gross_exposure=59.9971 net_exposure=59.9971
limits     max_loss=off max_gross_notional=off max_net_notional=off max_open_notional=off

balance    sim BTC free=100 locked=0.001 total=100.001
balance    sim USDT free=9999880.57348389 locked=59.96431 total=9999940.53779389

attachment epoch engine                    pid        up md_dropped refused                  instruments
1              1 mm-btc                  42911       10s          0 0                        sim:BTCUSDT

position       instrument                            qty  owner
sim            BTCUSDT                             0.001      1
...
routing        md_discard ord_discard  unrouted gw_cancels untracked   stale  skipped books_lost refused
sim                   109          6         0          0         0       0        0          0 0
```

A tripped account shows `ACCOUNT KILLED (<reason>)` and `LATCHED`; an unexplained part of a shared position shows in red. `--json` prints the frame with `"kind": "gateway"`; the layout is in [Status file](../../reference/status-file.md#gateway-block).

`--metrics 9110` serves Prometheus metrics: the session families `fastmm-live` exports ([Monitoring a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)) and these.

| Metric | Type | Meaning |
|---|---|---|
| `fastmm_info{gateway,pid}` | gauge | constant 1 |
| `fastmm_kill_active`, `fastmm_kill_latched`, `fastmm_kill_reason` | gauge | the account's kill switch, whether the kill file latches it, the `KillReason` |
| `fastmm_account_net_pnl`, `_realized_pnl`, `_unrealized_pnl`, `_fees`, `_pnl_carry` | gauge | the account, quote currency (`[accounting] reporting_currency` when set) |
| `fastmm_account_gross_exposure`, `_net_exposure` | gauge | at the marks |
| `fastmm_account_max_loss`, `_max_gross_notional`, `_max_net_notional` | gauge | `[gateway]`, 0 when off |
| `fastmm_account_position{venue,instrument}` | gauge | base units |
| `fastmm_account_underlying_net{underlying}`, `_underlying_max_net{underlying}` | gauge | `[gateway.underlying]`, base units; the net is absent while an inverse contract has no mark |
| `fastmm_balance_free`, `_locked`, `_total`, `_equity`, `_maintenance` `{venue,asset,account}` | gauge | the account's balances over every strategy, asset units |
| `fastmm_perp_mark`, `_index`, `_funding_rate`, ... `{venue,symbol}` | gauge | the venues' mark, index and funding; positions are valued at the mark while it is fresh |
| `fastmm_gateway_instrument_owner{venue,instrument}` | gauge | the owner's epoch; absent while nobody trades it, and for a shared instrument |
| `fastmm_gateway_instrument_traders{venue,instrument}` | gauge | attachments trading it |
| `fastmm_account_unattributed{venue,instrument}`, `_unexplained{venue,instrument}` | gauge | a shared instrument's parts, base units; alert on `_unexplained != 0` |
| `fastmm_gateway_attachments` | gauge | strategies attached |
| `fastmm_gateway_attachment_info{epoch,engine,pid,attachment}` | gauge | constant 1 per attachment |
| `fastmm_gateway_attachment_uptime_seconds{epoch,engine}` | gauge | since it attached |
| `fastmm_gateway_attachment_md_dropped_total{epoch,engine}` | counter | market data its rings dropped |
| `fastmm_gateway_attachment_refused_total{epoch,engine,reason}` | counter | its orders the gateway refused |
| `fastmm_gateway_refused_total{venue,reason}` | counter | the same per venue |
| `fastmm_gateway_md_discarded_total`, `_order_discarded_total`, `_unrouted_total`, `_cancels_total`, `_untracked_total`, `_stale_replays_total`, `_account_skipped_total`, `_account_books_lost_total` `{venue}` | counter | the once-a-second log line's counters |

An epoch is unique per attachment for the gateway's life, so it keys a strategy's series across a restart of that strategy.

## Control

`fastmm-ctl --gateway <name>` talks to `<journal_dir>/<name>.gw.ctl`, the same wire format as `fastmm-live`'s socket ([Operating a running session](operate-a-running-session.md#the-socket)). `--control <path>` moves it and `--no-control` leaves it out.

```bash
./build/release/bin/fastmm-ctl --gateway sim-local pull --venue sim
```

```text
ok pull queued (venue 0), sent to 1 strategies
```

| Command | What it does |
|---|---|
| `pull [--instrument SYM \| --venue NAME]` | the strategies in scope stop quoting; each gets `PullQuotes` on its order ring, so its journal records it |
| `resume [--instrument SYM \| --venue NAME]` | `ResumeQuotes` the same way |
| `kill` | trips the account as `max_loss` does, reason `GatewayOperator`; with `max_loss` set the kill file latches it, otherwise it holds until `clear-kill` or exit |
| `clear-kill` | clears the kill switch and the kill file and re-arms the whole `max_loss` budget; refused while any strategy is attached |
| `attachments` | one line per attached strategy |
| `status` | the `fastmm-top` frame |
| `help` | the command list |

A `pull` reaches the strategies attached at that moment; one that attaches later quotes. Before `clear-kill`, stop every strategy (`on_kill = "exit"` does), since each stays killed in its own engine.

## Detach and restart

Closing the connection is the detach; the kernel closes it when the process dies. The gateway then:

1. takes the strategy out of the routing;
2. cancels each working order of its epoch, one by one, so the other strategies keep their quotes;
3. asks the venue for open orders and cancels any of that epoch it did not know;
4. frees the instruments and removes the rings.

A late acknowledgement for the dead epoch is cancelled too. A strategy that stops cleanly cancels its own quotes first and logs `no venue cancel_all here`.

| Process stops | Effect |
|---|---|
| a strategy, any way | its orders cancelled; the others trade on |
| the gateway, SIGINT or SIGTERM | detaches everyone, cancels every open order on every venue |
| the gateway, any way | each strategy exits 5 |

### Under systemd

`deploy/fastmm-gateway.service` runs the gateway, `deploy/fastmm-live@.service` one strategy per instance. `fastmm-live@mm1` runs `/etc/fastmm/mm1.toml` attached to `/var/lib/fastmm/runs/gateway.gw`, so the gateway's `[engine] name` is `gateway`.

```bash
sudo systemctl enable --now fastmm-gateway fastmm-live@mm1 fastmm-live@mm2
```

An enabled instance is wanted by the gateway: every gateway start, crash restarts included, starts the strategies again, and stopping the gateway stops them. The gateway's unit is `Type=notify`, so strategies start only once its socket listens. After a `kill -9` of the gateway, systemd restarts it within 2 s and the strategies reattach with their positions.

## Books and routing

The gateway keeps a copy of every book, up to 1024 levels a side. A strategy that attaches, or whose ring dropped, gets a snapshot from that copy followed by the live events, so its 256-level book matches the venue's. The venue is not asked for anything and the other strategies do not pause. Only when the gateway's own copy lost events (`account books lost`) does it ask the venue to resync, at most once a second.

| Event | Goes to |
|---|---|
| market data | every attachment; one that falls behind drops data alone, then gets `Resyncing` and fresh snapshots |
| order event | the strategy whose epoch its client order id carries |
| fill of an epoch nobody holds, or naming no order | the strategy that trades the instrument; with none, kept (up to 4096 per venue) for the one that attaches |
| reconciliation | the strategies that asked for it, or all when the connector started it; a row of a dead epoch is cancelled |
| balance report | every attachment |
| replayed fill | like a streamed one; each engine deduplicates by execution id |

## Latency

With `spin_mode = "adaptive"` an idle side sleeps and the other wakes it, a system call only while the other side sleeps. Each event and order is copied once more than in-process.

Tick-to-trade p50 against the simulator with one strategy attached, in µs, on an unpinned WSL2 host (method: `scripts/bench-gateway.sh`):

| `spin_mode` | engine, in-process | engine, gateway | wire, in-process | wire, gateway |
| --- | --- | --- | --- | --- |
| `adaptive` | 34.8 | 36.9 | 66.4 | 70.3 |
| `busy` | 7.2-7.7 | 7.9-9.2 | 41.0 | 43.0 |
