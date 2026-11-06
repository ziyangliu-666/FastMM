# Price order capacity

A venue that limits each account to so many orders per window, and per day, makes each order slot a resource. With `[capacity]` the engine sends a new order, or moves a resting one, only when what it is worth covers what its slot costs at that moment. Cancels are never priced, so a cancel that takes risk off always goes. Why a price rather than a cap or a queue: [Order capacity](../../explanation/order-capacity.md).

## Observe first

Give the venue's own limits, not the connector's lower cap, and turn the price on without refusing anything:

```toml
[capacity]
mode = "observe"
value = "learned"
venues = ["binance"]    # this venue and its pool's accounts; a hedge venue stays unpriced
orders_10s = 100        # Binance Spot: 100 new orders per account per 10 s
orders_1d = 200000      # ... and 200,000 per day
fill_credit = true      # Binance gives a filled order's slot back
margin_bps = 0.8        # what a filled unit of notional earns: the maker rebate here
```

The limits apply to every account of the listed venues, so price one kind of venue per session. The window counts this engine's own orders: a second process or other strategies behind `fastmm-gateway` on the same account are not in it. Changing `[capacity]` takes a restart.

The engine prices every new order and every move, counts what the price would have refused, and sends everything. Each minute it writes one row per account to the store's [`capacity`](../../reference/storage.md#capacity) table, and logs a line when the price would have refused something:

```text
capacity: 980 actions priced in the last minute at 2.82e-06 to 1.22e-05; 0 priced out (0 new, 0 moves), 70 more would be (observed: 44 new, 26 moves)
```

Read a day per account from the store:

```bash
sqlite3 runs/mm.db "SELECT account, SUM(sent), SUM(observed_new), SUM(observed_move), MAX(peak_used), MAX(price_max) FROM capacity WHERE day = '2026-11-05' GROUP BY account"
```

`fastmm-top` and the status file show the session's totals (`capacity priced_out=... observed=...`).

## Read the price's scale

The price starts at the floor `L`, what an order of the smallest instrument `min_notional` is worth: `L = min_notional x yield x margin_bps / 10,000`. The yield is the session's filled notional per placed notional, 0.02 until it has learnt one. With a 5 USDT minimum, a yield of 0.02 and `margin_bps = 0.8`, `L` is 8e-6 USDT. It rises with the account's window up to `L` times `[risk] max_order_notional` over that minimum. `ctx.capacity(account).floor` and the store's `floor` column show it as the session runs.

## Say what each order is worth

`value = "learned"` values a quote level that states nothing at its notional times the filled notional per placed notional of its instrument, side and size class, times `margin_bps`. A strategy with a model of its own states each level's value on the same scale, in the quote currency, and optionally what its resting order is worth where it rests:

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#capacity_quote -->
```cpp
// One bid worth `value` where it is asked for, its resting order worth `keep` where it rests (both
// in the quote currency, from the strategy's own model). Priced out, the level stays empty and the
// engine applies it again when the price falls.
[[maybe_unused]] void quote_with_values(
    Ctx& ctx, InstrumentId id, Price bid, Qty qty, float value, float keep) {
  DesiredQuotes q;
  q.bid(bid, qty, value, keep);
  ctx.set_quotes(id, q);
}
// A direct order worth `value`: refused with PricedOut below the price, which the strategy reads.
[[maybe_unused]] bool send_if_worth(Ctx& ctx, InstrumentId id, Price px, Qty qty, float value) {
  auto sent = ctx.send(NewOrderRequest::limit(id, Side::Sell, px, qty).value(value));
  if (!sent && sent.error() == RejectReason::PricedOut)
    return ctx.capacity(ctx.instrument(id).venue).price > static_cast<double>(value);
  return sent.has_value();
}
```

In Python a level is `(price, qty, value)` or `(price, qty, value, keep)`, and `ctx.send(..., value=...)` prices a direct order; a refused one raises `fastmm.OrderRejected` with reason `PricedOut`. Hot hooks state no values: their levels take the `value` default. A direct order that states no value is sent without a price, and still takes a slot of the window.

| The action | What it buys |
|---|---|
| a new order into an empty level | its value |
| a move towards the touch | its gain: the level's value less the resting order's `keep`; with no `keep`, the value times the move over `full_move_bps`, up to the whole value |
| a move away from the touch | nothing for the cancel; the new order is bought as a new one |
| a cancel, a pull, a replace that only lowers the quantity | nothing |

A value of 0 or below is never sent: the price is at least `L`. With `[engine] quote_room_reserve`, a move towards the touch is held first by the reserve, then priced.

## Enforce

Switch `mode` to `"new"` to refuse new orders below their price while moves are only observed, then to `"enforce"` to refuse both. A level that is priced out stays empty, or keeps its resting order where it is, and the engine applies it again when the account's window rolls over or a fill gives a slot back. Each refusal is a [`rejects`](../../reference/storage.md#rejects) row with `source = 'capacity'`.

`ctx.capacity(account)` returns what a slot on that account costs now ([Order capacity](../../reference/strategy-api.md#order-capacity)).
