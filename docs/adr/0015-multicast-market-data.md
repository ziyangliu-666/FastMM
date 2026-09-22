# ADR-0015: UDP multicast market data and kernel-bypass receive

Status: proposed (2026-09)

`fastmm-live` receives exchange multicast feeds, starting with Nasdaq TotalView-ITCH 5.0 over MoldUDP64 with GLIMPSE 5.0 for the initial book. Datagrams arrive through one of two backends: kernel UDP sockets or AF_XDP. This record amends ADR-0014, which listed live ITCH as out of scope. Order entry to Nasdaq (OUCH, sponsored access) stays out of scope.

## Context

- Every live venue today is WebSocket over TLS on the kernel TCP stack. T0 is taken in the feed's `on_message`, after the socket read, TLS and WebSocket framing (`binance_md_feed.hpp`).
- What exists:
  - `moldudp::Receiver` detects gaps and sends request packets.
  - `ItchDecoder` turns A/F/E/C/X/D/U into L3 messages and P/Q into `TradeMsg`.
  - Also `L3Book`, `ItchEncoder`, `moldudp::Transmitter`, the SoupBinTCP client and server sessions, and client-side OUCH 5.0 codecs.
- What is missing:
  - Nothing opens a UDP socket.
  - The engine ignores L3 messages (`engine.hpp`, `case EventType::OrderAddL3 ... break`).
  - `ItchDecoder` does not set `t0_cycles`.
- `moldudp::Receiver` has three limits: it drops packets that arrive ahead of a gap, it has no A/B arbitration, and it stamps requests with the time of the last `on_timer`. `Mdp3Feed` does A/B arbitration with a reorder window and recovers from CME's snapshot channel.
- ITCH carries per-order changes, and the stock directory (`R` messages) comes once before the open. A receiver that joins mid-day, or that loses packets it cannot recover, has no book to apply changes to. Nasdaq serves the current state over GLIMPSE (SoupBinTCP): directory, open orders, and an End of Snapshot message with the ITCH sequence number to continue from.
- `L2Book::is_valid()` requires a snapshot (`BookDeltaMsg` with `kSnapshot`) before deltas count (`l2_book.hpp`).
- Kernel-bypass options on Linux:
  - ef_vi and Onload need AMD Solarflare NICs. Onload accelerates unmodified socket code through `LD_PRELOAD`; ef_vi is its own API.
  - DPDK takes the NIC away from the kernel and needs hugepages, a poll-mode driver per NIC family and the EAL. ADR-0007 keeps third-party network libraries out.
  - AF_XDP is in mainline Linux. It runs on any netdev in generic (copy) mode, and without copies on drivers with zero-copy support (mlx5, ice, i40e, ixgbe, igc). An XDP program redirects matching packets to the socket and passes the rest, IGMP and ARP included, to the kernel.
- Development machine (WSL2, Linux 6.6, `hv_netvsc`, MTU 1280):
  - `CONFIG_XDP_SOCKETS` and veth are available.
  - An unprivileged `unshare -Urn` namespace can create veth pairs and enable multicast on `lo`.
  - Unprivileged BPF is disabled, so loading an XDP program needs root.
  - Neither veth nor `hv_netvsc` supports AF_XDP zero-copy or hardware timestamps.

## Decision

### 1. Receive interface

`net::DatagramSource` is a concept, not a virtual class. The net thread calls `poll(handler)`, which delivers a batch of datagrams and returns the count:

```cpp
struct RxMeta {
  Cycles t0_cycles;          // rdtscp when the batch was taken from the kernel or RX ring
  std::int64_t t0_wall_ns;   // CLOCK_REALTIME read next to t0_cycles
  std::int64_t sw_ts_ns;     // kernel RX timestamp (CLOCK_REALTIME), 0 when unavailable
  std::int64_t hw_ts_ns;     // NIC timestamp (PHC time), 0 when unavailable
  std::uint32_t src_ip, dst_ip;
  std::uint16_t dst_port;
  std::uint8_t line;         // index of the subscription (A/B)
};
// handler: void(std::span<const std::byte> payload, const RxMeta&) noexcept
```

- Payload spans are valid only until `poll` returns. Anything kept longer is copied.
- A subscription is `(interface, group, port, source?)`. A and B lines may use different interfaces.
- No backend allocates after `open`.

### 2. Kernel backend (`kernel`)

- One UDP socket per subscription, set up as follows:
  - bound to the group and port with `SO_REUSEADDR`;
  - joined with `IP_ADD_MEMBERSHIP` or `IP_ADD_SOURCE_MEMBERSHIP` on the subscription's interface;
  - `SO_RCVBUF` from config.
