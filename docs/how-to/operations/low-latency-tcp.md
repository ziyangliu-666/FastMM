# Low-latency TCP

Order entry runs on the kernel TCP stack: the OUCH connection of `nasdaq_itch` and the WebSocket and REST connections of the crypto venues. Every one of them sets `TCP_NODELAY`, and `nasdaq_itch` writes all the orders of one drain of the order ring with a single `write`. Three steps make that path faster without changing FastMM: no interrupt coalescing on the NIC, busy polling in the kernel, and a kernel-bypass stack loaded with `LD_PRELOAD`.

## 1. Receive interrupts

The TCP connections read what the NIC's receive interrupt delivered to their sockets. A NIC that coalesces interrupts (`ethtool -c eth1`: `Adaptive RX: on`, or a non-zero `rx-usecs`) holds each packet back for up to that long first. Turn it off on the trading interface:

```bash
sudo ethtool -C eth1 adaptive-rx off rx-usecs 0
```

The [NIC interrupts](running-in-production.md#nic-interrupts) section has what it changes and the warning `log_irq_affinity` prints while it is on. Pin the network thread and steer the NIC's interrupts as described there.

## 2. Busy polling

Busy polling makes a thread that waits for a socket poll the NIC queue itself instead of waiting for its interrupt:

```bash
sudo sysctl -w net.core.busy_read=50     # µs; the SO_BUSY_POLL default of every socket
sudo sysctl -w net.core.busy_poll=50     # µs; epoll_wait and poll
echo 2      | sudo tee /sys/class/net/eth1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/eth1/gro_flush_timeout     # ns
```

It runs where a thread would wait: an adaptive network thread blocked in `epoll_wait` while idle. A network thread that polls (`spin_mode = "busy"`, or `net_spin_dedicated`) calls `epoll_wait` with a zero timeout, which returns without polling the queue, and reads a socket only once it is readable, so for it the interrupt setting above is what counts. The TCP connections do not set `SO_BUSY_POLL` themselves, so `net.core.busy_read` is what applies to them.

## 3. Kernel bypass: OpenOnload or XLIO

Both stacks intercept the socket calls through `LD_PRELOAD` and run TCP and UDP in user space on their own NICs. FastMM needs no rebuild.

| Stack | NICs | Start |
|---|---|---|
| [OpenOnload](https://github.com/Xilinx-CNS/onload) | AMD Solarflare | `onload --profile=latency fastmm-live --config live.toml` |
| [NVIDIA XLIO](https://docs.nvidia.com/networking/category/xlio) | NVIDIA ConnectX, BlueField | `LD_PRELOAD=libxlio.so fastmm-live --config live.toml` |

- Keep `[engine] net_backend = "epoll"` (the default). Both stacks intercept `epoll_wait`, `read` and `write`; `io_uring` goes around them.
- Keep `rx_backend = "kernel"` on the accelerated NIC: the stack then takes the multicast feed's UDP sockets too. `af_xdp` and `dpdk` own the NIC queues themselves.
- Run with `spin_mode = "busy"` and the network thread on an isolated core; the stacks poll from the calling thread.
- Check that the stack is in use: `onload_stackdump` lists the process's stacks; XLIO prints its banner on start.

`bench_order_tcp` measures the kernel path over a veth pair ([bench/README.md](../../../bench/README.md)).
