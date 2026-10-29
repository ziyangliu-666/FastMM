# Add a venue

Write a connector for a new JSON-over-WebSocket exchange, register it, and prove it with the same tests the shipped connectors pass. Throughout, `foo` stands for your venue. Prerequisites: a debug or release build of this repository and the venue's API documentation.

Model it on a shipped connector under `include/fastmm/venues/`:

| Connector | Directory | Shows |
|---|---|---|
| Bybit v5 spot and linear | `bybit/` | the main worked example |
| Binance Spot | `binance/` | REST snapshots, Ed25519, SBE market data |
| Binance USDⓈ-M | `binance_usdm/` | reusing another connector's signing and feed |
| OKX v5 | `okx/` | a third credential, contracts as quantities |
| Deribit | `deribit/` | JSON-RPC, request credits, options data |
| Coinbase | `coinbase/` | orders over REST, a user channel, ES256 JWTs |

Binary formats live in `codecs/` ([FIX](../../reference/codecs/fix.md), [Nasdaq](../../reference/codecs/nasdaq.md), [CME MDP 3.0](../../reference/codecs/cme-mdp3.md)), and a connector can build on them, as `nasdaq_itch` does. FIX and MDP 3.0 build only with `-DFASTMM_CODEC_FIX=ON` or `-DFASTMM_CODEC_MDP3=ON` ([Optional codecs](../../getting-started/install.md#optional-codecs)); turn the option on in the change that adds the venue.

## 1. Before you start

A connector is one control-path class implementing `fastmm::venues::Venue` (`venue.hpp`). It owns three hot-path objects, all driven from the venue's own network thread.

| Method | Runs on | Rule |
|---|---|---|
| `load_reference_data()`, `attach()`, `subscribe()` | main thread, once, before any network thread | blocking REST is allowed |
| `connect()`, `disconnect()`, `on_timer()`, `on_wake()`, `poll()`, `resync_books()`, `request_open_orders()`, `request_executions()` | the venue's reactor thread | `poll()` runs after every iteration; `spin_mode = "busy"` needs it |
| `send_now()` | the engine thread, with `threading = "single"` | encode and write inline ([Run to completion](../../explanation/architecture.md#run-to-completion)) |
| `cancel_all()` | any thread, even with the reactor stuck | use its own blocking connection (`BlockingHttp`) |

The market-data sink is lossy: when its ring is full the delta is dropped and the book must resync. The order sink never drops: it spins, then calls its overflow callback, and `fastmm-live` shuts down. Nothing on the hot path allocates, throws or makes a virtual call.

Read [Venue connectors](../../reference/venues.md) for what the shipped connectors do, and its [list of venue behaviours](../../reference/venues/binance-spot.md#what-a-binance-compatible-simulator-must-implement) your connector must handle.

## 2. Lay out the files

Mirror the Bybit connector's names:

| File | Contents |
|---|---|
| `include/fastmm/venues/bybit/bybit_auth.hpp` | `Credentials` and `Signer`: REST header and WebSocket auth signatures |
| `bybit_md_parser.hpp`, `src/venues/bybit/bybit_md_parser.cpp` | `BybitMdParser::decode()`: one public frame into one normalised message |
| `bybit_md_feed.hpp` | `BybitMdFeed`: owns the parser and one book sync per instrument |
| `bybit_book_sync.hpp` | `BybitBookSync`: `BookSyncer<BybitSyncTraits>` plus resubscribe on gaps |
| `bybit_order_encoder.hpp`, `.cpp` | `BybitOrderEncoder` (WebSocket and REST requests) and `BybitResponseDecoder` |
| `bybit_private_parser.hpp`, `.cpp` | order, execution and wallet topics into order events |
| `bybit_rest_decoder.hpp`, `.cpp` | reference data and server time |
| `bybit_error_map.hpp` | `map_error()`: `retCode` to `RejectReason` and `VenueAction` |
| `bybit_venue.hpp`, `.cpp` | `BybitVenueConfig`, `BybitVenue`, `make_bybit_config()` |
| `src/venues/bybit/bybit_registration.cpp` | the registry entry ([step 10](#10-register-it)) |

Other connectors differ in a few files:

| Connector | Difference |
|---|---|
| Binance | `binance_user_parser.hpp` for the private stream; `binance_depth_sync.hpp`, snapshot from REST |
| OKX | `api_passphrase`; `instIdCode` in WebSocket orders; `contract_multiplier` = `ctVal`; level text kept for the checksum in `okx_book_sync.hpp` |
| Deribit | `deribit_json.hpp` parses JSON numbers with exponents exactly; `deribit_credits.hpp` holds the request credits |
| Coinbase | orders over REST; feeds number messages themselves (`coinbase_book_sync.hpp`); `advanced_auth.hpp` signs ES256 JWTs; `coinbase_wire.hpp` maps client ids to UUIDs |
| Binance USDⓈ-M | reuses Spot signing, feed and depth sync; `binance_usdm_venue.hpp` lists what is shared |

Do not copy plumbing; these headers in `include/fastmm/venues/` already hold it:

| Header | What it gives you |
|---|---|
| `connector_common.hpp` | `map_conn_state()`, `channel_state()`, `header_int()`, `origin_of()`, `url_root()`, `IdText`, `load_published_status()`, `trip_venue_kill_once()`, `VenueExtras` |
| `book_sync.hpp` | `StreamBookSync<Traits>` for a venue that sends its snapshot on the stream; write the traits, not the syncer |
| `order_events.hpp` | `emit_order_ack()`, `emit_order_reject()`, `emit_cancel_ack()`, `emit_cancel_reject()`, `emit_replayed_fill()`, `emit_funding()`, `emit_connection_state()`, `emit_venue_kill()` |
| `order_commands.hpp` | `OrderCommand`, `SentWatermark`, `is_reconcile_request()` |
| `wire_latency.hpp` | `BatchedOrders`, `drain_outbound_coalesced()` |
| `dead_mans_switch.hpp` | `CountdownSwitch`, the refresh clock for a venue-side countdown cancel |

New `.cpp` files under `src/venues/` and tests under `tests/venues/` are globbed, so they need no CMake edits.

## 3. Parse market data

Every feed satisfies the `MarketDataFeed` concept in `feed.hpp`:

```cpp
template <class F>
concept MarketDataFeed = requires(F& f, std::string_view json, std::int64_t rx_ts) {
  { f.on_message(json, rx_ts) } -> std::same_as<ParseStatus>;
  { f.on_connected() };
  { f.on_disconnected() };
  { f.subscription_payloads() } -> std::convertible_to<std::span<const std::string>>;
};
```

End your feed's header with `static_assert(MarketDataFeed<FooMdFeed>);`, as `bybit_md_feed.hpp` does. The parser decodes one frame into a caller-provided buffer, and the feed writes the message into the sink.

| `ParseStatus` | Use it for |
|---|---|
| `Ok` | a message was produced |
| `Ignored` | a valid frame you do not consume: subscription acks, pongs, heartbeats |
| `Malformed` | JSON error, missing or invalid field; counted in `md_malformed` |
| `UnknownSymbol` | a symbol not in the `SymbolTable` |
| `Overflow` | the ring is full or the buffer too small; counted in `md_dropped` |
| `Error` | a venue error payload |

Rules for the parser:

- Produce `BookSnapshotMsg`, `BookDeltaMsg`, `TradeMsg`, `BookTickerMsg` and, for options, `OptionTickerMsg`. Reserve a `BookDeltaMsg` in the sink or buffer; never construct one by value.
- Parse prices and quantities exactly with `decimal.hpp` (`Fixed::from_decimal`), never through `double`. For JSON numbers, parse the raw token as `deribit_json.hpp` does.
- simdjson needs 64 readable bytes after the payload. WebSocket frames have them; wrap test fixtures in `PaddedJson`.
- Look symbols up in `SymbolTable`, which folds case without allocating.

## 4. Synchronise the book

`BookSyncer<Traits, Sink>` is the snapshot-and-delta state machine; you supply the traits.

| Member | Meaning |
|---|---|
| `kNeedsRestSnapshot` | the snapshot comes from REST (`request_snapshot()` is called) |
| `kBuffersDeltas` | deltas wait until the snapshot arrives |
| `is_stale(delta, snapshot_id)` | the delta is older than the snapshot and is dropped |
| `first_applies(delta, snapshot_id)` | the first delta after the snapshot bridges it |
| `next_applies(delta, prev_id)` | the delta continues the chain; otherwise it is a gap |
| `is_snapshot_marker(delta)` | the venue signals a reset |

The sink receives `on_snapshot()`, `on_delta()`, `on_resync(SyncReason)` and `request_snapshot()`. The shipped rules:

| Traits | Rule |
|---|---|
| `BinanceSpotSyncTraits` | REST snapshot with `lastUpdateId`; buffered deltas chained on `U`/`u` |
| `BinanceFuturesSyncTraits` | REST snapshot; the first delta brackets `lastUpdateId`, later ones chain on `pu` |
| `BybitSyncTraits` | snapshot in the stream; `u` strictly increasing; `u == 1` resets |
| `DeribitSyncTraits` | first notification is the snapshot; `prev_change_id` equals the previous `change_id` |
| `OkxSyncTraits` | snapshot in the stream; `prevSeqId` equals the previous `seqId` |
| `GeminiSyncTraits` | the first frame after subscribing is the snapshot; `U` ≤ last `u` < `u` |

On a gap, emit `ConnState::Resyncing` on channel 0 with `emit_connection_state()` and fetch a new snapshot. Rate-limit it: Bybit, Deribit and OKX resubscribe at most every 2 s per instrument, and again after 10 s without a snapshot. Any state other than `Live` on channel 0 makes the engine clear the book and pull that venue's quotes.

A market-data channel silent for `stale_ms` reports `Stale`; after `dead_ms` it reconnects. Private and order channels are often quiet, so they report no `Stale`, and their `dead_ms` sits above the keepalive interval ([Configuration keys](../../reference/venues.md#configuration-keys)).

## 5. Send orders

1. The engine writes `OutNewOrderMsg`, `OutCancelMsg` and `OutReplaceMsg` into the outbound ring and wakes the venue.
2. `on_wake()` drains the ring with `drain_outbound_coalesced()`, wrapping each message in an `OrderCommand` (`New`, `Cancel`, `Replace`). Cork the order connection for the drain, so all orders leave in one write.
3. Check the client-side rate limiter, then encode into a fixed buffer with `JsonWriter`, which never allocates and refuses to send a truncated request.
4. Name requests `<kind><client order id>` with `request_id.hpp`: kind `n`, `c`, `r`, or `a` for an amend.
5. Map post-only to the venue's flag: Binance `LIMIT_MAKER`, Bybit `PostOnly`, Deribit `post_only` with `reject_post_only`.
6. When a request cannot be sent (rate limit, fatal state, dry run), emit a reject with `emit_order_reject()`; otherwise the OMS waits forever.
7. Record `order_encode` and `order_send` latency in `VenueStatus`. In a drain, every order's send stamp is the return of the one write.

The ring can also carry a reconcile request (`is_reconcile_request(h)`), which the engine sends after a cancel is refused more than three times; answer it with `request_open_orders()`. The quote manager replaces quotes only when `VenueCaps::supports_replace`, the venue's `supports_replace` and `[engine] supports_replace` are all true; otherwise it sends cancel and new.

## 6. Sign requests

`net/crypto.hpp` provides HMAC-SHA256, Ed25519 and ECDSA P-256 with base64url for ES256 JWTs. Sign the exact bytes you send; Bybit signs timestamp, key, receive window and the query string or body.

Signed requests carry a timestamp and `recv_window_ms`. Measure the clock offset from the venue's server-time endpoint at start-up and after a timestamp error, and log it above 1000 ms.

## 7. Map the errors

Map every documented error code to an `ErrorMapping` (`error_action.hpp`): a `RejectReason` for the engine, a `VenueAction` for the connector, and whether the code is known. Follow Bybit's `constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept`, and cite the venue's documentation next to each code.

| `VenueAction` | Connector behaviour |
|---|---|
| `None` | report the reject only |
| `Backoff` | pause sending for 1 s |
| `RateLimit` | pause until the venue's reset time, log `rate limited (...)` |
| `ResyncClock` | fetch the server time and recompute the offset |
| `Reconcile` | `request_open_orders()` |
| `DisableInstrument` | log `venue rejected a precision/filter rule (...)`; the config is wrong for the symbol |
| `HardStop` | stop all REST (IP ban), log `REST hard stop (...)`, kill this venue with `VenueHardStop` |
| `Fatal` | refuse every further order, log `fatal venue error (...)`, kill this venue once with `VenueFatal` via `trip_venue_kill_once()` |

## 8. Parse the private stream and reconcile

| Event | Emit with | Rule |
|---|---|---|
| ack, reject, cancel ack, cancel reject | `order_events.hpp` helpers | |
| fill | `OrderFillMsg` | `fee` in units of `fee_asset` (`Quote`, `Base`, `Other`), classified against the instrument; base-asset fees go into the position |
| expiry, position | `OrderExpiredMsg`, `PositionUpdateMsg` | |
| funding | `emit_funding()` | carries the venue's payment id; the engine books each id once |
| balance | `emit_balance()` | absolute amounts, filtered by `reconcile_.assets()`, stamped with the venue's time ([Balance check](../../explanation/risk-model.md#balance-check)) |
| replayed execution | `emit_replayed_fill()` | the venue's execution id, `OrderFillMsg::kReplayed` |

Ignore events for client ids that are not FastMM's. For balances, pass the instrument table to `ReconcileDriver::attach` and implement `ReconcileHooks::fetch_balances(generation)`; without a balance stream, call `request_balances()` after fills.

`request_open_orders()` is a reconciliation:

1. If the venue lists executions (`caps.executions`), replay them since the last replay.
2. Emit `ReconcileMsg` `Begin` with `kSentWatermark`, plus `kExecutionsExact` when every instrument's replay answered in full.
3. Emit one `OpenOrder` per live order on the session's symbols, other programs' and manual ones too, with `venue_order_id` set and `cl_ord_id` decoded by `decode_cl_ord_id` or left invalid. A connector that drops the client id makes its own orders look foreign to `cancel_unknown = "own"`.
4. Emit `Position` if the venue reports one, then `End`.

The engine pauses quoting from `Begin` to `End` and cancels live orders it does not know.

| When | Reconcile |
|---|---|
| first connect of the private channel | with an empty watermark, to sweep orders a dead process left |
| every private or order-channel reconnect | yes; not on a return from `Stale` |
| once a minute while healthy | `request_executions()`, through `ReplayScheduler`, skipping instruments without order activity |
| order channel lost, `cancel_on_order_channel_loss = true` | cancel everything over REST |

If the venue offers a dead man's switch, arm it: a countdown refreshed with `CountdownSwitch`, as on Binance USDⓈ-M, or cancel-on-disconnect, as on Deribit and Bybit DCP.

## 9. Write the venue class

Subclass `Venue` and implement:

| Method | Does |
|---|---|
| `load_reference_data()` | fetch instruments; set tick, lot, quantity limits, minimum notional, and option strike, expiry, type and size; disable non-trading ones; fail on a missing required symbol unless `allow_offline_reference_data` |
| `attach()` | keep the symbol table, instruments, both sinks and the outbound ring |
| `connect()`, `disconnect()`, `subscribe()` | open the channels; Bybit uses one handler per channel inside a `ConnectionSlot<Handler>`, which picks TCP for `ws://` and TLS for `wss://`, with `net::BackoffConfig` for reconnects |
| `on_timer()` | keepalives, pings, clock-offset refresh, snapshot retries; called about once a second |
| `on_wake()`, `poll()`, `send_now()` | drain the outbound ring; one non-blocking pass for `busy`; inline send for `single` |
| `request_open_orders()`, `cancel_all()`, `request_executions(since_venue_ms)` | steps 1 and 8 |
| `resume_executions(since_venue_ms, known)` | where the first replay starts after a restart; with increasing trade ids also `resume_trade_ids()` and `resume_known_trade_ids()`; without `caps.executions` no position is restored |
| `resync_books()` | start every book over; `fastmm-gateway` calls it when its copies are lost |
| `refused_account_settings()` | true after a setting a retry will not fix, such as hedge mode, so the exit code is 3, not 4 |
| `status()` | fill `VenueStatus`, which the log and `fastmm-top` show |

REST on the reactor thread goes through `RestChannel`. Orders, cancels, cancel-all and dead man's switch refreshes take `RestLane::Orders`, a connection of their own, so they never wait behind a snapshot. A venue that needs requests to arrive in signing order, like Gemini's nonce, keeps them on one lane. Honour `--record-raw` with `RawRecorder`. In a dry run, open market data only and refuse orders.

## 10. Register it

A connector is one entry in the venue registry: its `kind`, capabilities, keys and factory. Put it in `src/venues/foo/foo_registration.cpp`:

```cpp
#include "fastmm/venues/foo/foo_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {
namespace {

// The keys this venue owns. The registry validates them before the factory runs, and
// tools/docs_config_ref.py turns this table into the connector's table in the configuration
// reference: say what the key does, its unit and its default.
constexpr VenueKeySpec kFooKeys[] = {
    {"stale_ms", KeyType::Int, false, "no traffic for this long pulls the quotes, ms (default 2000)"},
    {"order_api", KeyType::String, false, "order entry: ws (default) | rest"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  foo::FooVenueConfig c = foo::make_foo_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<foo::FooVenue>(id, std::move(c));
}

}  // namespace

void register_foo_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "foo",
                           .summary = "Foo spot (testnet)",
                           .keys = kFooKeys,
                           .caps = {.credentials = true, .order_entry = true,
                                    .replace = true, .positions = true, .polls = false,
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
```

Then add two lines to `src/venues/registry.cpp`: a declaration of `register_foo_venue` and a call in `register_builtin_venues()`. Nothing self-registers, because the linker drops a static library's self-registering object.

| Field | Meaning |
|---|---|
| `name` | the `[venues.<name>] kind` value that selects this connector |
| `aliases` | other `kind` values (`binance_spot` answers to `binance` and `sim`) |
| `summary` | one line for the connector table in [Configuration](../../reference/configuration.md#connectors) |
| `keys` | the section keys the venue owns beyond the generic ones |
| `caps` | `credentials`, `market_data_keys`, `order_entry`, `replace`, `positions`, `position_snapshot`, `polls`, `executions` |
| `make` | the factory, called after the keys are validated |

The core reads `caps` instead of testing `kind`. For example, `credentials = false` is why `nasdaq_itch` is never asked for a key, and `executions = true` lets a restart restore the position. `caps` says what the connector can do; `Venue::caps()` says what this instance does, given a dry run or missing credentials.

The central schema knows only the generic keys: `kind`, the URLs, the credentials, `testnet`, `supports_replace`, `insecure_tls`, `ca_file`, `recv_window_ms` and `fees`. Every other key reaches the venue in `VenueSection::extra` and is checked against `kFooKeys`: an unknown, mistyped or missing key stops the session, naming the line. Values only the connector can judge, such as a URL or a range, are `make_foo_config`'s job; throw `std::invalid_argument` with a message starting `venues.<name>.<key>:`.

### Document it

1. Add a section to [Configuration](../../reference/configuration.md) with the two markers, then run `python3 tools/docs_config_ref.py`:

    ```markdown
    #### `foo`

    <!-- BEGIN config-keys venue:foo -->
    <!-- END config-keys -->
    ```

2. Add a section in [Venue connectors](../../reference/venues.md), `configs/foo-testnet.toml`, the key variables in `.env.example`, an environment in [Run on a testnet](../operations/run-on-testnet.md) and a CHANGELOG entry.

## 11. Keep it outside FastMM

A connector can live in its own project. `examples/external-venue/` builds one against the installed headers: a connector, its registration, a live app and a test.

```cpp
// apps/live.cpp: fastmm-live plus this project's connector.
#include "echo/echo_venue.hpp"
#include <fastmm/cli/live.hpp>

int main(int argc, char** argv) {
  echo::register_echo_venue();
  return fastmm::cli::live(argc, argv);
}
```

```cmake
find_package(fastmm CONFIG REQUIRED COMPONENTS live)
add_library(echo_venue STATIC src/echo_venue.cpp src/register.cpp)
target_link_libraries(echo_venue PUBLIC fastmm::venues)
add_executable(echo-live apps/live.cpp)
target_link_libraries(echo-live PRIVATE echo_venue fastmm::live)
```

`kind = "echo"` selects it, and its own declaration validates its keys. Build it against an install prefix, or a source tree with `-DFASTMM_SOURCE_DIR=<checkout>`. This repository's `ctest` also compiles the two files into `fastmm_venues_tests` (`venues.external.*`).

## 12. Record fixtures

Once market data works, record public frames from the testnet:

```bash
./build/release/bin/fastmm-live --config configs/foo-testnet.toml --dry-run --duration 60s \
    --record-raw tests/fixtures/foo/raw
```

Each channel is appended to `<dir>/<venue>-<channel>.jsonl`, one frame per line after the receive timestamp and a tab.

1. Cut single messages into `tests/fixtures/foo/*.json`.
2. Remove keys, account ids and order ids that identify an account.
3. Describe each file in `tests/fixtures/foo/fixtures.meta.json` as `recorded`, `synthesised from ...` or `docs-example (<url>)`, with source and date, as in `tests/fixtures/bybit/fixtures.meta.json`.

Private payloads you cannot record yet come from the venue's documentation examples; say so in the meta file and the CHANGELOG.

## 13. Test it

Name the tests after the Bybit ones; `fastmm_venues_tests` picks them up with labels `unit` and `fixture`:

| Test file | Proves |
|---|---|
| `tests/venues/foo_md_parser_test.cpp` | fixtures in, expected messages out, including malformed, unknown-symbol and ignored frames; a recorded session syncs without a resync |
| `foo_book_sync_test.cpp` | snapshot first, stale and out-of-order deltas, gaps leading to a rate-limited resync |
| `foo_order_encoder_test.cpp` | golden requests, signatures against an independent implementation, response decoding, the error map |
| `foo_private_parser_test.cpp` | every order status, fills with the commission asset, positions, foreign client ids |
| `foo_venue_test.cpp` | the whole connector against `FakeVenueServer`: config, reference data, subscribe, orders, reconciliation, channel loss, blocking `cancel_all()` |
| `live_foo_test.cpp` | opt-in testnet check, skipped unless `FASTMM_LIVE_TESTS=1`; name cases `live.foo: ...` for the `live` label |
| `tests/hotpath/venues_noalloc_test.cpp` | no allocation in the parsers and encoder after warm-up (label `noalloc`) |
| `tests/core/book_syncer_test.cpp` | new sync traits, if you add them to `book_syncer.hpp` |

```bash
cmake --build --preset release -j --target fastmm_venues_tests
ctest --preset release -R 'foo\.'
```

Add the parsers to `bench/bench_json.cpp`, the encoder to `bench/bench_order_encoders.cpp`, and a p50 budget for each in `bench/ci_budget.toml`.

## 14. Check conformance

A venue is done when each item has an equivalent test.

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
| The registry resolves the `kind` and builds the connector | [`registry_test.cpp`](../../../tests/venues/registry_test.cpp) "venues.registry: make_venue builds the connector its kind names" |
| Reference data, subscribe, orders, reconciliation and `cancel_all()` against a fake exchange | [`bybit_venue_test.cpp`](../../../tests/venues/bybit_venue_test.cpp) "bybit.venue: scripted fake exchange end to end" |
| Private-channel loss cancels over REST, re-authenticates and reconciles | [`deribit_venue_test.cpp`](../../../tests/venues/deribit_venue_test.cpp) "deribit.venue: scripted fake exchange end to end" |
| Invalid credentials are fatal; the request limit refuses orders | [`deribit_venue_test.cpp`](../../../tests/venues/deribit_venue_test.cpp) "deribit.venue: invalid credentials are fatal and the credit limit refuses orders" |
| A dry run opens market data only and refuses orders | [`binance_venue_test.cpp`](../../../tests/venues/binance_venue_test.cpp) "binance.venue: dry run opens market data only and refuses orders" |
| Client-side rate limits and cooldowns | [`rate_limiter_test.cpp`](../../../tests/venues/rate_limiter_test.cpp) |
| The real testnet: book sync, place and cancel | [`live_bybit_test.cpp`](../../../tests/venues/live_bybit_test.cpp) "live.bybit: testnet book sync, then place and cancel a far limit order" |
| The engine quotes, fills and shuts down cleanly over the network (Binance protocol only) | [`e2e_test.cpp`](../../../tests/integration/e2e_test.cpp), against `fastmm-sim-exchange` |

Then run the connector on its testnet with tight `[risk]` limits ([Run on a testnet](../operations/run-on-testnet.md)).