- Reads with one `recvmmsg` per socket per `poll`, a fixed batch (default 32). With an edge-triggered reactor the caller polls until `poll` returns 0.
- Turns off `IP_MULTICAST_ALL`, so a socket receives only the groups it joined.
- `SO_TIMESTAMPING` requests software and raw hardware RX timestamps. Hardware timestamps need the NIC set up with `SIOCSHWTSTAMP` (`HWTSTAMP_FILTER_ALL`, `CAP_NET_ADMIN`). That is off by default (`hw_timestamps = true` turns it on).
- In `spin_mode = "busy"`, sets `SO_BUSY_POLL` and `SO_PREFER_BUSY_POLL`.
  - Capabilities: `SO_PREFER_BUSY_POLL` needs `CAP_NET_ADMIN`. So does raising `SO_BUSY_POLL` on Linux before 6.4.
  - Both only take effect with the netdev's `napi_defer_hard_irqs` and `gro_flush_timeout` set.
  - Options the kernel refuses are logged, and the socket polls from user space.
- Runs unmodified under Onload (`onload --profile=latency fastmm-live ...`). This is the way to use Solarflare NICs until an ef_vi backend exists.

### 3. AF_XDP backend (`af_xdp`)

Minimum Linux 5.11. Hand-written over raw syscalls like the io_uring backend, with no libbpf, libxdp or BPF compiler.

- **UMEM**: one per XDP socket, `frame_count` frames of 4096 bytes (default 4096), mmapped at `open`, with fill, RX and completion rings and no TX ring.
- **Sockets**: one XDP socket per `(interface, queue)` pair, and one net thread polls all of them.
  - Multicast lands on the RX queue picked by RSS. Operators steer the groups to the configured queue with `ethtool -N <if> flow-type udp4 dst-ip <group> action <q>`.
- **XDP program**: BPF bytecode kept as an array in the source.
  - It parses Ethernet, one optional 802.1Q tag, IPv4 (IHL masked and bounded for the verifier; fragments pass) and UDP.
  - It looks up `(dst_ip, dst_port, pad = 0)` in a hash map and calls `bpf_redirect_map(xsks_map, rx_queue_index, XDP_PASS)`.
  - A queue with no socket therefore falls back to the kernel instead of dropping; a per-CPU array counts those packets.
  - The map does not check the source address, so `IP_ADD_SOURCE_MEMBERSHIP` filtering does not apply on this path.
  - Unicast traffic, including re-request answers, goes to the kernel.
- **Attach**: through `BPF_LINK_CREATE` with `BPF_XDP`, so the program goes away when the last link fd closes (the link is not pinned).
  - Mode order: native attach with a zero-copy bind, then native attach with a copy bind, then generic attach (`XDP_FLAGS_SKB_MODE`) with a copy bind.
  - `xdp_mode` pins one mode. The chosen mode is logged and exported in status.
  - An interface that already has an XDP program fails `open` with `EBUSY`.
- **IGMP**: a kernel UDP socket per subscription still joins the group so IGMP reports reach the switch. It reads nothing.
- **User-space checks**: the parser repeats the Ethernet/VLAN/IPv4/UDP checks, checks lengths, and counts bad frames.
  - The UDP checksum is not verified by default (`verify_udp_checksum`). Frames from a local sender through veth with TX checksum offload carry only the pseudo-header sum and fail the check.
  - RX descriptors return to the fill ring when `poll` returns.
- **Polling**: uses `XDP_USE_NEED_WAKEUP`.
  - In busy mode, the socket gets `SO_PREFER_BUSY_POLL` and `SO_BUSY_POLL_BUDGET`, and the fill ring is refilled on every poll.
  - In adaptive mode, the reactor waits on the XSK fd.
- **Status**: `XDP_STATISTICS` (`rx_dropped`, `rx_invalid_descs`, `rx_ring_full`, `rx_fill_ring_empty_descs`) and the fallback counter go to status.
- **Capabilities**: `CAP_NET_ADMIN`, `CAP_NET_RAW`, `CAP_BPF` and `CAP_IPC_LOCK`. Without `CAP_IPC_LOCK` the UMEM counts against `RLIMIT_MEMLOCK`, often 8 MB.
  - `open` fails with the missing capability and the `setcap` command. It does not fall back to the kernel backend.

### 4. MoldUDP64 session layer

- **Reorder buffer**: `moldudp::Receiver` copies up to `reorder_packets` packets (default 256) that arrive ahead of a gap into a slab allocated at construction. Each copy keeps its receive metadata, so messages drained later keep their original T0.
  - When the buffer is full, later packets are dropped and counted, and recovery continues from requests.
