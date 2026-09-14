# Monitoring a live session

`fastmm-live` publishes its state for external monitors without touching the engine thread: the
engine refreshes a seqlocked copy of its counters, PnL, kill-switch state and latency percentiles
every second, and the control thread copies that and every venue's status into a small
memory-mapped file every 250 ms. The file is `/dev/shm/fastmm-<engine name>.status` unless
`--status <path>` is given; `--no-status` turns it off.

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml &
./build/release/bin/fastmm-top --name binance-demo          # or --path <status file>
```

`fastmm-top` redraws the dashboard in place (`--interval <ms>`, default 500; `--no-color`;
`--once` prints one frame, useful in scripts). It shows:

- the session: engine and strategy name, session id, pid, dry-run flag, uptime and the state
  (starting, running, stopping, stopped). A running engine that has not published for more
  than 3 seconds is shown as `STALE`, which usually means the process died;
- engine counters: events, book updates, orders, cancels, replaces, fills, risk rejects, kill
  switch trips and flags, realised and unrealised PnL and fees;
- engine latency per interval as count, p50, p99 and max. `decode` is the network thread's parse
  time; `book_apply` runs from the end of decoding on the network thread to the book update on the
  engine thread, so it includes the hand-off through the SPSC ring (with `spin_mode = "adaptive"`
  that is where an idle engine's wake-up shows); `strategy`, `serialize` and `send` are engine
  thread only; `tick_to_trade` and `wire_to_book` start at the socket read;
- per venue: market-data, user and order channel states, synced books, market-data messages,
  resyncs, orders, cancels, order events, reconnects, REST errors, clock offset and the network
  thread's wire tick-to-trade p50.

The file stays after the session ends, so the last frame shows `stopped` with the final numbers.
The layout is versioned (magic number and version field); a monitor built from a different
version refuses to read it instead of showing garbage.
