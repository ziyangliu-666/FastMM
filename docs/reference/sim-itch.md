# fastmm-sim-itch

`fastmm-sim-itch` is a Nasdaq-style simulated exchange for the multicast market-data path ([ADR-0015](../adr/0015-multicast-market-data.md), section 6). It publishes TotalView-ITCH 5.0 over MoldUDP64 to two multicast lines, answers MoldUDP64 re-requests, serves GLIMPSE 5.0 snapshots and accepts OUCH 5.0 orders, all over the protocol codecs in [Nasdaq ITCH and OUCH](codecs/nasdaq.md). It measures wire to wire: from the `sendmmsg` of the datagram that carried a market-data message to the arrival of the order that names it.

```
fastmm-sim-itch ── one thread, one net::Reactor
  ├─ per symbol     MatchingEngine + MarketGenerator, ItchPublisher (engine effects -> ITCH)
  ├─ feed           moldudp::Transmitter -> sendmmsg to line A and line B     (239.192.0.1:31001, .2:31002)
  ├─ UDP            MoldUDP64 re-request server                               (:31000)
  ├─ TCP            GLIMPSE 5.0 over SoupBinTCP                               (:31010)
  └─ TCP            OUCH 5.0 over SoupBinTCP                                  (:31020)
```

Code: `include/fastmm/sim/itch/` and `src/sim/itch/` (targets `fastmm::sim_itch_publisher` and `fastmm::sim_itch`), app `apps/fastmm-sim-itch/`.

## Running

```bash
./build/release/bin/fastmm-sim-itch --config configs/sim-itch.toml
./build/release/bin/fastmm-sim-itch --cpu 2 --busy-poll --duration 60s --summary-json runs/w2w.json
```

Multicast on `lo` needs the loopback interface to carry multicast and a route for 224.0.0.0/4. In an unprivileged namespace:

```bash
unshare -Urn sh -c 'ip link set lo up multicast on && ip route add 224.0.0.0/4 dev lo && \
  ./build/release/bin/fastmm-sim-itch --duration 30s'
```

