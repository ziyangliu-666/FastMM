# 9. Trade on Binance Demo

Binance Spot Demo Mode is a Binance environment with demo balances and its own API keys. The commands on this page are in `scripts/docs/tutorial.sh --through demo`.

## 1. Dry run

<!-- snippet: scripts/docs/tutorial.sh#demo-dry-run -->
```bash
"$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --dry-run --duration 60s
```

`--dry-run` connects to public market data only, reads no keys and sends no orders; `first_mm` logs `started, quoting disabled (dry run)`. The status line shows `md=live` and `books=1/1`. Exit messages: [Troubleshooting](../../how-to/operations/troubleshooting.md).

## 2. Keys

Switch to Demo Trading on binance.com, create an API key under API Key Management, and export it in the terminal that runs the session:

```bash
export FASTMM_BINANCE_API_KEY=<demo key> FASTMM_BINANCE_API_SECRET=<demo secret>
```

Binance testnet keys do not work here ([Binance Spot Demo Mode](../../how-to/operations/run-on-testnet.md#binance-spot-demo-mode)). The configuration reads the variables as `api_key = "${FASTMM_BINANCE_API_KEY}"` ([Configuration](../../reference/configuration.md#general-rules)).

## 3. The risk limits

The strategy quotes 0.0001 BTC (about 8 USDT) per side and stops adding at 0.0003 BTC. Independently of the strategy, the engine's pre-trade checks in `configs/tutorial-binance-demo.toml` refuse anything beyond these limits:

| `[risk]` key | Value | Effect |
|---|---|---|
| `max_order_qty` | `0.0002` | no order above 0.0002 BTC |
| `max_order_notional` | `25` | no order above 25 USDT |
| `max_position` | `0.0004` | position plus same-side open orders stays within 0.0004 BTC |
| `max_open_orders` | `2` | at most 2 open orders |
| `max_loss` | `5` | the kill switch trips when net PnL reaches -5 USDT ([After a kill](../../how-to/operations/kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself)) |
| `orders_per_sec`, `burst` | `2`, `4` | orders per second, burst |

The Demo account charges 10 bps per fill and `first_mm` quotes 5 bps from the microprice, so fills lose money on average.

## 4. A five-minute session

> **Warning.** This command places orders with the keys you exported. Use Demo Trading keys, not
> the keys of a real account ([Kill switch and shutdown](../../how-to/operations/kill-switch-and-shutdown.md)).

<!-- snippet: scripts/docs/tutorial.sh#demo-run -->
```bash
"$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --duration 5m \
  --journal runs/tutorial/demo.fmj --log runs/tutorial/demo.log
```

Watch it from a second terminal:

```bash
./build/release/bin/fastmm-top --name tutorial-binance-demo
```

Within a few seconds the status line shows `md=live user=live order=live` and `fastmm-top` shows two open orders. Stop the session with Ctrl-C before the five minutes are up.

## 5. Check the shutdown

The log ends with:

```text
fastmm-live: shutdown took <n> ms (cancel_all ok)
fastmm-live: exit code 0
```

Ctrl-C tripped the kill switch. `cancel_all ok` means the venue's cancel-all request succeeded; confirm in the Demo Trading web interface that no order is open ([Go-live checklist](../../how-to/operations/go-live-checklist.md#stopping)). If the line says `cancel_all FAILED`, cancel by hand and follow [When cancel_all failed](../../how-to/operations/kill-switch-and-shutdown.md#when-cancel_all-failed); the next session also cancels, when it connects, the orders an earlier one left open.

## 6. Check the PnL

<!-- snippet: scripts/docs/tutorial.sh#demo-report -->
```bash
python3 tools/pnl_report.py runs/tutorial/demo.fmj --engine-log runs/tutorial/demo.log
```

The report lists fills, maker share, volume, fees and inventory, and compares the journal's PnL with the engine's final summary; they agree to within rounding. To reconcile against the account's balances, take balance snapshots before and after the session ([Journals, replay and PnL](../../how-to/operations/journals-replay-pnl.md#check-pnl)).

Next: [Go-live checklist](../../how-to/operations/go-live-checklist.md) before a longer session; [Run on a testnet or Binance Demo](../../how-to/operations/run-on-testnet.md) for the other practice environments (Binance USDⓈ-M Demo Trading, the Binance Spot testnet, Bybit and Deribit).
