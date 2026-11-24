# Schedule windows

A `[[windows]]` table pulls quotes or holds positions reduce-only at the same local time every week, or on given dates, in the time zone of the market it follows. The engine opens and closes each window itself; daylight saving time and new dates need no edits. Every key is in [Configuration](../../reference/configuration.md#windows).

## Declare a weekly window

Hold the whole session reduce-only over the Sunday reopen of the US futures markets, 19:55 to 20:30 New York time:

```toml
[[windows]]
name = "weekly-reopen"
zone = "America/New_York"
days = ["sun"]
start = "19:55"
duration = "35m"
action = "reduce_only"
```

In March the window opens at 23:55 UTC, in January at 00:55 UTC. A cron entry in UTC, or a dated line, would be an hour off for half the year.

Pull one group of instruments over the US cash open on weekdays, except on market holidays:

```toml
[[windows]]
name = "cash-open"
zone = "America/New_York"
days = ["mon", "tue", "wed", "thu", "fri"]
start = "09:29"
end = "09:35"
instruments = ["equities"]          # a [strategy.groups] group
except = ["2026-12-25", "2027-01-01", "2027-01-18"]
```

## Declare dated windows

A data release has a date list instead of `days`. The time stays local, so a release at 08:30 New York is written as 08:30 whatever the date:

```toml
[[windows]]
name = "us-cpi"
zone = "America/New_York"
dates = ["2026-12-10", "2027-01-13", "2027-02-11"]
start = "08:25"
duration = "15m"
venues = ["binance_usdm"]
```

`from` and `until` bound a weekly window to a range of dates, for a schedule that changes on a known day.

## Check the schedule

The configuration is checked when it loads: a zone that is not in the build's tzdata, a time such as `"7pm"`, a date such as `"2026-02-30"`, or a scope that names no venue or instrument stops the program with the key's line. Run a backtest or `fastmm-live --dry-run` on the file to see each window open and close in the log:

```text
window weekly-reopen opens: reduce_only the session
window weekly-reopen closes: reduce_only the session
```

While a session runs, `fastmm-top` and `fastmm-ctl status` show each window, whether it is open, and when it next changes, in UTC:

```text
windows    weekly-reopen  reduce_only  OPEN  closes 2026-11-30 01:30Z (in 21m14s)
           us-cpi  pull  closed  opens 2026-12-10 13:25Z (in 239h55m02s)
```

A side the window pulls shows `window` as its obstacle in the quote table. Prometheus has `fastmm_window_open{window,action}` and `fastmm_window_next_edge_seconds{window,action}` ([Monitor with fastmm-top](monitor-with-fastmm-top.md#scrape-it-with-prometheus)).

## Work with the operator commands

A window and the operator's commands add up. `fastmm-ctl resume` and `fastmm-ctl reduce-only off` leave an open window in force, and a window that closes leaves an operator's pull in force ([Operate a running session](operate-a-running-session.md)). To lift a window early, remove it from the file and restart.

## Replace a dated schedule

A schedule kept as dated lines, read by a strategy or by a timer that calls `fastmm-ctl`, becomes one `[[windows]]` table per recurring event:

1. Group the lines by event and write each group's local time and zone, not its UTC time.
2. Write the recurring ones with `days` and the one-off ones with `dates`.
3. Backtest the new file over a week that crosses a daylight-saving change and compare the window lines in the log with the old schedule.
4. Remove the old lines or timer once the windows run live.

A backtest or replay of the session evaluates the windows from its configuration, so a journal recorded before a window was added replays without it.

## Time zone data

The IANA tzdata the engine uses is compiled into the build; `status` shows its release (`tzdata 2026e` in the JSON as `"tzdata"`). A replay on another machine opens a window at the same events as the live session. When a government changes a zone's rules, the change reaches the engine with a FastMM release that carries the new tzdata. Until then, a POSIX rule gives the new offsets: `zone = "<-03>3"`.
