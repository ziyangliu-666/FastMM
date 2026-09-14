# Glossary

| Term | Meaning |
|---|---|
| **ack** | a venue's confirmation that it accepted an order (`OrderAckMsg`); an order is working only after it |
| **basis point (bp, bps)** | 0.01 % = 0.0001. `5_bps` is a `Ratio` of 0.0005; 5 bps of 60,000 USDT is 30 USDT |
| **book** | the price levels of one instrument: an L2 book aggregates quantity per price, an L3 book keeps each order |
| **book ticker** | a top-of-book update (best bid and ask with quantities) without depth |
| **cancel-all** | a request that cancels every open order of an instrument or account; `fastmm-live` sends one per venue over REST at shutdown |
| **cancel-replace** | amending a working order's price or quantity in one request; FastMM uses it when the venue and `supports_replace` allow, else cancel then new |
| **channel** | one connection of a venue: 0 is market data, 1 is order entry and the user stream (`ConnectionStateMsg::channel`) |
| **client order id** | FastMM's id of an order: the session epoch in the upper 32 bits and a sequence number in the lower 32 |
| **context (`ctx`)** | the strategy's view of the engine passed to every hook (`StrategyContext`) |
| **desired quotes** | the ladder a strategy asks for with `set_quotes` (`DesiredQuotes`), up to 8 levels per side |
| **dry run** | `--dry-run`: public market data only, no keys, quoting disabled |
| **engine clock** | the time the engine uses for all decisions, read once per event; recorded in the journal |
| **epoch** | see *session epoch* |
| **fill** | an execution of one of our orders; `on_fill` receives a `Fill` |
| **fixed point** | integers with an implied scale: `Price`, `Qty` and `Notional` count units of 1e-8 ([Fixed point](fixed-point.md)) |
| **harness** | `StrategyHarness<S>`, a real engine with a simulated venue for unit tests |
| **hook** | a strategy member function the engine calls on an event (`on_book`, `on_fill`, ...) |
| **hysteresis** | the quote manager keeps a resting quote whose price or quantity is close enough to the desired one (`min_requote_ticks`, `min_qty_bps`) |
| **instrument table** | the instruments of a session, indexed by `InstrumentId` |
| **journal (`.fmj`)** | the file of every event a session consumed, in order, with the engine clock ([Journal format](journal-format.md)) |
| **kill switch** | a flag, global or per venue, that blocks new orders and replaces but not cancels ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)) |
| **late fill** | a fill for an order that was already terminal (for example cancelled) |
| **level** | one price with its quantity (`Level{price, qty}`); level 0 is the best |
| **lot** | the quantity increment of an instrument; quantities are multiples of it |
| **maker, taker** | a maker's order rested in the book; a taker's order traded against it |
| **microprice** | the touch weighted by the opposite side's size: `(bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)` |
| **mid** | `(best bid + best ask) / 2` |
| **notional** | price times quantity, in the quote currency (`Notional`) |
| **OMS** | the order management system: the state machine of every order from send to a terminal state |
| **outbound hash** | the SHA-256 over every order message a run sent; equal hashes mean identical order streams |
| **post-only** | an order the venue rejects (or reprices) if it would trade on arrival, so it is always a maker order |
| **quote** | a resting bid or ask a market maker keeps in the book; FastMM's quote manager owns quote orders |
| **quote manager** | the engine component that turns desired quotes into new, cancel and replace orders |
| **Ratio** | a dimensionless fixed-point factor; 1.0 is raw 100,000,000 and 1 bp is raw 10,000 |
| **raw** | the integer inside a fixed-point value (`.raw`); `1.5_px` has raw 150,000,000 |
| **reconciliation** | after a reconnect, the venue's open orders are compared with the OMS: Begin, open orders, End |
| **registry** | the table of strategies by name with their Sim, Replay and Live factories |
| **replay** | running a journal back through the engine and strategy; `--verify` compares the order stream |
| **session epoch** | a counter stored in `[engine] epoch_file` that makes client order ids unique across restarts |
| **sim exchange** | `fastmm-sim-exchange`, a local exchange that speaks the Binance Spot API ([Simulated exchange](sim-exchange.md)) |
| **spread** | best ask minus best bid; a quoted spread is our ask minus our bid |
| **stale** | a feed with no traffic for `stale_ms`; the engine pulls the venue's quotes |
| **STP** | self-trade prevention: an order that would trade against our own resting order is refused |
| **strategy module** | a strategy library's registration function (`StrategyModule`) |
| **testnet, Demo Mode** | practice environments of a venue with their own keys: Binance Demo Mode market data follows the real market, testnets have their own thin books |
| **tick** | the price increment of an instrument; prices are multiples of it |
| **touch** | the best bid and best ask |
| **transport** | how the engine sends orders: Sim (in-process matching), Replay (from a journal) or Live (a venue) |
| **TSC** | the CPU's time-stamp counter; `TscClock` maps it to wall time |
| **venue** | an exchange or simulator connection, configured under `[venues.<name>]` |
