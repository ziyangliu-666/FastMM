# Order capacity

An order's chance of a fill hardly depends on its size: a sweep through its price fills it. What it brings in grows with its notional, while the slot it takes in the account's order window costs the same at any size. A slot spent on a tiny order, or on a move that gains nothing, is a slot a valuable order cannot have when the window is busy. `[capacity]` treats each slot as a purchase ([Price order capacity](../how-to/strategies/price-order-capacity.md)).

## The price of a window

The price of the next order on an account depends on the share `z` of its window already used:

| `z` | price |
|---|---|
| below `z0 = 1 / (1 + ln R)` | `L`, the value of an order of the venue's minimum notional |
| from `z0` to 1 | `L * (R e)^z / e`, rising to `L * R` when the window is full |

`R` is `[risk] max_order_notional` over the minimum notional, so a full window admits only the largest order the engine allows. This is the online-knapsack threshold of Zhou, Chakrabarty and Lukose (2008): when it is not known what will arrive later in the window, it keeps the end of the window for the orders worth most, within a factor `ln R + 1` of what hindsight would choose. Windows are fixed, starting at multiples of `window_ms` since 1970-01-01 UTC. A window the venue counts net of fills (Binance) gives a slot back on an order's first fill.

While capacity is idle the price is `L`, and an order of minimum size costs about what it is worth. Moves that gain little are refused at any load. When a window is busy, at the opens, new orders of low value are refused too, the lowest first.

## The price of a day

A day's budget (`orders_1d` per account, `orders_per_day` for the strategy) adds a second price: the value at which the demand for the rest of the UTC day fits what is left of the budget. The demand is every action the engine priced, by value, with a profile by hour of the day kept for weekdays and weekends. While the rest of the day fits, the day price is zero.

## A move buys its gain

Moving a resting order takes a slot, so a move is worth only what it adds: the value at the new price less what the order is worth where it rests. A move away from the touch is a cancel, which is free, and a new order bought on its own value. An order that is not worth its price is not an error: its level keeps its target, and the engine applies it again when the window rolls over or a fill gives a slot back. There is no queue and no allocation.

## What it replaces

| Mechanism | Why the price does more |
|---|---|
| a minimum order notional | an arbitrary line that removes useful small unwinds at quiet times and does nothing for moves of orders above it |
| per-instrument rate caps | they ration without ranking: a cap removes a valuable order as readily as a worthless one |
| a queue sorted by value, sent when a window would overflow | a window stops at the connector's cap without overflowing, so the queue removes nothing while small orders fill the window; the price charges for the rest of the window |
| fixed reserves of orders or tokens | they protect a share without saying for what |
| a daily budget alone | zero while the day fits, and blind to the 10 s window where the scarcity is |

Every input is the engine's own: its sends, its fills, its configuration and its clock. A replay or a backtest from the journal therefore prices every order as the session did ([Determinism](determinism.md)).
