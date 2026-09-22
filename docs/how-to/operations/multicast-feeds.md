# Receive a multicast feed

A `nasdaq_itch` venue receives TotalView-ITCH 5.0 over MoldUDP64 multicast on two lines, A and B, and builds its books from a GLIMPSE snapshot ([Venue connectors](../../reference/venues.md#nasdaq-totalview-itch-nasdaq_itch)). Design: [ADR-0015](../../adr/0015-multicast-market-data.md).

## 1. Configure the venue

<!-- snippet: configs/nasdaq-itch-sim.toml#venue -->
```toml
[venues.itch]
kind = "nasdaq_itch"
rx_backend = "kernel"        # kernel | af_xdp (needs capabilities, docs/how-to/operations/multicast-feeds.md)
interface = "lo"             # both lines; line_a_interface / line_b_interface override
line_a = "239.192.0.1:31001"
line_b = "239.192.0.2:31002"
rerequest = "127.0.0.1:31000"
glimpse_url = "127.0.0.1:31010"
glimpse_username = "glimps"  # the simulator's defaults ([sim.glimpse] in configs/sim-itch.toml)
glimpse_password = "glimpse"
reorder_packets = 256
gap_timeout_ns = 2000000
max_request_attempts = 4
recovery_buffer_packets = 65536
depth = 20
order_entry = "sim_ouch"     # none: every order is rejected
ouch_url = "127.0.0.1:31020"
ouch_username = "fmouch"
ouch_password = "ouch"
supports_replace = true
```

Every key: [Configuration](../../reference/configuration.md#connector-specific-keys). Each `[[instruments]]` symbol is the ITCH stock symbol (1 to 8 characters) with a tick that is a multiple of 0.0001. Without `glimpse_url` the process must start before the feed's first message (sequence 1).

Against `fastmm-sim-itch` on one host, both processes need a network namespace whose `lo` carries multicast (the command is at the top of `configs/nasdaq-itch-sim.toml`); `scripts/bench-e2e.sh` runs them in two namespaces joined by a veth pair.

## 2. Choose the receive backend

| `rx_backend` | Needs | Receive path |
|---|---|---|
| `kernel` | nothing | one UDP socket per line, `recvmmsg`, kernel receive timestamps |
| `af_xdp` | Linux 5.11, `CAP_NET_ADMIN`, `CAP_NET_RAW`, `CAP_BPF`, `CAP_IPC_LOCK`; an interface name per line | an XDP program redirects the lines' datagrams to AF_XDP sockets; everything else goes to the kernel |

For `af_xdp`, grant the capabilities to the binary (or run it as root):

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_bpf,cap_ipc_lock+ep build/release/bin/fastmm-live
```

Without `CAP_IPC_LOCK` the UMEM (4096 frames of 4096 bytes per socket, 16 MiB) counts against `RLIMIT_MEMLOCK`. A missing capability stops the start with exit code 4 and names the `setcap` command; there is no fallback to `kernel`. `xdp_mode` pins the attach mode; the mode in use is logged and shown by `fastmm-top` (`af_xdp/zerocopy`, `af_xdp/native_copy`, `af_xdp/generic`). An interface that already has an XDP program refuses the attach (`EBUSY`).

On Solarflare NICs, run the `kernel` backend under Onload instead:

```bash
onload --profile=latency build/release/bin/fastmm-live --config configs/nasdaq-itch-sim.toml
```

The order connection (OUCH over SoupBinTCP, `order_entry = "sim_ouch"`) is a plain non-blocking kernel TCP socket driven by `connect`, `read`, `write` and the reactor, so Onload accelerates it in the same process without changes. Keep `[engine] net_backend = "epoll"` (the default): Onload intercepts epoll, not io_uring. FastMM has no TCPDirect or ef_vi path. Not tested on Solarflare hardware.

## 3. Steer the groups to one RX queue (af_xdp)

The NIC spreads multicast over its RX queues by RSS; the XDP socket reads one queue. Pin each group to a queue and list it in `queues`:

```bash
sudo ethtool -N eth1 flow-type udp4 dst-ip 233.54.12.111 action 2
sudo ethtool -N eth1 flow-type udp4 dst-ip 233.49.196.111 action 2
ethtool -n eth1                                   # the installed rules
```

Datagrams of a subscribed group that land on a queue without a socket go to the kernel; `fastmm-top` counts them as `fallback`.

## 4. Busy polling

With `[engine] spin_mode = "busy"` the network thread polls the sockets on every loop iteration instead of waiting in epoll, and sets `SO_BUSY_POLL` and `SO_PREFER_BUSY_POLL` (`af_xdp`: also `SO_BUSY_POLL_BUDGET`). `SO_PREFER_BUSY_POLL` needs `CAP_NET_ADMIN`; a refused option is logged and the socket is polled from user space. The options take effect only when the interface defers its interrupts:

```bash
echo 2      | sudo tee /sys/class/net/eth1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/eth1/gro_flush_timeout     # ns
```

The busy thread uses a whole core; give it one (next step).

## 5. Isolate the cores

1. Reserve cores on the kernel command line, for example `isolcpus=4,6 nohz_full=4,6 rcu_nocbs=4,6`, and reboot.
2. Pin the engine and the venue's network thread to them: `[engine] cpu = 4`, `net_cpus = [6]`.
3. Keep the NIC's interrupts off those cores and next to the network thread's queue: stop `irqbalance`, find the queue's IRQ in `/proc/interrupts`, and set it:

```bash
sudo systemctl stop irqbalance
grep eth1 /proc/interrupts
echo 5 | sudo tee /proc/irq/<irq>/smp_affinity_list
```

## 6. Timestamps

`recv_ts` of every event is the kernel's receive time of its datagram (`af_xdp`: the wall clock at T0). `hw_timestamps = true` enables NIC timestamps on the line interfaces (`SIOCSHWTSTAMP`, `CAP_NET_ADMIN`, `kernel` backend); they replace `recv_ts` only with `hw_clock = "phc_synced"`, which requires the NIC clock to follow `CLOCK_REALTIME` (for example `phc2sys -s eth1 -c CLOCK_REALTIME -O 0`). The kernel-to-T0 time (from the kernel's receive time to the network thread's read) is in the status file.

## 7. Check the feed

`fastmm-top` shows one feed line per multicast venue ([Status file](../../reference/status-file.md#multicast-feed)):

- `state` `live`; `snapshot` while a GLIMPSE snapshot is taken; `lost` when the books cannot be rebuilt (the venue's kill switch is tripped with `FeedLost`).
- `pkts_a`, `pkts_b` grow together; a line at 0 is not joined or not routed.
- `skew_max` is the largest delay of a line's copy behind the first copy. Set `gap_timeout_ns` above it: a smaller value re-requests packets that the other line is about to deliver.
- `gaps`, `recov`, `lost`: gaps declared, messages recovered by re-request, sequences given up. Every given-up range and every L3 book inconsistency starts a GLIMPSE snapshot (`snaps`).
- `reorder`: most packets held ahead of a gap; at `reorder_packets` later packets are dropped and re-requested.
