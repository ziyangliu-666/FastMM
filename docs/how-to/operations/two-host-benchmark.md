# Two-host benchmark

`scripts/bench-2host.sh` measures tick-to-order latency across a real network. It runs `fastmm-sim-itch` on one host over ssh and `fastmm-live` on another, and prints the table of `scripts/bench-e2e.sh` ([Benchmarks](../../explanation/benchmarks.md)).

You need two Ubuntu 24.04 VMs in one private network, each with a public NIC for ssh and a private NIC for the test, and root on both.

## 1. Build and copy

1. On the build machine, build the tarball with DPDK linked in. It lands in `dist/`:

    ```bash
    scripts/package-release.sh
    scp dist/fastmm-*-x86_64.tar.gz root@<sim>:/opt/
    scp dist/fastmm-*-x86_64.tar.gz root@<live>:/opt/
    ```

2. On both hosts, unpack it and prepare the host. `all` installs the packages and hugepages, turns irqbalance off and sends interrupts to CPU 0. `firewall` lets ufw pass the private subnet:

    ```bash
    cd /opt && tar xzf fastmm-*-x86_64.tar.gz && ln -sfn /opt/fastmm-*-x86_64 /opt/fastmm
    /opt/fastmm/scripts/host-setup.sh all
    /opt/fastmm/scripts/host-setup.sh firewall enp6s0
    ```

    `all` ends by listing each NIC with its PCI address, driver and queues.

3. Check that the private addresses ping each other.
4. Give the live host a key for `ssh root@<sim public ip>`. Use the public address: with `dpdk`, the live host's private NIC leaves the kernel.

## 2. Run

On the live host, run one backend at a time. `--prefix-len` is the private subnet's:

```bash
cd /opt/fastmm
C="--remote-sim root@<sim public ip> --sim-ip <sim vpc ip> --live-ip <live vpc ip>"
C="$C --prefix-len 20 --build . --duration 30 --runs 3"

scripts/bench-2host.sh $C --backend kernel --iface enp6s0
scripts/bench-2host.sh $C --backend kernel --iface enp6s0 --md multicast  # VPC with multicast

scripts/host-setup.sh xdp-prep enp6s0     # fewest queues, GRO/LRO off
scripts/bench-2host.sh $C --backend af_xdp --iface enp6s0

scripts/host-setup.sh dpdk-bind enp6s0    # prints the PCI address
scripts/bench-2host.sh $C --backend dpdk --dpdk-pci 0000:06:00.0
scripts/host-setup.sh dpdk-unbind 0000:06:00.0
```

| Backend | What happens on the live host |
|---|---|
| `kernel` | market data arrives on kernel UDP sockets; `--md multicast` uses multicast where the network carries it |
| `af_xdp` | `xdp-prep` first cuts the NIC to the fewest queues and turns GRO and LRO off |
| `dpdk` | the NIC is bound to `vfio-pci` in no-IOMMU mode; `fastmm-live` puts `<live vpc ip>` on a tap, `fmx0`, for ARP, GLIMPSE, re-requests and OUCH ([exception port](multicast-feeds.md#dpdk)) |

Market data goes unicast to `<live vpc ip>`, ports 31001 and 31002. GLIMPSE, re-requests and OUCH go to the simulator's private address.

Default pinning: the simulator on CPU 1; the engine on CPU 1 and the network thread on CPU 2 of the live host; CPU 0 takes the interrupts. Change it with `--sim-cpu`, `--engine-cpu` and `--net-cpu`; `--threading single` runs everything on the engine CPU.

Each run writes `runs/bench-2host-<time>-<backend>-<md>/`: the live config, both logs, and `sim.json` and `live.json` per run. The live log ends with the backend's counters, the `dpdk:` and `af_xdp:` lines.

## 3. Privileged tests on the live host

```bash
scripts/xdp-test.sh --build . --e2e   # verifier, XDP over veth, bench-e2e af_xdp
tests/fastmm_dpdk_tests                # DPDK over veth, in a user namespace
```
