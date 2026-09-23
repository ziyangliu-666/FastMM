# Working notes

A running record of what was found, what changed, the evidence, and what is next. Newest first.
This file is for whoever picks the work up, including me after a restart. Keep entries short.

## 2026-09-23

**Where it stands.** `main` at 0.2.0, published (PyPI `fastmm-engine`, GitHub release, GHCR image).
955 tests pass with every optional codec on, 887 in the default build. Docs at https://ziy.bio/FastMM/.

**Landed today.** Venue registry (a connector owns its config and declares capabilities; `VenueKind` is
gone; an out-of-tree venue is 282 lines). Connector deduplication (four shared headers, venue tree
21,928 → 21,524 lines, `libfastmm_codecs.a` −52%). FIX and MDP3 behind `FASTMM_CODEC_*` options,
default off: 12,925 lines out of the default build. Pluggable storage (`[storage] backend`, SQLite
first) and pluggable market-data sources (`--data binance:BTCUSDT,2024-03-27`). Markouts in the
backtest. Single-file HTML run report. `fastmm-top --metrics` in Prometheus format.

**The result that matters.** `basic_mm` on real BTCUSDT perpetual data, 2024-03-27, Binance VIP-0
fees: net −499 USDT. Spread captured 0.032 bps against 2 bps of fees, and markouts −0.85 bps at 1 s,
10 s and 1 min — the fills lose money before fees. The shipped strategies are reference
implementations of published rules, not an edge.

**Known gaps, in the order I intend to close them.**

1. Recovery is asserted, not demonstrated. There is no test that disconnects a venue mid-flight,
   kills the process with orders in the market, or leaves an order in an unknown state, and then
   checks that the restarted session reaches the right position without losing a fill or sending a
   duplicate. `integration.store restart` covers the store, not the trading state.
2. No flatten and no runtime control. A kill pulls quotes and cancels; inventory stays on. Being
   built now: an `AF_UNIX` control socket routed through the control ring so operator actions are
   journaled and replayable, plus an engine-owned flatten.
3. No way to evaluate a signal without running a strategy. `microprice()` and `imbalance()` exist and
   no shipped strategy calls them. A feature/forward-markout extractor over `MdSource` would say
   whether a quote at the touch is adversely selected, before any strategy is written.
4. A sweep is a cartesian grid on one dataset with no out-of-sample structure; the best row is the
   luckiest row.
5. `Engine<Strategy>` binds one strategy; portfolio risk is per instrument plus one `max_loss`.

**Flaky.** `integration.store restart: ...` failed once under load average ~40 and passed on rerun.
Watch it; if it recurs, it is a timing assumption, not a store bug.