- **Metadata**: `on_packet` takes a line index, `now_ns` and a metadata value that is passed through to `on_message`. The last one makes the stale request timestamp go away.
- **A/B arbitration**: the receiver accepts packets from any line.
  - The first copy of a sequence wins. Later copies count as duplicates per line.
  - The receiver records the arrival skew between lines.
  - A gap is declared after `gap_timeout_ns` (default 2 ms, as in `Mdp3Feed`) without the missing sequence on any line. Operators size it from the exported skew.
- **Retransmission**: requests go to `rerequest` (host:port) from a kernel UDP socket.
  - Without `rerequest`, or when a request times out `max_request_attempts` times, the gap is unrecoverable. The feed then starts recovery (section 5).
- **End of Session**: after End of Session, packets from a new session are adopted only with `follow_session = true` (default false).

### 5. ITCH feed venue (`kind = "nasdaq_itch"`)

- **Startup and recovery**:
  - The venue joins multicast first and buffers up to `recovery_buffer_packets` datagrams (default 65536, allocated at `open`).
  - It then logs in to GLIMPSE over SoupBinTCP and builds the directory and the L3 books from the snapshot.
  - It applies the buffered and live ITCH messages from the sequence in End of Snapshot.
  - Books stay `Resyncing` until then.
  - The same procedure runs after a gap the receiver gives up, after any L3 book error, and when the buffer overflows during a snapshot. Overflowing during two snapshots in a row trips the venue's kill switch (`KillReason::FeedLost`).
  - If the buffer starts after the End of Snapshot sequence, the receiver is moved back (`Receiver::reset`) and the hole is re-requested; without `rerequest`, another snapshot is taken.
  - Without `glimpse_url`, the venue must start before the directory spin (sequence 1) and cannot recover from a gap; `open` warns.
- **Bridge**, on the net thread: source → MoldUDP receiver → `ItchDecoder` → one `L3Book` per configured instrument → L2 messages to the engine:
  - `BookDeltaMsg` with `kSnapshot` (top `depth` levels per side, default 20) when a book becomes complete, and `ConnectionStateMsg{Resyncing}` when it stops being complete.
  - After that, at most one `BookDeltaMsg` per instrument per datagram. Levels carry absolute quantities, 0 deletes. A level that enters the top `depth` because another one emptied is included.
  - `TradeMsg` for P/Q, and for E and C against displayed orders. Price comes from the resting order for E and from the message for C. The aggressor side is opposite to the resting side. C with Printable = N is not a trade; the decoder keeps that flag instead of dropping it.
  - `R` and `S` messages are decoded whatever their locate. Other messages for locates that are not configured are skipped after the 11-byte header.
- **`L3Book` changes**:
  - Capacity and price window become constructor arguments (`price_window_ticks`, `max_orders` from config) instead of template parameters.
  - Orders outside the window (stub quotes) are kept in an overflow store, so they neither drop nor trigger a recentre on every add.
  - Prices use ITCH's 4 decimals, tick 0.0001.
- **Timestamps**:
  - Every message of a datagram carries that datagram's `t0_cycles`.
  - `t1_delta` is taken after decoding and the L3 update.
  - `recv_ts` is `sw_ts_ns`, or the wall clock when that is missing. `hw_ts_ns` replaces it only with `hw_clock = "phc_synced"`.
  - The net thread records the kernel-to-T0 hop (`t0_wall_ns - sw_ts_ns`) in a venue histogram, the same way `WireLatencyRecorder` does. The engine's `LatencyInterval` does not change.
- **Order entry**: `order_entry = "none"` (default) or `"sim_ouch"`.
  - With `none`, API keys are not required: `resolve_venue_env` skips the check for this kind. Orders are rejected by the venue with the existing `VenueReject`.
  - `sim_ouch` sends OUCH 5.0 over SoupBinTCP to `fastmm-sim-itch` only (section 6). The ClOrdID is `T` and 13 digits: the MoldUDP64 sequence of the first datagram of the receive batch that triggered the order. The engine copies only `t0_cycles` into outbound messages, so the venue maps `t0_cycles` to that sequence in a 4096-slot table.
  - GLIMPSE and OUCH logins use `glimpse_username`/`glimpse_password` and `ouch_username`/`ouch_password`.
- **Engine**: no changes. `Venue` gains `poll()`, called after every reactor iteration; in busy mode the venue polls its sources there, in adaptive mode the reactor waits on their descriptors.
- The sources open in `load_reference_data`, so a missing capability or interface fails the start (exit code 4).
- **Status** (`kStatusVersion` 4):
  - packets, bytes, per-line packets, duplicates and skew;
  - gaps, recovered and unrecovered sequences, snapshot recoveries;
  - reorder high-water mark, requests, malformed frames;
  - the kernel-to-T0 p50/p99;
  - the AF_XDP statistics and mode.

