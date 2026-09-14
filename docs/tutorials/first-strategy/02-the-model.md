# 2. The model

Before you write code, here is what a strategy is to the engine. Five ideas are enough for this
tutorial.

**One engine thread.** The engine owns the books, positions, orders and your strategy, and runs on
one thread. Network threads decode venue messages and pass them to it through lock-free rings. Your
code is never called concurrently, so it needs no locks.

**Hooks are plain member functions.** The engine calls `on_book` after a book update, `on_fill`
after one of your orders trades, `on_timer` when a timer fires, and so on. Every hook is optional.
The engine checks their signatures when it compiles your strategy: a hook with a wrong signature is
a build error, not a hook that is silently never called. There are no virtual calls.

**You describe quotes; the engine sends orders.** A hook calls `ctx.set_quotes(id, quotes)` with
the bids and asks it wants. The engine's quote manager compares them with your resting orders and
sends the minimum: new orders, cancels and replaces, with hysteresis so that a price that moved by
less than a tick does not cause a cancel. Every order passes pre-trade risk checks first. For
anything else there are direct orders (`ctx.send`).

**Money is exact.** Prices, quantities and amounts are 64-bit integers with 8 decimals (`Price`,
`Qty`, `Notional`); `Ratio` holds basis points. `100.25_px` and `5_bps` are compile-time literals.
Parameters are parsed from the configuration exactly, never through a `double`.

**Every input is journaled.** A session records each event the engine consumed and the clock at
which it did. Replaying the journal through the same strategy must send the same orders, byte for
byte; you use this on pages 7 and 8.

```text
venue ──► network thread ──► ring ──► engine: book ─► on_book ─► set_quotes ─► quote manager ─► risk ─► OMS ──► ring ──► network thread ──► venue
                                            └──────────────────────────── journal ──────────────────────────────┘
```

Two rules follow for hook code:

- **Do not block and do not allocate.** Hooks run on the hot path; return quickly. `noexcept` is
  recommended.
- **Only the engine's inputs may drive decisions.** Use `ctx.now()` for time and `ctx.rng()` for
  randomness, never the system clock or `std::random_device`, or replays stop matching.

Further reading: [Event flow](../../explanation/event-flow.md),
[Determinism](../../explanation/determinism.md) and [ADR-0009](../../adr/0009-crtp-strategies-over-virtual.md).

Next: [3. Write the strategy](03-write-the-strategy.md)
