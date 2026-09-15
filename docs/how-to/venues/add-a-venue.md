# Add a venue

Model a JSON-over-WebSocket connector on the three that ship: Binance Spot (`include/fastmm/venues/binance/`), Bybit v5 spot (`include/fastmm/venues/bybit/`) and Deribit options and futures (`include/fastmm/venues/deribit/`). Bybit is the main worked example; Deribit shows JSON-RPC, request credits and options data. Binary protocols (FIX, ITCH/OUCH, SBE) are codecs instead; see [FIX](../../reference/codecs/fix.md), [Nasdaq](../../reference/codecs/nasdaq.md) and [CME MDP 3.0](../../reference/codecs/cme-mdp3.md).

Throughout, `foo` stands for your venue.

## 1. Before you start

A connector is one control-path class that implements `fastmm::venues::Venue` (`include/fastmm/venues/venue.hpp`) and owns the hot-path objects: a market-data feed, a private (user) stream parser and an order encoder, all driven from the venue's own network thread.

The threading contract, from `venue.hpp`:

- `load_reference_data()` and `attach()` run once on the main thread before any network thread starts; blocking REST calls are allowed there. `fastmm-live` also calls `subscribe()` once during that setup.
- `connect()`, `disconnect()`, `on_timer()`, `on_wake()` and `request_open_orders()` run on the venue's reactor thread.
- `cancel_all()` must work from any thread, including while the reactor thread is stuck: use an independent blocking REST connection (`BlockingHttp`, `include/fastmm/venues/blocking_http.hpp`).
- Two sinks carry events to the engine (`include/fastmm/venues/event_sink.hpp`). The market-data sink is lossy: when its ring is full the delta is dropped and the book must resync. The order sink never drops: it spins, then calls its overflow callback, and `fastmm-live` shuts down.
- Nothing on the hot path allocates, throws or calls a virtual function; the virtual `Venue` interface is control path only.

