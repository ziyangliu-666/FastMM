# Trading rules

An instrument's tick, lot step, minimum and maximum quantity, minimum and maximum notional and trading status are the venue's. The connector reads them when the session starts and, where it can, again while the session runs; the engine takes each change at one event, journaled, so a replay changes them at the same point.

## At start-up

Every connector reads the rules before the engine starts and writes them over the `[[instruments]]` values. fastmm-live and fastmm-gateway then compare the two for each field the configuration sets (`tick`, `lot`, `min_qty`, `max_qty`, `min_notional`) and act by `[venues.<name>] rules_mismatch`:

| `rules_mismatch` | A configured value that differs from the venue's |
|---|---|
| `"adopt"` (default) | the venue's is used; a warning names both |
| `"warn"` | the configured one is used; a warning names both |
| `"refuse"` | the session does not start: exit code 3, each difference on stderr |

```text
venues.binance: SOLUSDT lot is 0.0001 in [[instruments]], 0.01 on the venue: using the venue's
```

With `"warn"` an order that breaks the venue's rule is refused by the venue. A rule changed later on the venue is taken whatever `rules_mismatch` says.

An instrument whose venue status is not trading at start-up is kept in the table and held: nothing is sent for it until a later read finds it trading (Binance Spot). Bybit and Deribit disable such an instrument for the session ([Troubleshooting](../../how-to/operations/troubleshooting.md#reference-data)).

## While the session runs

| Connector | Reads the rules again | Key |
|---|---|---|
| `binance` | `GET /api/v3/exchangeInfo` for the session's symbols (weight 20) | `rules_refresh_s`: every 60 s by default, at least 10, 0 for none |
| the others | no | |

A connector that reads them again also reads them 5 s after the venue refused an order or a cancel for a rule or for the symbol's status: on Binance, `-1013 Filter failure` (`LOT_SIZE`, `PRICE_FILTER`, `NOTIONAL`), `-1111` and `Market is closed`. Several refusals in those 5 s cause one read.

A read that finds a field or the status changed sends the engine an `InstrumentRules` event naming the fields that changed:

| Change | What the engine does |
|---|---|
| tick, lot step, quantity or notional limit | the instrument takes the new value; every later rounding, risk check and `ctx.instrument(id)` reads it |
| trading to halted or closed | nothing more is sent for the instrument, not a new order, a replace or a cancel; working orders stay as the engine knows them; `fastmm-top` shows `not_trading` |
| halted or closed to trading | the venue reconciles the account's orders first (the venue may have ended them), then quoting resumes |

While an instrument does not trade, the connector's kill-switch cancel-all skips it: the venue refuses the request.

| Binance `status` | Engine status |
|---|---|
| `TRADING` | trading |
| `BREAK`, `HALT`, `AUCTION_MATCH`, `PRE_TRADING`, `POST_TRADING` | halted |
| `END_OF_DAY`, or the symbol missing from the reply | closed |

If the reply is an error, for example `-1121` after the venue removed a symbol, the rules stay as they were and the next read tries again.

## What a change does not move

| Part | After a change |
|---|---|
| orders already working | keep their price and quantity; the next requote replaces one that is off the new grid |
| positions and balances | unchanged by the rules; the base asset of a split reaches the engine as the venue's balance and position reports |
| fixed-point values | prices and quantities are int64 with 8 decimals whatever the tick ([Fixed-point](../fixed-point.md)); a tick or lot finer than 1e-8 cannot be represented |
| `[capacity]` minimum notional | computed from the table at start-up |
| a strategy attached to fastmm-gateway later | receives the gateway's start-up rules; changes made before it attached are not sent again |
| a backtest | the simulated venue keeps matching on a halted instrument; a journal's `InstrumentRules` records reach the strategy |

## Strategies

`on_instrument_rules(ctx, id, m)` runs after the engine took the change; `ctx.instrument(id)` already holds it and `m.changed` names the fields. A strategy that derives values from the tick or lot at start-up recomputes them here. In Python the hook is `on_instrument_rules(self, ctx, inst)`, and `inst.trading` tells whether the venue trades the instrument.

## Related

- [Journal format](../journal-format.md): the `InstrumentRules` record
- [Configuration](../configuration.md): `rules_mismatch`, `rules_refresh_s`
- [Binance Spot](binance-spot.md)
