# Market-data sources

A backtest reads its market data through a named source. `fastmm-backtest --data <spec>`, `[backtest] source` and `run_backtest(data=...)` all resolve the same spec through one registry, so a new source is one file plus a registration and nothing about it reaches the configuration schema.

```bash
./build/release/bin/fastmm-data list
```

## Spec syntax

```text
<name>[:<arg>[,<arg>...]]      arg := <key>=<value> | <positional>
```

Only the first `:` separates the name, so a value may contain one (`start=09:30`). Arguments without an `=` fill the source's positional options in order. A bare path with a known extension works too: `--data runs/session.fmj` is `--data journal:runs/session.fmj`.

```bash
--data synthetic
--data binance:BTCUSDT,2024-03-27
--data binance:symbol=BTCUSDT,date=2024-03-27,start=13:00,end=14:00
--data tardis:binance-futures,BTCUSDT,2026-09-01
--data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
```

An unknown option is an error naming the ones the source takes, so a typo fails at startup rather than silently changing nothing.

## What each source carries

| Source | Top of book | L2 depth | Trades | Clock |
|---|---|---|---|---|
| `synthetic` | yes | yes | yes | simulated, ns |
| `journal` | as recorded | as recorded | as recorded | as recorded, ns |
| `csv` | as written | as written | as written | as written, ns |
| `binance` | yes | **no** | yes | venue transaction time, ms |
| `tardis` | yes | yes | yes | exchange time, µs |

Depth is the part that decides what a run can conclude. The queue model sets an order's queue position from the displayed quantity at its price, so a source without depth tells the simulator nothing about any price except the touch: an order resting one tick behind the best quote is modelled as alone at its price and fills the instant a trade reaches it. Quote at the touch on such a source, or read the fill counts knowing that the "behind touch" ones are optimistic ([Backtesting](../explanation/backtesting.md#what-the-simulator-cannot-tell-you)).

## `journal`

The market data of an `.fmj` journal: book deltas and snapshots, trades and tickers, as recorded, in recorded order. `fastmm-data convert` writes one from any source.

| Option | Default | Meaning |
|---|---|---|
| `path` (1st positional) | — | The journal |
| `strip_own` | `false` | Take the recording session's own resting orders out of the depth and tickers |

A live session's journal holds the venue's feed, which shows the session's own orders. A backtest over it sees them as someone else's liquidity: a strategy that improves the best bid improves on its own live bid. `strip_own=1` subtracts from each book level what the session had resting at that price at the message's venue time, from the journal's own order events (ack to cancel ack or last fill, venue time); a level left with nothing is deleted. A throttled depth update that still shows an order already cancelled is stripped as of its own time, so the leftover does not come back. A ticker whose best bid or ask was only ours is dropped: the ticker cannot tell the next level, and the depth book carries the top. Public trades are kept, our own fills included: the aggressor existed either way and, without our order, would have traded with the next order at that price. The run's report ends with a line counting the levels and tickers changed. The option refuses a journal without a TSC calibration (a backtest's; its simulated feed never showed its orders).

## `binance`

