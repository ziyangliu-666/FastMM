# Market-data sources

A backtest reads market data through a named source. `fastmm-backtest --data <spec>`, `[backtest] source` and `run_backtest(data=...)` resolve the same spec through one registry.

```bash
./build/release/bin/fastmm-data list
```

## Spec syntax

```text
<name>[:<arg>[,<arg>...]]      arg := <key>=<value> | <positional>
```

| Rule | Example |
|---|---|
| Only the first `:` separates the name, so a value may contain one | `start=09:30` |
| An argument without `=` fills the source's next positional option | `binance:BTCUSDT,2024-03-27` |
| A bare path with a known extension names its source | `runs/session.fmj` is `journal:runs/session.fmj` |
| Specs separated by `;` are merged by event time, ties to the earlier spec | several venues in one run |
| An unknown option is an error that lists the source's options | |

```bash
--data synthetic
--data binance:BTCUSDT,2024-03-27
--data binance:symbol=BTCUSDT,date=2024-03-27,start=13:00,end=14:00
--data tardis:binance-futures,BTCUSDT,2026-09-01
--data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
--data "binance:BTCUSDT,2024-03-27,venue=0; csv:bybit-btcusdt.csv,venue=1"
```

Python takes a list of specs as well. Merging feeds of several venues: [Backtesting](../explanation/backtesting.md#several-venues).

At the end of a run, a warning names each configured instrument the data had no events for, and counts events for instrument ids the configuration lacks.

## What each source carries

| Source | Top of book | L2 depth | Trades | Clock |
|---|---|---|---|---|
| `synthetic` | yes | yes | yes | simulated, ns |
| `journal` | as recorded | as recorded | as recorded | as recorded, ns |
| `csv` | as written | as written | as written | as written, ns |
| `binance` | yes | no | yes | venue transaction time, ms |
| `tardis` | yes | yes | yes | exchange time, µs |
| `itch` | yes | yes, and every order | yes | exchange time, ns |

Without depth, an order one tick behind the touch is modelled as alone at its price, so its fills are optimistic ([model assumptions](../explanation/backtesting.md#model-assumptions)).

## `journal`

The market data of an `.fmj` journal, as recorded and in recorded order: book deltas and snapshots, trades and tickers. `fastmm-data convert` writes one from any source.

| Option | Default | Meaning |
|---|---|---|
| `path` (1st positional) | — | The journal |
| `strip_own` | `false` | Take the recording session's own resting orders out of the depth and tickers |
| `parts` | `true` | After `x.fmj`, read the later parts `x.1.fmj`, `x.2.fmj`, ... as one stream; `0` reads one file |
| `remap` | `true` | Give events the ids and venues of the configuration's instruments of the same symbol; `false` keeps the recorded ids |

### Parts

A session recorded with `[engine] journal_max_bytes` is several complete journals ([rotation](journal-format.md#rotation-and-retention)). `journal:x.fmj` reads them in order until one is missing.

* Repeated prologues are not events, so nothing plays twice.
* A part whose first sequence number does not follow the previous part's last is read, with a warning in the log and the report.
* A file numbered as the next part but recorded by another session is an error.
* Do not list the parts yourself (`"x.fmj;x.1.fmj"`): `x.fmj` already reads `x.1.fmj`, so it would play twice.
* `fastmm-replay` reads a market-data journal's parts the same way, and verifies a session journal one part at a time.

### `remap`

A journal numbers instruments as the recording session did. Read with another configuration, or merged with a recording of other instruments, its events would land on the wrong instrument.

| Case | Result |
|---|---|
| Symbol in the configuration | the instrument of that symbol, the recording's venue first, else any venue listing it |
| Symbol not in the configuration | event dropped, counted in the run's report |
| Positions, checkpoints, latencies, balances and flows | renumbered the same way; an account takes the venue of the first mapped instrument on it |
| Configuration numbers instruments as the journal does | events unchanged |
| Recorded snapshot deeper than 256 levels a side | copied whole |

`remap` runs after `strip_own` and needs the instrument table; a spec opened without one refuses `remap=1`.

### `strip_own`

A live journal's feed shows the session's own orders, so a backtest over it would quote against its own live bid. `strip_own=1` removes them:

| Data | Treatment |
|---|---|
| Book level | minus what the session had resting at that price at the message's venue time; a level left empty is deleted |
| Order sent before the journal's first record | known from the first reconciliation that lists it, removed from the start |
| Throttled depth update still showing a cancelled order | stripped as of its own time |
| Ticker whose best bid or ask was only ours | dropped; the depth book carries the top |
| Public trades, our fills included | kept |
| Book tickers a backtest's simulated venue sent (journal format 4 and older) | dropped |

The run's report ends with a line counting the levels and tickers changed. A backtest journal from format 5 records our quantity in separate `OwnLevels` records, so the option leaves it unchanged. It refuses a journal whose session never saw its orders in the feed: no TSC calibration (not live) and no header bit 2.

## `binance`

Daily dumps from [data.binance.vision](https://data.binance.vision): `bookTicker` (best bid and ask after every change) and `aggTrades` (trades aggregated per aggressing order and price). Fetch them with `python3 -m fastmm.data fetch` ([how-to](../how-to/backtesting/binance-public-data.md)).

| Option | Default | Meaning |
|---|---|---|
| `symbol` (1st positional) | — | `BTCUSDT` |
| `date` (2nd positional) | — | `YYYY-MM-DD`; or `from=` and `to=` for a range |
| `market` | `um` | `um` USDⓈ-M futures, or `spot` (trades only) |
| `dir` | `$FASTMM_DATA_HOME` | Cache root |
| `book` / `trades` | `true` (`book`: `false` for `spot`) | Which streams to read |
| `start` / `end` | whole days | `HH:MM[:SS]` or `YYYY-MM-DDTHH:MM[:SS]`, UTC |
| `clock` | `transaction` | `transaction` (matching engine) or `event` (stream publication) |
| `inst` / `venue` | resolved by symbol | Instrument and venue id the events carry |

| Dataset | Available |
|---|---|
| `bookTicker` | USDⓈ-M futures, 2023-05-16 to 2024-03-30 only |
| `aggTrades` | USDⓈ-M and spot, to yesterday |

* The first update becomes a one-level `BookSnapshot`; each later one a `BookDelta` of what changed.
* A row that repeats the previous top of book produces nothing.
* A trade and the book update it caused often share a millisecond; the trade goes first.

Licence: [CC BY-NC-SA 4.0](https://data.binance.vision/Binance_Vision-Terms_of_Use.pdf). Clause 4.1 allows personal non-production backtesting, clause 4.2 forbids live proprietary trading use, and clause 4.5 keeps the licence on derivatives. No sample ships in this repository.

## `tardis`

Normalised datasets from [datasets.tardis.dev](https://datasets.tardis.dev): `incremental_book_L2` (a snapshot, then one row per level change) and `trades`. The first day of each month is free; other days need `$TARDIS_API_KEY`.

| Option | Default | Meaning |
|---|---|---|
| `exchange` (1st positional) | — | `binance-futures`, `binance`, `bitmex`, … |
| `symbol` (2nd positional) | — | `BTCUSDT` |
| `date` (3rd positional) | — | `YYYY-MM-DD`; or `from=` and `to=` |
| `dir` | `$FASTMM_DATA_HOME` | Cache root |
| `book` / `trades` | `true` | Which datasets to read |
| `start` / `end` | whole days | `HH:MM[:SS]` or a full timestamp, UTC |
| `clock` | `exchange` | `exchange` (the venue's stamp) or `local` (collector receipt) |
| `inst` / `venue` | resolved by symbol | Instrument and venue id the events carry |

* Snapshots keep the 256 best levels a side; a larger delta goes out as several messages with one timestamp.
* Rows sharing a timestamp form one message; the feed has no update id.
* With `start`, earlier rows build the book, which goes out as one `BookSnapshot` at the first row at or after `start`.

Licence: the [Tardis terms](https://docs.tardis.dev/legal/terms-of-service) allow keeping and using downloaded data for internal, research or personal use (clause 9.4). Raw data may not be redistributed; only aggregates of 10 minutes or coarser (clause 9.2).

## `itch`

A day of Nasdaq TotalView-ITCH 5.0: every displayed order added, executed, cancelled and replaced, in matching-engine order. Nasdaq publishes days at [emi.nasdaq.com/ITCH](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/); gunzip the file first (about 8.6 GB a day, memory-mapped).

| Option | Default | Meaning |
|---|---|---|
| `path` (1st positional) | — | The file, a 2-byte big-endian length before each message |
| `date` | from the file name | `YYYY-MM-DD`; Nasdaq's names start `MMDDYYYY` |
| `start` / `end` | the whole file | `HH:MM[:SS]`, New York time |
| `venue` | `0` | The venue whose instruments the source follows, by symbol |
| `depth` | `64` | Levels per side in the engine's book, 1 to 256 |
| `max_orders` | `262144` | Resting orders per book |

| Event | What the engine and the simulated venue get |
|---|---|
| Messages of one ITCH timestamp | one `BookDelta` per followed book, from its [L3 book](codecs/nasdaq.md#l3-book) |
| Printable execution | a `Trade` at the execution price |
| P and Q trades | a `Trade` with `TradeMsg::kNoAggressor` |
| Add, execute, cancel, replace | `OrderAddL3` and the like, to the simulated venue only |

Under `fill_model = "l3_queue"` the order events give each of our orders its place in its level's queue ([Backtesting](../explanation/backtesting.md#the-queue-from-every-order)); `l2_queue` ignores them.

Books build from the file's first message. With `start`, earlier messages are applied but not yielded, and the run begins with each book's resting orders, then its snapshot. Times are midnight New York plus the message timestamp; `[backtest] latency_md_us` delays them.

```bash
fastmm-backtest --config configs/backtest-itch.toml --data "itch:07302019.NASDAQ_ITCH50,start=09:30,end=16:00"
```

A full day of eight active stocks, books built from 04:00, runs in about 11 s on one core.

## Others considered

| Source | Status |
|---|---|
| IEX HIST DEEP pcap | Free and redistributable with [attribution](https://www.iex.io/legal/hist-data-terms), depth back to 2016; needs a pcap reader and IEX-TP/DEEP decoder ([ADR-0014](../adr/0014-us-equities.md)) |
| Kaiko | No free tier; licensed per enterprise agreement |

## Adding a source

A source is one file and one registration; it parses its own options, so the configuration schema does not change.

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

| `MdSource` member | Does |
|---|---|
| `next()` | yields `BookSnapshot`, `BookDelta`, `Trade` or `BookTicker` in non-decreasing time; `nullptr` at the end |
| `reset()` | rewinds |
| `start_ts()` | the first event's time |

| Helper | Use |
|---|---|
| `CsvLineReader` (`backtest/binance_source.hpp`) | streams a file too large for memory |
| `RowAssembler` (`backtest/data_source.hpp`) | turns rows into messages |
| `ChainSource` | concatenates days |
| `MergedSource` | merges streams by time, lower index first on a tie |

`fastmm-data list` prints the capabilities. Call the registration before opening data: from an app's `main`, or from `register_builtin_data_sources()` for a source in this tree.

## Packing a source for replay

`fastmm-data convert` writes any `--data` spec, merged ones included, to an `.fmj` journal ([Journal format](journal-format.md)), which replays the same events without parsing text.

```bash
./build/release/bin/fastmm-data convert --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
```

On 30 minutes of BTCUSDT (252,760 events) the run takes 0.06 s from the journal against 0.77 s from the CSVs, with identical results. The journal is not smaller than the CSV. From Python: `fastmm.convert_data(spec, out, config)`.