### 6. Simulator and end-to-end benchmark

- **`fastmm-sim-itch`** runs `MatchingEngine` with `MarketGenerator`.
  - It encodes ITCH, packs MoldUDP64 and sends to A and B groups with `sendmmsg`. Rate, burst size and a drop rate per line are configurable.
  - It answers re-requests from its `Transmitter` history and serves GLIMPSE from the matching engine state.
  - It accepts OUCH 5.0 orders over SoupBinTCP. This needs server-side OUCH encoding and decoding on the existing message structs.
- **Wire-to-wire**: for every Enter or Replace Order whose token names a sequence, the simulator records the time from `sendmmsg` of that datagram to receiving the order.
  - It uses its own TSC, stamped per datagram, independent of the engine's instrumentation.
- **`scripts/bench-e2e.sh`** runs the simulator and `fastmm-live` in two network namespaces joined by a veth pair, pinned to separate cores. Market data and orders cross the veth.
  - It reports wire-to-wire and the engine's hops at p50, p99 and p99.9 for each backend.
  - The kernel backend runs in `unshare -Urn`. `af_xdp` runs under `sudo scripts/bench-e2e.sh --backend af_xdp`.
- veth has no NAPI busy polling without XDP, no zero-copy and no NIC timestamps. The results compare the backends on this machine; the benchmarks page names the mode measured.

### 7. Not in this record

- ef_vi. `DatagramSource` already provides batch delivery, spans valid until return and hardware timestamps; it becomes a backend when a Solarflare NIC is available to test on.
- DPDK.
- AF_XDP hardware RX timestamps through XDP metadata kfuncs (Linux 6.3+ and driver support).
- CME MDP3 and IEX over multicast. The source and session layers do not depend on the feed, and `Mdp3Feed` already takes datagrams.
- IPv6 multicast.
- Splitting L3 work across threads when one feed carries many configured symbols.

## Consequences

- Maintenance:
  - A second network path, with a BPF program kept as bytecode; changes to it go through the verifier tests below.
  - `L3Book` moves from template capacities to runtime capacities.
- AF_XDP needs capabilities, so `fastmm-live` with `af_xdp` runs with `setcap` or as root. The AF_XDP tests run through `sudo scripts/xdp-test.sh`, and `ctest` skips them without privileges.
- The net thread does the L3 work: one `L3Book` update per ITCH message for configured symbols. The engine sees L2 changes within `depth`.
- ADR-0014's "live ITCH out of scope" is replaced for market data. Order entry to Nasdaq is unchanged.

## Implementation order

1. `net::UdpSocket` and the `kernel` `DatagramSource`: multicast join, `recvmmsg`, timestamps, busy-poll options.
   Tests: multicast in an `unshare -Urn` namespace, timestamps, batch boundaries, truncation, no allocation in `poll`.
2. MoldUDP64 receiver: reorder slab with metadata, A/B arbitration, `gap_timeout_ns`, request attempts, session follow.
   Tests: two lines with independent drops, reordering and duplicates over 20 seeds, with and without retransmission; every message is delivered once and in order.
3. `L3Book` runtime capacities and overflow store; the ITCH-to-L2 bridge with snapshots, trades, and T0/T1.
   Tests: property test against a reference book rebuilt from the same messages, including stub quotes and C/N executions; `noalloc`.
4. GLIMPSE client and the `nasdaq_itch` venue: config keys, status version 4, `fastmm-top` columns.
   Tests: join mid-stream, buffered replay from End of Snapshot, recovery after an unrecoverable gap.
5. `fastmm-sim-itch` with GLIMPSE and the SoupBinTCP/OUCH gateway, the venue's `sim_ouch` path, and `scripts/bench-e2e.sh` with the kernel backend. Results go in `bench/README.md`.
6. AF_XDP backend: UMEM and rings, BPF bytecode, `BPF_LINK_CREATE`, mode fallback, capability check, statistics.
   Tests (all through `sudo scripts/xdp-test.sh`): verifier load, `BPF_PROG_TEST_RUN` against crafted frames compared with the user-space parser, and receive over veth in each mode.
   Then `bench-e2e.sh --backend af_xdp`.
7. Docs:
   - a how-to for multicast feeds (capabilities, queue steering, `napi_defer_hard_irqs`, `isolcpus`, IRQ affinity);
   - the configuration reference;
   - the event-flow page with the new T0.

Steps 1, 2 and 6 are independent. 3 needs 2. 4 needs 1 and 3. 5 needs 4.
