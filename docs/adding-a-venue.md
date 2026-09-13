# Adding a venue

A venue is two hot-path types plus one control-path class. Use `include/fastmm/venues/bybit/` as
the worked example.

1. **Market data parser** — `include/fastmm/venues/foo/foo_md_parser.hpp`, a type satisfying the
   `MarketDataFeed` concept (`on_message(text, rx_ts, sink)`, `on_connected(sink)`,
   `subscription_payloads()`). Map venue JSON to `BookSnapshotMsg` / `BookDeltaMsg` / `TradeMsg` /
   `BookTickerMsg`. If the venue needs snapshot+delta alignment, instantiate `BookSyncer` with a
   `SyncTraits` describing its sequence rule.
2. **Order gateway** — `foo_order_encoder.hpp` satisfying `OrderGateway` (`encode(cmd, out)`,
   `on_message(...)` for acks/fills/rejects, `can_send(cmd)`), plus `foo_error_map.hpp` mapping
   venue error codes to `RejectReason`.
3. **Auth** — `foo_auth.hpp` using `fastmm::net::hmac_sha256` etc.
4. **Control path** — `foo_venue.hpp` + `src/venues/foo/foo_venue.cpp`: subclass `Venue`
   (`load_reference_data`, `connect`, `subscribe`, `on_timer`, `request_open_orders`, `cancel_all`).
5. **Register** — add `Foo` to the venue X-macro in `apps/fastmm-live/registrations/` and a
   `[venues.foo]` schema entry in `include/fastmm/config/schema.hpp`.
6. **Fixtures and tests** — record raw frames with `fastmm-live --record-raw` into
   `tests/fixtures/foo/*.json` (sanitised), then add `tests/venues/foo_parser_test.cpp`
   (fixture → expected events), `foo_sync_test.cpp` (gaps → resync), `foo_encoder_test.cpp`
   (golden request strings and signature vectors). Optionally script a fake endpoint with
   `fastmm::net::WsServer` to test the connection FSM.

Binary-protocol venues (FIX, ITCH/OUCH, SBE) implement `Framer` / `Decoder` / `Encoder`
(/ `SessionLayer`) under `include/fastmm/codecs/<proto>/` and plug into the same `Connection`.
