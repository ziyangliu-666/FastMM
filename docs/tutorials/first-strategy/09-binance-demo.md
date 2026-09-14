# 9. Trade on Binance Demo

In this page you run `first_mm` on Binance Spot Demo Mode: realistic market data and a demo
account with demo balances. You first run without keys, then place real demo orders for five
minutes with tight risk limits, stop the session and check it.

> **Warning.** This page places orders on an exchange. Use Demo Trading keys only: they are not
> testnet keys, and never use keys of a real account. Read
> [Kill switch and shutdown](../../how-to/operations/kill-switch-and-shutdown.md) before you start.

CI does not run this page. The commands are in `scripts/docs/tutorial.sh` (`--through demo`).

## 1. Dry run

<!-- snippet: scripts/docs/tutorial.sh#demo-dry-run -->
```bash
"$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --dry-run --duration 60s
```

`--dry-run` connects to the public market data only: no keys are read and no order is sent, and
`first_mm` logs `started, quoting disabled (dry run)`. Once a second the log shows the venue's
status; a healthy run has `md=live` and `books=1/1`. If it exits instead, look up the message in
[Troubleshooting](../../how-to/operations/troubleshooting.md).

## 2. Keys

Switch to Demo Trading on binance.com, create an API key under API Key Management, and export it in
the terminal that runs the session:

```bash
export FASTMM_BINANCE_API_KEY=<demo key> FASTMM_BINANCE_API_SECRET=<demo secret>
```

The configuration refers to these variables (`api_key = "${FASTMM_BINANCE_API_KEY}"`); FastMM
refuses a literal secret in a configuration file.

## 3. The risk limits

`configs/tutorial-binance-demo.toml` keeps the session small. The strategy quotes 0.0001 BTC
(about 8 USDT) per side and stops adding at 0.0003 BTC; independently of the strategy, the
engine's pre-trade checks refuse anything beyond these limits:

| `[risk]` key | Value | Effect |
|---|---|---|
| `max_order_qty` | `0.0002` | no order above 0.0002 BTC |
| `max_order_notional` | `25` | no order above 25 USDT |
| `max_position` | `0.0004` | position plus same-side open orders stays within 0.0004 BTC |
| `max_open_orders` | `2` | at most 2 open orders |
| `max_loss` | `5` | the kill switch trips when net PnL reaches -5 USDT; `tutorial-live` then cancels everything and exits with code 6 |
| `orders_per_sec`, `burst` | `2`, `4` | order rate, far below Binance's limits |

The Demo account charges 10 bps per fill and `first_mm` quotes 5 bps from the microprice, so
expect a small loss per fill: the point of this page is the procedure, not the profit.

## 4. A five-minute session

<!-- snippet: scripts/docs/tutorial.sh#demo-run -->
```bash
"$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --duration 5m \
  --journal runs/tutorial/demo.fmj --log runs/tutorial/demo.log
```

Watch it from a second terminal:

```bash
./build/release/bin/fastmm-top --name tutorial-binance-demo
```

Within a few seconds the status line shows `md=live user=live order=live`, and `fastmm-top` shows
two open orders. Stop the session before the five minutes are up with Ctrl-C.

## 5. Check the shutdown

The last line of the log must be:

```text
fastmm-live: shutdown took <n> ms (cancel_all ok)
```

Ctrl-C tripped the kill switch: the engine pulled the quotes and each venue cancelled all open
orders over a separate REST connection. `cancel_all ok` means those requests succeeded; confirm in
the Demo Trading web interface that no order is left open. If the line says `cancel_all FAILED`,
cancel by hand and follow
[When cancel_all failed](../../how-to/operations/kill-switch-and-shutdown.md#when-cancel_all-failed).

## 6. Check the PnL

<!-- snippet: scripts/docs/tutorial.sh#demo-report -->
```bash
python3 tools/pnl_report.py runs/tutorial/demo.fmj --engine-log runs/tutorial/demo.log
```

The report lists fills, maker share, volume, fees and inventory, and compares the journal's PnL
with the engine's final summary; they should agree to within rounding. To reconcile against the
account's balances as well, take balance snapshots before and after the session
([Journals, replay and PnL](../../how-to/operations/journals-replay-pnl.md#check-pnl)).

## Where next

- [Go-live checklist](../../how-to/operations/go-live-checklist.md) before a longer session
- [Run on a testnet or Binance Demo](../../how-to/operations/run-on-testnet.md) for Bybit, Deribit
  and the Binance testnet
- [Strategy API](../../reference/strategy-api.md), [Risk model](../../explanation/risk-model.md)
