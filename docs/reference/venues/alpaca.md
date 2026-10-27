# Alpaca

`kind = "alpaca"` trades US-listed stocks and ETFs through Alpaca's Trading API, with market data from Alpaca's free IEX stream ([Trade US stocks on Alpaca](../../how-to/venues/trade-us-stocks-on-alpaca.md)).

| | |
|---|---|
| Markets | US stocks and ETFs |
| Order entry | REST |
| Environments | paper (default), live |
| Keys | key id and secret key |
| Replace | `PATCH`, a new order at the venue |
| Dead man's switch | none |
| Example config | `configs/alpaca-paper.toml` |

`testnet = true`, the default, refuses the live trading host `api.alpaca.markets`.

## Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`wss://stream.data.alpaca.markets/v2/iex`) | `quotes`, `trades`; `v2/test` streams the symbol `FAKEPACA` at any hour |
| trade | `ws_private_url` (`wss://paper-api.alpaca.markets/stream`) | `trade_updates`: order states and executions, in binary frames |
| rest | `rest_url` (`https://paper-api.alpaca.markets`) | the requests below |

| request | use |
|---|---|
| `GET /v2/clock`, `GET /v2/assets/<symbol>` | clock offset, reference data |
| `POST /v2/orders` | new order |
| `PATCH /v2/orders/<id>` | replace |
| `DELETE /v2/orders/<id>` | cancel, kill switch, trading-stream loss |
| `GET /v2/orders?status=open&symbols=` | reconciliation, kill switch |
| `GET /v2/positions` | reconciliation |
| `GET /v2/account/activities/FILL` | fill replay, newest first, paged by `page_token` |
| `GET /v2/orders/<id>` | the client id of an order a replayed fill names |

## Keys

Every REST request carries `APCA-API-KEY-ID` and `APCA-API-SECRET-KEY`. Both WebSockets send `{"action":"auth","key":..,"secret":..}` first. A refused key on the trading stream is fatal for the venue. The market-data stream needs the keys too, so a dry run keeps them and sends no order.

## Reference data

| FastMM | value |
|---|---|
| tick | $0.01 (Rule 612 at or above $1.00) |
| lot | 1 share; a fractional configured lot if the asset is `fractionable` |
| asset class | equity |

An asset that is not `active` and `tradable` is disabled.

## Market data

Every IEX quote is a one-level book snapshot of IEX's own top of book. It is often wider than the national best bid and offer that paper fills use. Sizes are as the feed sends them, and trades carry no aggressor.

The stream sends nothing on subscribing and nothing for a symbol outside IEX's hours, so a book exists from the symbol's first quote. The free plan allows one market-data connection per account; a second is refused with error 406. Client pings every 5 s keep a quiet connection from going `Stale`.

## Orders

`client_order_id` is FastMM's client order id. Alpaca has no post-only order.

| FastMM | Alpaca |
|---|---|
| `PostOnly` | plain limit |
| `Gtc`, `Day` | `time_in_force`; `day` by default |
| IOC, FOK | `ioc`, `fok` |
| market | `day` |
| `extended_hours = true` | `"extended_hours":true` on resting limit orders |

A replace is `PATCH` with the new price, quantity and client id. Alpaca makes a new order with a new id, which the reply acknowledges, and ends the old one with the event `replaced`. A cancel names the venue's order id; one sent before the id is known waits for it. `DELETE` answers 204 also for an order that is over.

## Trade updates

Orders with other client ids are ignored.

| event | engine event |
|---|---|
| `new` | ack |
| `fill`, `partial_fill` | fill: this execution's price and quantity; `filled_qty` is the cumulative quantity |
| `canceled` | cancel ack |
| `expired`, `done_for_day` | expiry |
| `rejected` | reject |

Fills carry no fee and no maker or taker flag. Alpaca charges no commission; regulatory fees are summed per day and posted after the close.

## Reconciliation and fills

A reconciliation first replays the account's FILL activities since the watermark ([Execution replay](connector-behaviour.md#execution-replay)). An activity's id is `<New York time>::<execution_id>`; the part after `::` is the execution id the stream reported, so a fill is booked once whichever brings it.

Then come the open orders of the subscribed symbols and the position of every subscribed symbol from `GET /v2/positions`, zero when flat. A restarted session therefore trades from the broker's position.

On trading-stream loss the connector cancels the symbols' open orders (`cancel_on_order_channel_loss`). It reconciles when the stream is back.

## Kill switch and dead man's switch

The kill switch lists the open orders of the subscribed symbols and cancels each over its own blocking connection. It repeats until the list is empty, at most 3 rounds.

Alpaca has no dead man's switch. Orders a process left resting stay until the next session's start-up sweep cancels them or the day ends, for `day` orders.

## Rate limits

Alpaca allows 200 requests a minute per account. The connector caps new orders and replaces at 80 % of `requests_per_minute`; a 429 pauses order entry for 10 s.

## Venue rules

A strategy meets these Alpaca rules as rejects:

| order | reject reason |
|---|---|
| a buy at or above the account's open sell in the symbol, or a sell at or below its open buy (potential wash trade) | `SelfTradePrevention` |
| a short sale while a buy of the symbol is open | `SelfTradePrevention` |
| a sell larger than the long position not held by other sell orders | `InsufficientBalance` |

From a flat position the account therefore quotes one side at a time. With a long position of at least the ask size, it quotes both.

## Not handled

| what | effect |
|---|---|
| sessions and halts | the engine does not know market hours; a quote outside them waits, or trades in the extended session with `extended_hours` |
| LULD bands, short-sale restrictions, buying power | not checked by the connector |
| sub-dollar ticks, the SIP feed | not supported |
| options and crypto on Alpaca | not supported |

Sources (read 2026-10-27): the [Alpaca API reference](https://docs.alpaca.markets/reference) and its streaming pages.
