# 2. The model

The engine owns the books, positions, orders and your strategy, and runs on one thread. Network threads decode venue messages and pass them to it through lock-free rings. Hooks are never called concurrently.

Hooks are plain member functions. The engine calls `on_book` after a book update, `on_fill` after one of your orders trades and `on_timer` when a timer fires. Every hook is optional. A hook with a wrong signature fails the build (page 3).

You describe quotes; the engine sends orders. A hook calls `ctx.set_quotes(id, quotes)` with the bids and asks it wants. The engine's quote manager compares them with your resting orders and sends the new orders, cancels and replaces that make up the difference; it keeps an order whose price is within `[engine] min_requote_ticks` of the desired one. Every order passes pre-trade risk checks first. For anything else there are direct orders (`ctx.send`).

Prices, quantities, amounts and ratios are 64-bit integers with 8 decimals (`Price`, `Qty`, `Notional`, `Ratio`). `100.25_px` and `5_bps` are compile-time literals. Parameters are parsed without `double`.

A session journals each event the engine consumed and the clock at which it did. Replaying the journal through the same strategy must send the same orders, byte for byte; pages 7 and 8 check this.

```text
venue ──► network thread ──► ring ──► engine: book ─► on_book ─► set_quotes ─► quote manager ─► risk ─► OMS ──► ring ──► network thread ──► venue
                                            └──────────────────────────── journal ──────────────────────────────┘
```

Two rules follow for hook code:

- Do not block and do not allocate. `noexcept` is recommended.
- Only the engine's inputs may drive decisions. Use `ctx.now()` for time and `ctx.rng()` for randomness, never the system clock or `std::random_device`, or replays stop matching.

Further reading: [Event flow](../../explanation/event-flow.md), [Determinism](../../explanation/determinism.md) and [ADR-0009](../../adr/0009-crtp-strategies-over-virtual.md).

Next: [3. Write the strategy](03-write-the-strategy.md)
