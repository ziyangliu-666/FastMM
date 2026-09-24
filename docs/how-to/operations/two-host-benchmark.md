# Two-host benchmark

`scripts/bench-2host.sh` runs `fastmm-sim-itch` on one host (over ssh) and `fastmm-live` on another, and prints the table of `scripts/bench-e2e.sh` ([Benchmarks](../../explanation/benchmarks.md)). The steps below are for two Ubuntu 24.04 VMs with virtio NICs in one VPC: a public NIC (SSH) and a VPC NIC for the test. Everything runs as root.

## 1. Build and copy

On the build machine (Ubuntu 24.04 or older glibc):

```bash
scripts/package-release.sh            # build/release-dpdk, DPDK linked in; dist/fastmm-<version>-x86_64.tar.gz
scp dist/fastmm-*-x86_64.tar.gz root@<sim>:/opt/
scp dist/fastmm-*-x86_64.tar.gz root@<live>:/opt/
```

On both hosts:

```bash
cd /opt && tar xzf fastmm-*-x86_64.tar.gz && ln -sfn /opt/fastmm-*-x86_64 /opt/fastmm
/opt/fastmm/scripts/host-setup.sh all     # packages, hugepages, irqbalance off, interrupts to CPU 0, info
/opt/fastmm/scripts/host-setup.sh firewall enp6s0   # ufw (on by default on Vultr): allow the VPC subnet
```

`info` lists each NIC with its PCI address, driver and queues. Check that the VPC addresses ping each other, and give the live host a key for `ssh root@<sim public address>` (the live host's VPC NIC leaves the kernel for DPDK, so use the public address for ssh).

## 2. Run

On the live host (`--prefix-len` is the VPC subnet's):

```bash
cd /opt/fastmm
C="--remote-sim root@<sim public ip> --sim-ip <sim vpc ip> --live-ip <live vpc ip> --prefix-len 20 --build . --duration 30 --runs 3"

scripts/bench-2host.sh $C --backend kernel --iface enp6s0
scripts/bench-2host.sh $C --backend kernel --iface enp6s0 --md multicast   # only if the VPC carries multicast

scripts/host-setup.sh xdp-prep enp6s0                                        # fewest queues, GRO/LRO off
scripts/bench-2host.sh $C --backend af_xdp --iface enp6s0

scripts/host-setup.sh dpdk-bind enp6s0                                       # prints the PCI address
scripts/bench-2host.sh $C --backend dpdk --dpdk-pci 0000:06:00.0
scripts/host-setup.sh dpdk-unbind 0000:06:00.0
```

- Market data is unicast to `<live vpc ip>`:31001/31002; GLIMPSE, re-requests and OUCH go to the simulator's VPC address.
- `dpdk`: `vfio-pci` in no-IOMMU mode; fastmm-live puts `<live vpc ip>` on a tap (`fmx0`) behind the DPDK port for ARP, GLIMPSE, re-requests and OUCH ([exception port](multicast-feeds.md#dpdk)).
- Pinning defaults: simulator on CPU 1, engine on 1 and network thread on 2 of the live host (`--sim-cpu`, `--engine-cpu`, `--net-cpu`); CPU 0 takes the interrupts. `--threading single` runs everything on the engine CPU.
- Output: `runs/bench-2host-<time>-<backend>-<md>/` with the live config, both logs, `sim.json` and `live.json` per run. The live log ends with the backend's counters (`dpdk:` and `af_xdp:` lines).

## 3. Privileged tests on the live host

```bash
scripts/xdp-test.sh --build . --e2e        # verifier, XDP over veth, bench-e2e af_xdp
tests/fastmm_dpdk_tests                     # DPDK over veth (af_packet vdev), in a user namespace
```