Read [Venue connectors](../../reference/venues.md) for what the three connectors do today, and its section [What a Binance-compatible simulator must implement](../../reference/venues.md#what-a-binance-compatible-simulator-must-implement) as a list of venue behaviours your connector must handle.

## 2. File layout

The Bybit connector, with the file names a new venue should mirror:

| File | Contents |
|---|---|
| `include/fastmm/venues/bybit/bybit_auth.hpp` | `Credentials` and `Signer`: REST header and WebSocket auth signatures |
| `include/fastmm/venues/bybit/bybit_md_parser.hpp`, `src/venues/bybit/bybit_md_parser.cpp` | `BybitMdParser::decode()`: one public frame into one normalised message |
| `include/fastmm/venues/bybit/bybit_md_feed.hpp` | `BybitMdFeed`: the `MarketDataFeed`; owns the parser and one book sync per instrument |
| `include/fastmm/venues/bybit/bybit_book_sync.hpp` | `BybitBookSync`: `BookSyncer<BybitSyncTraits>` plus resubscribe on gaps |
| `include/fastmm/venues/bybit/bybit_order_encoder.hpp`, `src/venues/bybit/bybit_order_encoder.cpp` | `BybitOrderEncoder` (WebSocket and REST requests) and `BybitResponseDecoder` (order responses, open orders) |
| `include/fastmm/venues/bybit/bybit_private_parser.hpp`, `src/venues/bybit/bybit_private_parser.cpp` | `BybitPrivateParser`: order, execution and wallet topics into order events |
| `include/fastmm/venues/bybit/bybit_rest_decoder.hpp`, `src/venues/bybit/bybit_rest_decoder.cpp` | Reference data and server time |
| `include/fastmm/venues/bybit/bybit_error_map.hpp` | `map_error()`: `retCode` to `RejectReason` and `VenueAction` |
| `include/fastmm/venues/bybit/bybit_venue.hpp`, `src/venues/bybit/bybit_venue.cpp` | `BybitVenueConfig`, `BybitVenue`, `make_bybit_config()` |

Differences in the other connectors:

- Binance names its private parser `binance_user_parser.hpp` and its book sync `binance_depth_sync.hpp` (the snapshot comes from REST).
- Deribit adds `deribit_json.hpp` (exact parsing of JSON numbers with exponents) and `deribit_credits.hpp` (`CreditBucket`, the matching-engine request credits).

`src/venues/CMakeLists.txt` globs `src/venues/**/*.cpp` and the test targets glob `tests/venues/*.cpp`, so new files need no CMake edits; re-run `cmake --preset release` so the glob sees them.

## 3. Market data

### The feed contract

`include/fastmm/venues/feed.hpp` defines the concept every feed satisfies:

```cpp
template <class F>
concept MarketDataFeed = requires(F& f, std::string_view json, std::int64_t rx_ts) {
  { f.on_message(json, rx_ts) } -> std::same_as<ParseStatus>;
  { f.on_connected() };
  { f.on_disconnected() };
  { f.subscription_payloads() } -> std::convertible_to<std::span<const std::string>>;
};
```

All three feeds check it at compile time with a `static_assert` at the end of their header, for example `static_assert(MarketDataFeed<BybitMdFeed>);` in `bybit_md_feed.hpp`. Do the same.

Underneath, the parser decodes one frame into a caller-provided buffer, for example `MdDecodeResult BybitMdParser::decode(std::string_view json, Timestamp recv_ts, Cycles t0, std::span<std::byte> out) noexcept`, and the feed writes the message into the sink.

### Parse status

Return the `ParseStatus` that says what happened; the feed counts `Malformed` into `VenueStatus::md_malformed` and ring overflows into `md_dropped`:

| Status | Use it for |
|---|---|
| `Ok` | A message was produced |
| `Ignored` | A valid frame you do not consume: subscription acks, pongs, heartbeats |
| `Malformed` | JSON error, or a missing or invalid field |
| `UnknownSymbol` | A symbol that is not in the `SymbolTable` |
| `Overflow` | The ring is full or the output buffer is too small |
| `Error` | A venue error payload |

### Messages and parsing rules

- Produce `BookSnapshotMsg` and `BookDeltaMsg`, `TradeMsg`, `BookTickerMsg` and, for options, `OptionTickerMsg` (`include/fastmm/core/messages.hpp`). Never construct a `BookDeltaMsg` by value; reserve it in the sink or the output buffer (CONTRIBUTING rule 8).
- Parse every price and quantity string exactly with `include/fastmm/venues/decimal.hpp` (`Fixed::from_decimal`, no `double`). If the venue sends JSON numbers, parse the raw token as Deribit does in `deribit_json.hpp`.
- simdjson needs 64 readable bytes after the payload (`kJsonPadding`). WebSocket frames have them; in tests and benchmarks wrap fixtures in `PaddedJson` (`include/fastmm/venues/padded_json.hpp`).
- Look symbols up in `SymbolTable` (`include/fastmm/venues/symbology.hpp`); lookups fold case without allocating.

## 4. Book synchronisation

`BookSyncer<Traits, Sink>` (`include/fastmm/core/book/book_syncer.hpp`) is the venue-independent snapshot and delta state machine. A venue supplies a traits struct:

| Member | Meaning |
|---|---|
| `kNeedsRestSnapshot` | The snapshot comes from REST (`request_snapshot()` is called) instead of the stream |
| `kBuffersDeltas` | Deltas are buffered until the snapshot arrives |
| `is_stale(delta, snapshot_id)` | The delta is older than the snapshot and is dropped |
| `first_applies(delta, snapshot_id)` | The first delta after the snapshot bridges it |
| `next_applies(delta, prev_id)` | The delta continues the chain; otherwise it is a gap |
| `is_snapshot_marker(delta)` | The venue signals a reset |

The sink receives `on_snapshot()`, `on_delta()`, `on_resync(SyncReason)` and `request_snapshot()`. The shipped rules:

| Traits | Rule |
|---|---|
| `BinanceSpotSyncTraits` (`book_syncer.hpp`) | REST snapshot with `lastUpdateId`; buffered deltas chained on `U`/`u` |
| `BinanceFuturesSyncTraits` (`book_syncer.hpp`) | REST snapshot; the first delta brackets `lastUpdateId`, later deltas chained on `pu` (Binance USDⓈ-M) |
| `BybitSyncTraits` (`book_syncer.hpp`) | Snapshot in the stream; `u` strictly increasing; `u == 1` is a reset marker |
| `DeribitSyncTraits` (`include/fastmm/venues/deribit/deribit_book_sync.hpp`) | First notification is the snapshot; `prev_change_id` equals the previous `change_id` |

On a gap, the connector emits `ConnectionStateMsg` with `ConnState::Resyncing` on channel 0 (`emit_connection_state()` in `include/fastmm/venues/order_events.hpp`) and fetches a new snapshot, rate limited: Bybit and Deribit resubscribe at most once every 2 s per instrument and again when no snapshot arrives within 10 s. Any state other than `Live` on channel 0 makes the engine clear the book and pull the quotes of that venue's instruments.

A channel with no traffic for `stale_ms` reports `ConnState::Stale`; after `dead_ms` the connection is closed and reopened. Defaults are 2000 ms and 10000 ms (Binance), 2000 ms and 30000 ms (Bybit), 10000 ms and 30000 ms (Deribit); for quiet testnet feeds see [Venue connectors](../../reference/venues.md#configuration-keys).

## 5. Order entry

`feed.hpp` also defines an `OrderGateway` concept (`encode(cmd, now_ms, out) -> std::size_t`, `on_message(json, rx_ts) -> ParseStatus`, `can_send(now_ns) -> bool`), but none of the shipped encoders is checked against it: each exposes the calls its venue needs. Binance and Bybit have `encode_ws()` and `encode_rest()`; Deribit has `DeribitOrderEncoder::encode(const OrderCommand& cmd, const OrderShadow* shadow, std::string_view access_token, std::span<char> out)`. Follow the pattern rather than the concept:

1. The engine writes `OutNewOrderMsg`, `OutCancelMsg` and `OutReplaceMsg` into the outbound ring and wakes the venue. `on_wake()` drains the ring and wraps each message in an `OrderCommand` (`include/fastmm/venues/order_commands.hpp`, kinds `New`, `Cancel`, `Replace`).
2. It checks the client-side rate limiter (`include/fastmm/venues/rate_limiter.hpp`; Deribit's `CreditBucket`), then encodes into a fixed buffer with `JsonWriter` (`include/fastmm/venues/json_writer.hpp`), which never allocates and refuses to send a truncated request.
3. Request ids are `<kind><client order id>` (`include/fastmm/venues/request_id.hpp`: kind `n`, `c` or `r`, 15 characters).
4. Post-only maps to the venue's flag: Binance `LIMIT_MAKER`, Bybit `timeInForce` `PostOnly`, Deribit `post_only` with `reject_post_only`.
5. Replace is used only when both `VenueCaps::supports_replace` and the config's `supports_replace` are true; otherwise the quote manager sends cancel and new.
6. When a request cannot be sent (rate limit, fatal state, dry run), emit a reject to the order sink with `emit_order_reject()` so the OMS never waits for an answer that will not come.

Record encode and send latency in `VenueStatus::order_encode` and `order_send` (`include/fastmm/venues/wire_latency.hpp`).

## 6. Authentication

Use `include/fastmm/net/crypto.hpp` for HMAC-SHA256 and Ed25519 (Binance `key_type = "ed25519"`). Sign the exact bytes you send: Bybit signs timestamp, key, receive window and the query string or body. Signed requests carry a timestamp and a receive window (`recv_window_ms`), so measure the clock offset from the venue's server-time endpoint at startup and whenever the venue reports a timestamp error, and log it when it exceeds 1000 ms.

## 7. Error map

Map every documented venue error code to an `ErrorMapping` (`include/fastmm/venues/error_action.hpp`): a `RejectReason` for the engine, a `VenueAction` for the connector and whether the code is known. The signature to follow is Bybit's `constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept`. Cite the venue's documentation next to each code.

What each action does in the shipped connectors (`BybitVenue::apply_action()`):

| `VenueAction` | Connector behaviour |
|---|---|
| `None` | Report the reject only |
| `Backoff` | Pause sending for 1 s |
| `RateLimit` | Pause until the venue's reset time, count a cooldown, log `rate limited (...)` |
| `ResyncClock` | Fetch the server time and recompute the offset |
| `Reconcile` | `request_open_orders()` |
| `DisableInstrument` | Log `venue rejected a precision/filter rule (...)`; the config is wrong for the symbol |
| `HardStop` | Stop all REST requests (IP ban) |
| `Fatal` | Refuse every further order on the venue, log `fatal venue error (...)` and, once, send the engine `ControlCommand::TripVenueKill` with `emit_venue_kill(sink, venue, KillReason::VenueFatal)` (`venues/order_events.hpp`) so that it kills this venue only |

## 8. Private stream and reconciliation

- Emit acknowledgements, rejects, cancel acknowledgements and cancel rejects with the helpers in `include/fastmm/venues/order_events.hpp`, and `OrderFillMsg`, `OrderExpiredMsg` and `PositionUpdateMsg` directly.
- Every fill carries `fee` in units of `fee_asset` (`FeeAsset::Quote`, `Base` or `Other`); the engine books base-asset commission into the position (Binance charges BTC on buys). Classify the commission asset against the instrument's base and quote.
- Ignore events for client ids that are not FastMM's (orders placed by hand or by other software).
- `request_open_orders()` emits `ReconcileMsg` `Begin` (with `kSentWatermark` set), one `OpenOrder` per live order (and `Position` if the venue reports one), then `End`. Call it after every private or order-channel reconnect; the engine pauses quoting from `Begin` to `End` and cancels live orders it does not know.
- On order-channel loss with `cancel_on_order_channel_loss = true`, cancel everything over REST.

## 9. The venue class

Subclass `Venue` and implement:

- `load_reference_data()`: fetch instruments and overwrite tick, lot, minimum and maximum quantity and minimum notional (and, for options, strike, expiry, type and contract size). Disable instruments that are not trading, and fail on a required symbol that is missing unless `allow_offline_reference_data` is set.
- `attach()`: keep the symbol table, instruments, both sinks and the outbound ring.
- `connect()`, `disconnect()`, `subscribe()`: open the channels. Bybit uses one handler struct per channel (`on_state`, `on_text`, `on_binary`, `on_connected_send_subscriptions`) inside a `ConnectionSlot<Handler>` (`include/fastmm/venues/connection_slot.hpp`), which picks plain TCP for `ws://` and TLS for `wss://`; reconnect backoff comes from `net::BackoffConfig` (`include/fastmm/net/backoff.hpp`).
- `on_timer()`: keepalives and application pings, clock-offset refresh, snapshot retries. The backend calls it about once a second.
- `on_wake()`: drain the outbound ring (section 5).
- `request_open_orders()` and `cancel_all()` (sections 1 and 8).
- `status()`: fill `VenueStatus`; `fastmm-live` logs it every second and `fastmm-top` shows it.
- Control-path REST on the reactor thread goes through `RestChannel` (`include/fastmm/venues/rest_channel.hpp`). Honour `--record-raw` with `RawRecorder` (`include/fastmm/venues/raw_recorder.hpp`).
- In a dry run, open market data only and refuse orders (`VenueCaps::user_stream = false`).

## 10. Registration

Connectors are registered in code, in four places:

1. `include/fastmm/venues/venue_factory.hpp`: add `Foo` to `enum class VenueKind`.
2. `src/venues/venue_factory.cpp`: map the `kind` strings in `venue_kind()`, add a `case` to `make_venue()` that builds your config with `make_foo_config(const VenueSection&, bool dry_run)` and sets `record_raw_dir`, and add the kind to the "unsupported kind" message.
3. `include/fastmm/config/schema.hpp`: add the kind to the `kind` description, and declare every connector-specific key as a passthrough entry (`{"venues.*", "<key>", KeyType::Int, false, "<meaning>", true}`), which the loader hands to the connector in `VenueSection::extra` without an "unknown key" warning.
4. Documentation: the kind and keys in [Configuration](../../reference/configuration.md), the kind table and a section in [Venue connectors](../../reference/venues.md), a `configs/foo-testnet.toml`, the key variables in `.env.example`, an environment section in [Run on a testnet](../operations/run-on-testnet.md) and a CHANGELOG entry.

## 11. Fixtures

Record public frames from the venue's testnet with the connector itself once market data works:

```bash
./build/release/bin/fastmm-live --config configs/foo-testnet.toml --dry-run --duration 60s --record-raw tests/fixtures/foo/raw
```

Each channel is appended to `<dir>/<venue>-<channel>.jsonl`, one frame per line prefixed with the receive timestamp and a tab. Cut single messages out into `tests/fixtures/foo/*.json`, remove keys, account ids and order ids that identify an account, and describe every file in `tests/fixtures/foo/fixtures.meta.json` as `recorded`, `synthesised from ...` or `docs-example (<url>)`, with the source URL and recording date (see `tests/fixtures/bybit/fixtures.meta.json`). Private payloads you cannot record yet come from the venue's documentation examples; say so in the meta file and in the CHANGELOG.

## 12. Tests

Name the tests after the Bybit ones; they are picked up by the `fastmm_venues_tests` binary (labels `unit` and `fixture`):

| Test file | What it proves |
|---|---|
| `tests/venues/foo_md_parser_test.cpp` | Fixture in, expected messages out, including malformed, unknown-symbol and ignored frames, and a recorded session that syncs without a resync |
| `tests/venues/foo_book_sync_test.cpp` | Snapshot first, stale and out-of-order deltas, gaps leading to a rate-limited resync |
| `tests/venues/foo_order_encoder_test.cpp` | Golden request strings, signatures against an independent implementation, response decoding and the error map |
| `tests/venues/foo_private_parser_test.cpp` | Every order status, fills with the commission asset, positions, foreign client ids |
| `tests/venues/foo_venue_test.cpp` | The whole connector against `FakeVenueServer` (`tests/venues/fake_venue_util.hpp`): config mapping, reference data, subscribe, order round trip, reconciliation, channel loss and the blocking `cancel_all()` |
| `tests/venues/live_foo_test.cpp` | Opt-in testnet check with `tests/venues/live_test_util.hpp`: skipped unless `FASTMM_LIVE_TESTS=1`. Name the test cases `live.foo: ...`; the `venues.live.` prefix gives them the ctest label `live` instead of `unit` and `fixture` |
| `tests/hotpath/venues_noalloc_test.cpp` | A test case for the market-data parser, the private parser and the order encoder: after a warm-up pass over the fixtures, no allocation (binary `fastmm_hotpath_tests`, label `noalloc`) |
| `tests/core/book_syncer_test.cpp` | New sync traits, if you add them to `book_syncer.hpp` |

Run them:

```bash
cmake --build --preset release -j --target fastmm_venues_tests
ctest --preset release -R 'foo\.'
```

Benchmarks: add the parsers to `bench/bench_json.cpp` and the order encoder to `bench/bench_order_encoders.cpp`, then a p50 budget for each in `bench/ci_budget.toml`.

## 13. Conformance checklist

Your venue is done when each item has an equivalent test.

| Item | Proven by |
|---|---|
| The feed satisfies `MarketDataFeed` | `static_assert` in [`bybit_md_feed.hpp`](../../../include/fastmm/venues/bybit/bybit_md_feed.hpp) |
| Recorded snapshot and delta parse into exact messages | [`bybit_md_parser_test.cpp`](../../../tests/venues/bybit_md_parser_test.cpp) "bybit.md_parser: recorded orderbook.50 snapshot and delta" |
| Malformed, unknown-symbol and control frames get the right `ParseStatus` | [`bybit_md_parser_test.cpp`](../../../tests/venues/bybit_md_parser_test.cpp) "bybit.md_parser: publicTrade aggressor from S, control frames, errors" |
| A recorded session syncs with no resync | [`bybit_md_parser_test.cpp`](../../../tests/venues/bybit_md_parser_test.cpp) "bybit.md_feed: recorded session syncs the book with no resync" |
| Deltas before the snapshot are dropped | [`bybit_book_sync_test.cpp`](../../../tests/venues/bybit_book_sync_test.cpp) "bybit.book_sync: snapshot first, deltas dropped before it" |
| A gap or reset marker resyncs and resubscribes | [`bybit_book_sync_test.cpp`](../../../tests/venues/bybit_book_sync_test.cpp) "bybit.book_sync: u == 1 marker and a stale u resync and resubscribe" |
| A missing snapshot is retried | [`bybit_book_sync_test.cpp`](../../../tests/venues/bybit_book_sync_test.cpp) "bybit.book_sync: no snapshot within the timeout resubscribes" |
| A full market-data ring forces a resync | [`binance_depth_sync_test.cpp`](../../../tests/venues/binance_depth_sync_test.cpp) "binance.depth_sync: full market-data ring forces a resync" |
| Signatures match an independent implementation | [`bybit_order_encoder_test.cpp`](../../../tests/venues/bybit_order_encoder_test.cpp) "bybit.auth: signatures match an independent HMAC implementation" |
| New, cancel and replace requests are byte-exact | [`bybit_order_encoder_test.cpp`](../../../tests/venues/bybit_order_encoder_test.cpp) "bybit.encoder: order.create / order.cancel / order.amend WS frames" |
| REST requests sign the bytes sent | [`bybit_order_encoder_test.cpp`](../../../tests/venues/bybit_order_encoder_test.cpp) "bybit.encoder: REST requests sign the exact bytes sent" |
| Every documented error code maps to a reason and an action | [`bybit_order_encoder_test.cpp`](../../../tests/venues/bybit_order_encoder_test.cpp) "bybit.error_map: documented retCodes" |
| Order statuses map to order events | [`bybit_private_parser_test.cpp`](../../../tests/venues/bybit_private_parser_test.cpp) "bybit.private_parser: order topic statuses map to order events" |
| Fills and positions, with the commission asset | [`bybit_private_parser_test.cpp`](../../../tests/venues/bybit_private_parser_test.cpp) "bybit.private_parser: execution -> fill, wallet -> position, control frames"; [`binance_user_parser_test.cpp`](../../../tests/venues/binance_user_parser_test.cpp) "binance.user: the commission asset of a fill is classified as base, quote or other" |
| Foreign client ids are ignored | [`binance_user_parser_test.cpp`](../../../tests/venues/binance_user_parser_test.cpp) "binance.user: foreign client ids, unknown symbol, malformed, ignored events" |
| Config keys map onto the connector config | [`bybit_venue_test.cpp`](../../../tests/venues/bybit_venue_test.cpp) "bybit.venue: config mapping derives the private and trade URLs" |
| The factory builds the connector from its `kind` | [`deribit_md_parser_test.cpp`](../../../tests/venues/deribit_md_parser_test.cpp) "deribit.config: section mapping and factory registration" |
| Reference data, subscribe, orders, reconciliation and `cancel_all()` against a fake exchange | [`bybit_venue_test.cpp`](../../../tests/venues/bybit_venue_test.cpp) "bybit.venue: scripted fake exchange end to end" |
| Private-channel loss cancels over REST, re-authenticates and reconciles | [`deribit_venue_test.cpp`](../../../tests/venues/deribit_venue_test.cpp) "deribit.venue: scripted fake exchange end to end" |
| Invalid credentials are fatal; the request limit refuses orders | [`deribit_venue_test.cpp`](../../../tests/venues/deribit_venue_test.cpp) "deribit.venue: invalid credentials are fatal and the credit limit refuses orders" |
| A dry run opens market data only and refuses orders | [`binance_venue_test.cpp`](../../../tests/venues/binance_venue_test.cpp) "binance.venue: dry run opens market data only and refuses orders" |
| Client-side rate limits and cooldowns | [`rate_limiter_test.cpp`](../../../tests/venues/rate_limiter_test.cpp) |
| The real testnet: book sync, place and cancel | [`live_bybit_test.cpp`](../../../tests/venues/live_bybit_test.cpp) "live.bybit: testnet book sync, then place and cancel a far limit order" |
| The engine quotes, fills and shuts down cleanly over the network (Binance protocol only) | [`e2e_test.cpp`](../../../tests/integration/e2e_test.cpp), against `fastmm-sim-exchange` |

Then run the connector on its testnet with tight `[risk]` limits ([Run on a testnet](../operations/run-on-testnet.md)).
