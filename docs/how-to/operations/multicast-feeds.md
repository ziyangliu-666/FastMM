# Receive a multicast feed

A `nasdaq_itch` venue receives TotalView-ITCH 5.0 over MoldUDP64 multicast on two lines, A and B, and builds its books from a GLIMPSE snapshot. Orders go out over OUCH. Connector reference: [Venue connectors](../../reference/venues/nasdaq-itch.md); design: [ADR-0015](../../adr/0015-multicast-market-data.md).

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

Every key is in [Configuration](../../reference/configuration.md#nasdaq_itch).

- Each `[[instruments]]` symbol is the ITCH stock symbol, 1 to 8 characters, with a tick that is a multiple of 0.0001.
- Without `glimpse_url` the process must start before the feed's first message, sequence 1.
- Against `fastmm-sim-itch` on one host, both processes need a network namespace whose `lo` carries multicast. The command is at the top of `configs/nasdaq-itch-sim.toml`.

On a network without multicast, such as a cloud VPC, give each line the receiving host's own address and send the lines there:

```bash
fastmm-sim-itch --line-a 10.0.0.2:31001 --line-b 10.0.0.2:31002
```

```toml
line_a = "10.0.0.2:31001"   # a local unicast address: bound, not joined
line_b = "10.0.0.2:31002"
```

All three backends take unicast lines and match them by address and port.

## 2. Choose the receive backend

| `rx_backend` | Needs | Receive path |
|---|---|---|
| `kernel` | nothing | one UDP socket per line, `recvmmsg`, kernel receive timestamps |
| `af_xdp` | Linux 5.11; `CAP_NET_ADMIN`, `CAP_NET_RAW`, `CAP_BPF`, `CAP_IPC_LOCK`; an interface name per line | an XDP program redirects the lines' datagrams to AF_XDP sockets; other traffic goes to the kernel |
| `dpdk` | a `-DFASTMM_WITH_DPDK=ON` build, `spin_mode = "busy"` | `rte_eth_rx_burst` on one DPDK port; the kernel's traffic through an exception tap ([DPDK](#dpdk)) |

### AF_XDP

The XDP program takes the feed's datagrams off the network card before the kernel's IP and UDP layers and hands them to the network thread through a shared-memory ring. Everything else still goes through the kernel.

<video class="fastmm-video" autoplay loop muted playsinline preload="metadata" poster="../../../assets/anim/kernel-bypass.jpg" src="../../../assets/anim/kernel-bypass.mp4"></video>

Grant the capabilities to the binary, or run it as root:

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_bpf,cap_ipc_lock+ep build/release/bin/fastmm-live
```

- A missing capability stops the start with exit code 4 and prints the `setcap` command. There is no fallback to `kernel`.
- Without `CAP_IPC_LOCK`, each socket's 16 MiB UMEM counts against `RLIMIT_MEMLOCK`.
- `xdp_mode` pins the attach mode. The log and `fastmm-top` show the mode in use: `af_xdp/zerocopy`, `af_xdp/native_copy` or `af_xdp/generic`.
- An interface that already has an XDP program refuses the attach with `EBUSY`.

### Solarflare and Onload

On Solarflare NICs, run the `kernel` backend under Onload:

```bash
onload --profile=latency build/release/bin/fastmm-live --config configs/nasdaq-itch-sim.toml
```

Onload also accelerates the OUCH connection, a non-blocking kernel TCP socket in the same process. Keep `[engine] net_backend = "epoll"`, the default: Onload intercepts epoll, not io_uring. FastMM has no TCPDirect or ef_vi path, and this setup is untested on Solarflare hardware.

### DPDK

Build with DPDK:

```bash
cmake --preset release-dpdk
cmake --build --preset release-dpdk -j
```

CMake uses pkg-config's `libdpdk`. Without one it runs `scripts/build-dpdk.sh`, which builds a static DPDK 25.11 into `<build>/_deps/dpdk` in a few minutes, without root. `scripts/package-release.sh` packs the binaries for other hosts.

To try the code path without hugepages, PCI access or root, read a veth through `net_af_packet`:

```toml
rx_backend = "dpdk"
interface = "fmlive"
dpdk_eal_args = """--no-huge --no-pci --in-memory --no-telemetry -l 0 -m 128 \
  --vdev=net_af_packet0,iface=fmlive,framecnt=4096"""
dpdk_port = "net_af_packet0"
```

The kernel still receives every frame here, so this tests the code, not kernel bypass. In a user namespace the EAL logs `Error creating '/var/run/dpdk'`; with `--in-memory` it needs no runtime files and carries on.

On a real NIC:

1. Bind it to `vfio-pci` with `scripts/host-setup.sh dpdk-bind <iface>`; a VM uses no-IOMMU mode.
2. Give the EAL hugepages and name the PCI address in `dpdk_port`.
3. Add an exception port, so the kernel keeps a network device for GLIMPSE, re-requests, IGMP, ARP and TCP:

```toml
dpdk_eal_args = "-l 0 --in-memory --no-telemetry -a 0000:06:00.0 --vdev=net_tap0,iface=fmx0"
dpdk_port = "0000:06:00.0"
dpdk_exception_port = "net_tap0"      # a net_tap vdev
dpdk_exception_ip = "10.0.0.2/24"     # the host's address, now on fmx0
interface = "fmx0"                    # IGMP joins go out through it
```

The tap takes the port's MAC. Frames the venue does not take go to the kernel through it, and what the kernel sends there leaves through the port. The tap is read every `dpdk_exception_interval_us`, 20 µs by default, at one system call per read; set 0 when the OUCH connection goes through it. Without an exception port the source answers ARP for unicast line addresses and drops everything else.

## 3. Steer the groups to one RX queue (af_xdp)

An XDP socket reads one RX queue, and the NIC spreads datagrams over its queues by RSS. Without `queues`, the source opens a socket, with its own 16 MiB UMEM, on every RX queue and polls each one on every loop. On a NIC with many queues, pin each group to one queue and list it in `queues`:

```bash
sudo ethtool -N eth1 flow-type udp4 dst-ip 233.54.12.111 action 2
sudo ethtool -N eth1 flow-type udp4 dst-ip 233.49.196.111 action 2
ethtool -n eth1
```

Datagrams that land on a queue without a socket go to the kernel; `fastmm-top` counts them as `fallback`. virtio_net adds a queue pair per CPU once an XDP program is attached, and the host delivers on those too.

## 4. Busy polling

With `[engine] spin_mode = "busy"` the network thread polls the sockets on every loop instead of waiting in epoll. It sets `SO_BUSY_POLL` and `SO_PREFER_BUSY_POLL`, and `SO_BUSY_POLL_BUDGET` for `af_xdp`. `SO_PREFER_BUSY_POLL` needs `CAP_NET_ADMIN`; a refused option is logged and the socket is polled from user space.

The options take effect only when the interface defers its interrupts:

```bash
echo 2      | sudo tee /sys/class/net/eth1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/eth1/gro_flush_timeout     # ns
```

The busy thread uses a whole core; give it one in the next step.

## 5. Isolate the cores

1. Reserve cores on the kernel command line, for example `isolcpus=4,6 nohz_full=4,6 rcu_nocbs=4,6`, and reboot.
2. Pin the engine and the network thread to them: `[engine] cpu = 4`, `net_cpus = [6]`.
3. Move the NIC's interrupts off those cores, next to the network thread's queue:

```bash
sudo systemctl stop irqbalance
grep eth1 /proc/interrupts
echo 5 | sudo tee /proc/irq/<irq>/smp_affinity_list
```

`scripts/host-setup.sh irq-affinity eth1 5` does this for every queue of `eth1`. `[engine] log_irq_affinity = true` logs the result at start ([NIC interrupts](running-in-production.md#nic-interrupts)).

## 6. Timestamps

Each event's `recv_ts` is the kernel's receive time of its datagram; with `af_xdp` it is the wall clock at T0. The status file holds the kernel-to-T0 time, from the kernel's receive to the network thread's read.

For NIC timestamps on the `kernel` backend:

1. Set `hw_timestamps = true`; it needs `CAP_NET_ADMIN`.
2. Set `hw_clock = "phc_synced"` and make the NIC clock follow `CLOCK_REALTIME`, for example with `phc2sys -s eth1 -c CLOCK_REALTIME -O 0`. Only then do NIC times replace `recv_ts`.

## 7. Check the feed

`fastmm-top` shows one feed line per multicast venue ([Status file](../../reference/status-file.md#multicast-feed)):

| Field | Read it as |
|---|---|
| `state` | `live`; `snapshot` while a GLIMPSE snapshot is taken; `lost` when the books cannot be rebuilt, which trips the venue's kill switch with `FeedLost` |
| `pkts_a`, `pkts_b` | grow together; a line at 0 is not joined or not routed |
| `skew_max` | the largest delay of a line's copy behind the first copy. Set `gap_timeout_ns` above it, or the venue re-requests packets the other line is about to deliver |
| `gaps`, `recov`, `lost` | gaps declared, messages recovered by re-request, sequences given up |
| `snaps` | GLIMPSE snapshots, one per given-up range or L3 book inconsistency |
| `reorder` | most packets held ahead of a gap; at `reorder_packets`, later packets are dropped and re-requested |

## 8. Order entry

The OUCH connection runs on kernel TCP with `TCP_NODELAY`. For busy polling and kernel bypass on it, see [Low-latency TCP](low-latency-tcp.md).

## 9. Two hosts

`scripts/bench-2host.sh` runs `fastmm-sim-itch` on another host over ssh and `fastmm-live` here, with unicast lines. Steps: [Two-host benchmark](two-host-benchmark.md).
