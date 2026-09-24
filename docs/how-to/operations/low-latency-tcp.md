# Low-latency TCP

Order entry runs on the kernel TCP stack: the OUCH connection of `nasdaq_itch` and the WebSocket and REST connections of the crypto venues. Every one of them sets `TCP_NODELAY`, and `nasdaq_itch` writes all the orders of one drain of the order ring with a single `write`. Two steps make that path faster without changing FastMM: busy polling in the kernel, and a kernel-bypass stack loaded with `LD_PRELOAD`.

## 1. Busy polling

With `[engine] spin_mode = "busy"` the network thread never sleeps in `epoll_wait`. Busy polling makes the reading thread also poll the NIC queue instead of waiting for its interrupt:

```bash
sudo sysctl -w net.core.busy_read=50     # µs; the SO_BUSY_POLL default of every socket
sudo sysctl -w net.core.busy_poll=50     # µs; epoll_wait and poll
echo 2      | sudo tee /sys/class/net/eth1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/eth1/gro_flush_timeout     # ns
```

The TCP connections do not set `SO_BUSY_POLL` themselves, so `net.core.busy_read` is what applies to them. Pin the network thread and steer the NIC's interrupts as in [Multicast feeds](multicast-feeds.md#5-isolate-the-cores).

## 2. Kernel bypass: OpenOnload or XLIO

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