Daily dumps from [data.binance.vision](https://data.binance.vision): `bookTicker` (best bid and ask after every change) and `aggTrades` (every trade, aggregated per aggressing order and price). Fetch them with `python3 -m fastmm.data fetch` ([How-to](../how-to/backtesting/binance-public-data.md)).

| Option | Default | Meaning |
|---|---|---|
| `symbol` (1st positional) | — | `BTCUSDT` |
| `date` (2nd positional) | — | `YYYY-MM-DD`; or `from=` and `to=` for a range |
| `market` | `um` | `um` USDⓈ-M futures, `spot` (trades only: the archive has no spot book) |
| `dir` | `$FASTMM_DATA_HOME` | Cache root |
| `book` / `trades` | `true` | Which streams to read |
| `start` / `end` | whole days | `HH:MM[:SS]` or `YYYY-MM-DDTHH:MM[:SS]`, UTC |
| `clock` | `transaction` | `transaction` (matching engine) or `event` (stream publication) |
| `inst` / `venue` | resolved by symbol | Instrument and venue id the events carry |

`bookTicker` exists for USDⓈ-M futures between 2023-05-16 and 2024-03-30 and nowhere else; the archive stopped publishing it. `aggTrades` runs to yesterday for both markets.

Decoding: the first update becomes a one-level `BookSnapshot`, every later one a `BookDelta` carrying only what changed (a price move is the old level deleted and the new one added). Rows that repeat the previous top of book with a new update id produce nothing. Both files stamp milliseconds, so a trade and the book update it caused routinely share a timestamp; the merge puts the trade first, so it consumes the queue before the level is recorded as smaller.

**Licence.** The datasets are [CC BY-NC-SA 4.0](https://data.binance.vision/Binance_Vision-Terms_of_Use.pdf) (Binance Vision Dataset Terms, clause 3.1). Clause 4.1 allows "algorithmic historical backtesting for purely personal non-production research"; clause 4.2 forbids using them for "live proprietary trading execution"; clause 4.5 requires any redistributed derivative to keep the same licence and attribute Binance Vision. That is why no sample ships in this repository: ShareAlike and the non-commercial clause do not mix with an MIT tree.

## `tardis`

Normalized datasets from [datasets.tardis.dev](https://datasets.tardis.dev): `incremental_book_L2` (a full book snapshot then one row per level change) and `trades`. The first day of every month is free for every exchange and symbol; any other day needs `$TARDIS_API_KEY`.

| Option | Default | Meaning |
|---|---|---|
| `exchange` (1st positional) | — | `binance-futures`, `binance`, `bitmex`, … |
| `symbol` (2nd positional) | — | `BTCUSDT` |
| `date` (3rd positional) | — | `YYYY-MM-DD`; or `from=` and `to=` |
| `dir` | `$FASTMM_DATA_HOME` | Cache root |
| `book` / `trades` | `true` | Which datasets to read |
| `start` / `end` | whole days | `HH:MM[:SS]` or a full timestamp, UTC |
| `clock` | `exchange` | `exchange` (the venue's own stamp) or `local` (collector receipt) |
| `inst` / `venue` | resolved by symbol | Instrument and venue id the events carry |

Depth is truncated to the 256 best levels per side, which is the widest message the simulator moves. Rows sharing a timestamp form one message; the feed has no update id.

**Licence.** The [Tardis terms](https://docs.tardis.dev/legal/terms-of-service) grant a perpetual licence to keep and use downloaded data, including free samples (clause 9.4), for internal, research or personal use. Clause 9.2 forbids redistributing the raw data; only derived data aggregated to 10 minutes or coarser may be passed on. So: download it, back-test on it, do not commit it anywhere.

## Others considered

| Source | Why not (yet) |
|---|---|
| IEX HIST DEEP pcap | The friendliest terms of the lot — free, no registration, redistributable with the attribution line in the [HIST data terms](https://www.iex.io/legal/hist-data-terms), aggregated depth at every price level back to 2016. It needs a pcap reader and an IEX-TP/DEEP decoder, which [ADR-0014](../adr/0014-us-equities.md) plans; daily files are over 10 GB gzipped |
| Kaiko | No free tier; data is licensed per enterprise agreement |

## Adding a source

One file and one registration. The source parses its own options, so nothing is added to `include/fastmm/config/schema.hpp`.

```cpp
#include "fastmm/backtest/data_registry.hpp"

namespace {
std::unique_ptr<fastmm::bt::MdSource> open_mine(const fastmm::bt::DataSourceOptions& o) {
  o.reject_unknown({"path", "venue"});                     // a typo fails here
  return std::make_unique<MySource>(std::string(o.require("path")),
                                    fastmm::VenueId{static_cast<std::uint8_t>(
                                        o.get_int("venue", 0))});
}
}  // namespace

void register_my_source() {
  fastmm::bt::DataSourceRegistry::instance().try_add(
      {.name = "mine",
       .summary = "my venue's tape",
       .options = "path=<file>  venue=<id>",
       .positional = {"path"},
       .caps = {.top_of_book = true, .depth = true, .trades = true,
                .clock = "exchange time, nanoseconds"},
       .open = &open_mine});
}
```

`MySource` implements `fastmm::sim::MdSource`: `next()` yields `BookSnapshot` / `BookDelta` / `Trade` / `BookTicker` messages in non-decreasing time order and returns `nullptr` at the end, `reset()` rewinds, `start_ts()` reports the first event's time. `CsvLineReader` (`backtest/binance_source.hpp`) streams a file too large to hold in memory, and `RowAssembler` (`backtest/data_source.hpp`) turns a row stream into messages. `ChainSource` concatenates days and `MergedSource` merges streams by time, lower index first on a tie.

Declare the capabilities honestly: they are what `fastmm-data list` prints and what tells a reader of a result whether its fills mean anything.

Call the registration before opening data — from an app's `main`, or from `register_builtin_data_sources()` for a source in this tree.

## Packing a source for replay

Decoding text is the slow part of a run. `fastmm-data convert` writes any source to an `.fmj` journal ([Journal format](journal-format.md)), which replays the same events with no parsing:

```bash
./build/release/bin/fastmm-data convert --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
```

On 30 minutes of BTCUSDT `bookTicker` + `aggTrades` (252,760 events) that is 0.77 s from the CSVs against 0.06 s from the journal, with identical results and the same outbound hash. The journal is not smaller than the CSV; it is already decoded.

From Python, `fastmm.convert_data(spec, out, config)` does the same thing.
