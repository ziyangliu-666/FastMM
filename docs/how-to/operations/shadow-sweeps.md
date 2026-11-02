# Shadow sweeps

Run variants of a strategy over a live session's journal while the session writes it, and see during the session how far the simulation is from live and which variants would have done better.

You need a session that writes a journal (the default) and a strategy the shadow's program can run. The shadow is a process of its own that reads the file the session already writes: if it falls behind or dies, the session does not notice.

## Turn it on

Add to the session's configuration:

```toml
[backtest]
latency_fixed_us = 250

[shadow]
enabled = true

[shadow.grid]
half_spread_bps = [3, 4, 6, 8]
```

`fastmm-live` starts `fastmm-shadow` from its own directory, once per session name. To run it yourself, leave `enabled` out:

```bash
./build/release/bin/fastmm-shadow --config configs/sim-local.toml
```

A key of `[shadow.grid]` is a strategy parameter or a setting a sweep can vary, such as a latency or `queue_conservatism`; `[[shadow.points]]` lists points one by one. Every key: [Configuration](../../reference/configuration.md#shadow). The simulated venue is the configuration's `[backtest]` table, under `l2_queue`.

The first point has no overrides: it runs the session's own parameters, and it is the one set beside the real fills.

## Read it

```bash
./build/release/bin/fastmm-top --name sim-local
```

```text
shadow  waiting  5 points  12 windows of 30 s  0.0 s behind  updated 0.1 s ago
                        live   simulated   ratio     error
  last net             +1.43       +1.63             +0.19
  last notional         4091        4541    1.11
  last fills              71          91    1.28
  all net             +20.15      +21.68             +1.53
  all notional         58303       61518    1.06
  all fills              981        1209    1.23
  error on net, 95%: +0.71 to +2.34
  widest gaps            live net    sim net   live pos    sim pos
  BTCUSDT                  +20.15     +21.68     0.0012    0.00016
  variants                                  vs live           95% interval         
  half_spread_bps=4                           -2.65         -5.31 to +0.01  unclear
  half_spread_bps=3                          -11.76        -16.90 to -6.62    worse
  half_spread_bps=6                          -19.88       -27.03 to -12.73    worse
  half_spread_bps=8                          -20.91       -29.34 to -12.49    worse
```

| Line | Means |
|---|---|
| `following`, `catching_up`, `waiting` | at the end of the journal; behind it and reading as fast as it can; between sessions |
| `s behind` | the wall clock less the venue time reached: at least `settle_s` while following |
| `last`, `all` | the last closed window, and every window so far |
| `ratio` | simulated over live |
| `error on net, 95%` | interval of the simulation's net PnL less the session's, every window so far |
| `widest gaps` | the instruments whose simulated net PnL is furthest from live |
| `vs live` | the variant's net PnL less the live point's, both simulated |
| `better`, `worse`, `unclear` | the variant's interval lies wholly above, or below, the size of the error at the live parameters, or neither |

The intervals need 8 windows. They hold for all the variants at once, so the best of many has no luck of the draw in its interval ([Following a live session](../../explanation/backtesting.md#following-a-live-session)).

Net PnL is that of a backtest's ledger: the fills, with their fees, marked at the venue mid at each window's end, with the inventory the session started with left out. Funding and rebates paid outside the fills are in neither side.

### The report

`<journal_dir>/<name>.shadow.json` is rewritten as each window closes, and `<name>.shadow.jsonl` beside it gets one line per window.

```json
{
  "error": {
    "all": {
      "live": {
        "net": 20.14719,
        "notional": 58303.33975,
        "fills": 981,
        "orders": 5448
      },
      "sim": {
        "net": 21.675079,
        "notional": 61517.952226,
        "fills": 1209,
        "orders": 5202
      },
      "net_error": 1.52789,
      "net_error_lo": 0.712199,
      "net_error_hi": 2.343581,
      "notional_ratio": 1.055136,
      "fills_ratio": 1.232416
    }
  },
  "variants": [
    {
      "label": "half_spread_bps=4",
      "net": 19.024263,
      "diff": -2.650817,
      "lo": -5.310297,
      "hi": 0.008664,
      "verdict": "unclear",
      "estimated_live_net": 17.496373,
      "notional": 79835.234374,
      "fills": 1523,
      "orders": 4920
    }
  ]
}
```

| Key | Holds |
|---|---|
| `error.last_window`, `error.last_15_windows`, `error.all` | net, notional, fills and orders of `live` and of `sim`, the live point |
| `error.all.net_error`, `net_error_lo`, `net_error_hi` | `sim` less `live`, with its 95% interval |
| `error.by_instrument` | the same per instrument, with both positions and the error's mean and standard deviation per window |
| `variants[].diff`, `lo`, `hi`, `verdict` | against the live point, per variant |
| `variants[].estimated_live_net` | the session's real net PnL plus `diff` |
| `net_by_instrument` | per point, the live point first, the net PnL per entry of `instruments` |
| `sessions[]` | per session: its journal, how it ended, what it read and each point's `outbound_sha256` |

A `.jsonl` line has the window's `live`, `sim`, each variant's net in `variants_net`, and `instruments` for those with a fill.

### Prometheus

`fastmm-top --name <name> --metrics <port>` adds the `fastmm_shadow_*` gauges to the session's metrics: `fastmm_shadow_lag_seconds`, `fastmm_shadow_net_error`, `fastmm_shadow_live_notional`, `fastmm_shadow_sim_notional`, `fastmm_shadow_late_records` and `fastmm_shadow_variant_diff{variant="..."}` for the 8 best ([Monitor a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)).

## What the points follow

| In the session | In every point |
|---|---|
| restored strategy state, positions reconciled before the first order, first balance snapshots | the start |
| a parameter update (`fastmm-ctl param`) | taken at its time, except by the variants that set that parameter |
| `fastmm-ctl pull`, `resume`, `flatten`, `limits`, a kill switch tripped or reset | taken at its time |
| money moved in or out of an account, an account refresh | taken at its time |
| a rotation to the next part | nothing: the parts are one stream |
| a restart | the session's journal ends; every point starts again from the new session's start, and its windows go on in the same report |
| a crash | the journal has no trailer: it ends when a newer session appears, or after 300 s with no block |

A session on several venues or an account pool runs as a backtest of it does: the same accounts and routing ([Account pools](../../reference/configuration.md#account-pools)).

`latency_from_journal` needs the whole journal and is refused.

## Falling behind

A shadow restarted in the middle of a session starts from the session's first part and catches up, as fast as the disk and its cores allow ([cost](../../explanation/sweep-performance.md#following-a-live-session)). The report recomputes to the same numbers.

If it is more than `max_lag_s` behind and not gaining, half the variants stop, the highest numbered first. The live point runs on, the stopped variants keep the verdict `stopped` and their comparison stops at that window. It stays stopped until the shadow restarts.

`fastmm-shadow` moves itself off `[engine] cpu` and `net_cpus` and runs at nice 10 (`[shadow] cpus`, `nice`).

## When the numbers are exact

A point's result is that of a backtest of the finished journal with the same configuration, as long as the journal says nothing about a venue time more than `settle_s` after it. `late` in the report's `sessions[]` and `fastmm-top` count what did; at 0 the two are equal. Past 0 raise `settle_s`: the simulation runs that far behind in exchange.

To check `settle_s` against a recorded session of yours:

```bash
./build/release/bin/bench/fastmm-bench-shadow --journal runs/sim-local-1793640838419389252.fmj --compare
```

```text
18109 events over 352 s, settle 10 s: 0 differ (0.000000%), 0 past the other's end; 0 records late, 0 late for the start, 0 sequence gaps
```

## From Python

`fastmm.shadow` follows one journal to its end or to Ctrl-C and returns the report as a dict, for a C++ strategy by the journal's name or a class with `@fastmm.hot` hooks:

```python
report = fastmm.shadow("runs/mm.fmj", {"half_spread_bps": [3, 4, 6, 8]}, strategy=MyStrategy, threads=2)
report["error"]["all"]["net_error"]
```

## An application's own strategies

Give the application a second program on `fastmm::cli::shadow`, as with `fastmm::cli::live`, with the same modules, and name it in `[shadow] program`.