Flags and exit codes: [Command lines](cli.md#fastmm-sim-itch). Every `--stats-interval` the simulator prints ITCH messages, datagrams built, datagrams sent and dropped per line, re-requests and answers, GLIMPSE snapshots, OUCH logins, orders, rejects, executions, and the wire-to-wire count and misses. At exit it sends End of Session on both lines and prints the wire-to-wire p50, p99, p99.9 and maximum.

## Configuration (`configs/sim-itch.toml`)

The simulator reads `[[instruments]]` (symbol, tick, lot; 1 to 8 characters, whole-share lots, ticks that are multiples of 0.0001; disabled ones are skipped) and `[sim]`. Without `--config` it runs FMAA and FMBB at $100. Command-line flags override the file.

| key | default | meaning |
|---|---|---|
| `seed` | 7 | generator seed |
| `venue` | all | simulate only the instruments of this venue |
| `start_mid`, `symbols.<SYM>.start_mid` | 100 | initial latent mid |
| `symbols.<SYM>.locate` | position + 1 | ITCH Stock Locate |
| `speed` | 1.0 | generator time per wall-clock time |
| `bind` | 127.0.0.1 | address of the re-request, GLIMPSE and OUCH servers |
| `busy_poll` | false | never block waiting for I/O |
| `itch.session` | FMSIM00001 | MoldUDP64 session |
| `itch.line_a`, `itch.line_b` | 239.192.0.1:31001, 239.192.0.2:31002 | `group:port`; `""` disables a line |
| `itch.line_a_drop_rate`, `itch.line_b_drop_rate` | 0 | probability that a data datagram is not sent on the line |
| `itch.drop_seed` | 1 | line i draws from seed + i |
| `itch.interface` | lo | `IP_MULTICAST_IF`: name, IPv4 address, or `""` for the routing table |
| `itch.source` | "" | local address the sending socket binds |
| `itch.ttl`, `itch.loop` | 1, true | `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP` |
| `itch.max_datagram` | 1472 | MoldUDP64 packet size limit |
| `itch.max_messages` | 0 | messages per datagram, 0 = as many as fit |
| `itch.burst` | 32 | datagrams per `sendmmsg`, and the token-bucket depth |
| `itch.packet_rate` | 0 | datagrams per second per line, 0 = unpaced |
| `itch.flush_us` | 0 | hold messages this long to fill datagrams |
| `itch.heartbeat_ms` | 1000 | MoldUDP64 heartbeat after this long without data |
| `itch.history_messages`, `itch.history_bytes` | 1 048 576, 64 MiB | re-request history, a ring |
| `itch.sndbuf_bytes` | 0 | `SO_SNDBUF`, 0 = system default |
| `itch.rerequest_port` | 31000 | UDP; 0 = ephemeral, -1 = off |
| `glimpse.port`, `glimpse.username`, `glimpse.password`, `glimpse.session` | 31010, glimps, glimpse, GLIMPSE | GLIMPSE listener and login |
| `glimpse.max_messages` | 262 144 | largest snapshot |
| `ouch.port`, `ouch.username`, `ouch.password`, `ouch.session` | 31020, fmouch, ouch, OUCH | OUCH listener and login |
| `ouch.history_messages` | 262 144 | outbound messages kept per connection |
| `generator.*` | see the file | `MarketGenerator` parameters, as in [fastmm-sim-exchange](sim-exchange.md#configuration-configssimtoml) |

## Feed

The first `poll()` publishes the opening spin: System Event `O`, Stock Directory `R` per symbol, System Events `S` and `Q`, Stock Trading Action `H` (`T`, trading) per symbol, then the seeded books.

Engine effects become ITCH messages (`ItchPublisher`, `include/fastmm/sim/itch/itch_publisher.hpp`):

| Engine effect | ITCH |
|---|---|
| an order rests | A with its leaves |
| a resting order executes | E (the match number is also in the OUCH Executed message) |
| a resting order is cancelled, or a replace re-enters it | D |
| a replace keeps priority (same price, quantity not above leaves), an OUCH partial cancel | X for the reduction; the order reference is kept |

Order reference numbers and match numbers are unique across symbols. Timestamps are nanoseconds since UTC midnight.

Messages go into a `moldudp::Transmitter` whose history is a ring (`TransmitterConfig::overwrite_oldest`); requests for evicted messages are not answered. Datagrams are sent with one `sendmmsg` per line of up to `burst` datagrams, line A first. A datagram dropped on a line is not sent on it; the re-request server still has it. After `heartbeat_ms` without data both lines get a heartbeat.

## GLIMPSE 5.0

After a SoupBinTCP login the connection receives, as Sequenced Data: Stock Directory for every symbol, Stock Trading Action `T` for every symbol, Add Order for every resting order (bids then asks per symbol, best level first, queue order within a level, under the order's ITCH reference), and End of Snapshot `G` with the sequence number to continue from. The snapshot is built between two engine calls, so it equals the ITCH stream up to that sequence number minus one, whether or not those messages were sent yet. The client logs out after the snapshot; the requested sequence number is ignored.

The client side is `codecs::itch::glimpse::GlimpseClient` ([Nasdaq ITCH and OUCH](codecs/nasdaq.md#glimpse-50)).

## OUCH 5.0

Each OUCH connection is an account (at most 15 at once). Orders are cancelled when the connection closes.

| Inbound | Handling |
|---|---|
| O Enter Order | UserRefNum must increase (otherwise ignored). Unknown symbol: Rejected `0x0017`; price off the tick: `0x001D`; quantity off the lot: `0x0013`. Time in force `3` is IOC, or FOK with MinQty equal to Quantity; any other value rests until cancelled. PostOnly `P` crossing the book: Rejected `0x0012` |
| U Replace Order | as `MatchingEngine::replace` (priority kept for the same price and no larger quantity). A replace of an order that is no longer live is ignored; when the new leg cannot be entered the original is gone and a Canceled follows |
| X Cancel Order | Quantity 0 cancels; a smaller quantity reduces the order in place; a larger one is ignored |
| anything else | ignored |

Outbound, as Sequenced Data: Accepted (Order Reference Number = the ITCH reference), Replaced, Canceled (`U` user, `I` IOC remainder; no self-trade prevention), Executed (`A` added, `R` removed liquidity; the ITCH match number), Rejected.

## Wire to wire

An Enter Order or Replace Order whose ClOrdID is a sequence token is timed:

```
ClOrdID = 'T' + 13 decimal digits, zero padded = the MoldUDP64 sequence number of the ITCH
          message that triggered the order                          "T0000000012345"
```

`codecs::ouch50::put_seq_token()` writes it and `parse_seq_token()` reads it; the `nasdaq_itch` venue with `order_entry = "sim_ouch"` sets it ([Venue connectors](venues.md#orders)). `scripts/bench-e2e.sh` runs the simulator against `fastmm-live` ([Benchmarks](../explanation/benchmarks.md#end-to-end-over-veth)). The simulator stamps every data datagram with `rdtscp` right before the `sendmmsg` call that first carries it, keeps the last `stamp_ring` stamps (default 1 048 576 datagrams) with the sequence range of each datagram, and stamps each TCP read with `rdtscp` right after it returns. For an order with a token the difference, converted with the calibrated TSC rate, goes into a `LogLinearHistogram` before the order is processed. A token outside the stamp ring counts as a miss.

`--summary-json <file>` writes one JSON object at exit:

| Key | Meaning |
|---|---|
| `wire_to_wire_ns` | `count`, `min`, `p50`, `p90`, `p99`, `p999`, `max`, `mean` of the histogram, in ns |
| `tokens`, `misses` | Enter and Replace Orders with a sequence token; tokens not found in the stamp ring |
| `messages`, `packets` | ITCH messages published, data datagrams built |
| `datagrams_sent`, `datagrams_dropped` | per line, `[A, B]` |
| `send_errors` | datagrams `sendmmsg` did not take |
| `requests`, `requests_answered` | MoldUDP64 re-requests |
| `snapshots`, `orders`, `executions` | GLIMPSE snapshots, OUCH orders entered, OUCH executions |

Percentiles are bucket upper bounds of the histogram (6.25 % resolution).

## Differences from Nasdaq

* One MoldUDP64 session per run; no end of day. System Event `E` and `C` are never sent.
* No crosses, halts, NOII, Reg SHO or MPID attribution. The order messages are A, E, X and D.
* GLIMPSE sends no System Event and no Reg SHO state.
* OUCH: no Modify Order, Mass Cancel, order-entry controls or Account Query; no firm or capacity checks. Day and GTX orders rest until cancelled.
* All orders are displayed.

## Tests

`tests/integration/sim_itch_test.cpp` runs the simulator in-process in an unprivileged user and network namespace (skipped with a message where none can be created):

* both lines drop 5 % of datagrams: a `KernelDatagramSource` + `moldudp::Receiver` client recovers every gap through re-requests, and the book rebuilt from ITCH equals the simulator's books (every resting order under its reference, side, price and leaves);
* a client joins mid-stream, takes a GLIMPSE snapshot, applies the buffered and live stream from End of Snapshot's sequence number, and its book equals the simulator's books;
* OUCH: Accepted with the ITCH reference, Executed with E on the feed, Replaced, Canceled with D on the feed, Rejected, and the wire-to-wire histogram counts the one order with a token.
