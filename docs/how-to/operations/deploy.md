# Deploy a release

Each release ships in three forms with the same version ([Versions and compatibility](../../reference/compatibility.md)). Download them from [GitHub releases](https://github.com/ziyangliu-666/FastMM/releases) or PyPI.

| Form | Holds | Use it for |
|---|---|---|
| `fastmm-<version>-x86_64.tar.gz` | `fastmm-live`, `fastmm-gateway`, `fastmm-ctl`, `fastmm-top`, `fastmm-pnl`, `fastmm-replay`, `fastmm-sim-exchange`, `fastmm-sim-itch`, the simulator configs, the systemd units, the Prometheus alert rules | a host that runs C++ strategies |
| `ghcr.io/ziyangliu-666/fastmm:<version>` | the same programs without the simulator, non-root | a container host |
| `pip install "fastmm-engine[live]"` | the engine as a Python package | strategies written in [Python](../../python.md) |

The tarball and the image are built for x86-64-v2. The tarball needs glibc 2.28 or newer and nothing else, so it runs on Ubuntu 20.04, Debian 10, RHEL 8, Amazon Linux 2023 and later. Neither holds your own strategy: a C++ strategy is linked into your own `fastmm-live` build ([Register a strategy](../strategies/register-a-strategy.md)).

## Install the tarball

```bash
VERSION=$(curl -s https://api.github.com/repos/ziyangliu-666/FastMM/releases/latest \
  | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')   # the newest; or set VERSION=x.y.z
URL=https://github.com/ziyangliu-666/FastMM/releases/download/v$VERSION
curl -LO $URL/fastmm-$VERSION-x86_64.tar.gz
curl -LO $URL/fastmm-$VERSION-x86_64.tar.gz.sha256
sha256sum -c fastmm-$VERSION-x86_64.tar.gz.sha256
sudo tar xzf fastmm-$VERSION-x86_64.tar.gz -C /opt
sudo ln -sfn /opt/fastmm-$VERSION-x86_64 /opt/fastmm
/opt/fastmm/bin/fastmm-live --version
```

To upgrade, unpack the new version next to the old one and move the symlink after the session stops; `fastmm-top` refuses a status file written by another build. To keep the pause to one restart, start the new build with `--takeover` from its own directory ([Hand over a running session](hand-over-a-session.md)). The old unit then exits 0 and systemd does not restart it.

To build a tarball from a checkout, run `scripts/package-release.sh`:

| Option | Effect |
|---|---|
| default preset `release-dpdk` | links in the DPDK receive backend ([Receive a multicast feed](multicast-feeds.md)) |
| `--preset <name>` | another preset |
| `--container` | builds in `manylinux_2_28` with docker, as the release does; without it the tarball needs the build host's glibc or newer |

`scripts/check-release.sh <tarball>` runs a tarball on Ubuntu 22.04 and 24.04, Debian 12, Rocky Linux 9 and Amazon Linux 2023, as each release is before it is published.

## Run under systemd

`deploy/fastmm-live.service` is in the tarball and the repository. Run one unit per engine: one config, one journal directory, one status file.

1. Create the user, the directories and the config, and install the unit:

```bash
sudo useradd --system --home-dir /var/lib/fastmm --shell /usr/sbin/nologin fastmm
sudo install -d -o fastmm -g fastmm /var/lib/fastmm /var/lib/fastmm/runs
sudo install -d -m 0750 -o root -g fastmm /etc/fastmm
sudo install -m 0640 -o root -g fastmm my-live.toml /etc/fastmm/live.toml
sudo install -m 0644 /opt/fastmm/deploy/fastmm-live.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl start fastmm-live
```

2. Put `FASTMM_CONFIG` and the API keys the config reads as `${VAR}` in `/etc/fastmm/fastmm-live.env`, owned by root, mode 0600. An unset variable exits 2 before anything connects ([Keys](running-in-production.md#keys)).

```ini
FASTMM_CONFIG=/etc/fastmm/live.toml
FASTMM_API_KEY=...
FASTMM_API_SECRET=...
```

3. Check the session:

```bash
systemctl status fastmm-live                 # the exit code of the last run
journalctl -u fastmm-live -f
/opt/fastmm/bin/fastmm-top --name <engine name>
```

`journalctl` shows the lines at `[logging] mirror_level` and above; `[logging] file` takes the full log for a rotation you own. Journal files grow without a cap ([The journal](running-in-production.md#the-journal)). A latched `max_loss` trip is cleared with `sudo rm /var/lib/fastmm/runs/<engine name>.kill`.

### What the unit sets

| Directive | Why |
|---|---|
| `Type=notify`, `TimeoutStartSec=180` | the process sends `READY=1` once it trades or waits as a standby, so `systemctl start` reports a failed start. Reference data and account queries come first; raise the timeout for a config that loads more |
| `WatchdogSec=30` | the control loop sends `WATCHDOG=1`; a process silent for 30 s is killed and restarted like a crash. A strategy hook that hangs is caught sooner by `[engine] stall_ms`, which cancels every order and exits 5 ([Engine stall](kill-switch-and-shutdown.md#engine-stall)) |
| `Restart=on-failure` | a crash, `kill -9` included, or exit 4 restarts after 2 s. The new session restores the position and cancels the orders the dead one left ([Restarts](running-in-production.md#restarts)) |
| `RestartPreventExitStatus=2 3 5 6 7 8` | 2 and 3 need a config fix; 5, 6 and 7 need a human; 8 means another process holds the instance lock. A latched kill switch exits 6 at every start ([Errors and exit codes](../../reference/errors.md)) |
| `StartLimitBurst=5` | five starts in ten minutes end a crash loop |
| `TimeoutStopSec=90` | SIGTERM pulls the quotes and cancels every order over REST before exit ([Kill switch and shutdown](kill-switch-and-shutdown.md)) |
| `LimitMEMLOCK=infinity` | lets `[engine] lock_memory = true` call `mlockall()`; drop it when `lock_memory` is off |
| `#LimitRTPRIO=50`, `#AmbientCapabilities=CAP_SYS_NICE` | commented out. Either one lets `rt_priority` and `net_rt_priority` set `SCHED_FIFO`; enable one only with the threads on isolated cores ([Real-time scheduling](running-in-production.md#real-time-scheduling)) |
| `CPUAffinity=2 3` | a superset of `[engine] cpu` and `net_cpus`, disjoint from the rest of the host. Pair it with `scripts/host-setup.sh tune`; delete it on a shared host, where `cpu = -1` |
| `MemorySwapMax=0`, `OOMScoreAdjust=-500` | a swapped-out or OOM-killed engine leaves orders resting with nothing to cancel them |
| `ReadWritePaths=/var/lib/fastmm` | `ProtectSystem=strict` makes the rest read-only; journals, the epoch file and the kill file live here |

`[engine] cpu_dma_latency_us` needs write access to `/dev/cpu_dma_latency`, which the unit does not grant; add the udev rule in [Host tuning](running-in-production.md#idle-states).

## Run the container

The image is tagged `ghcr.io/ziyangliu-666/fastmm:<version>`, and the newest also `:latest`. Set `VERSION` as in [Install the tarball](#install-the-tarball), then:

```bash
mkdir -p runs && sudo chown 10001:10001 runs
docker run --rm --name fastmm \
  -v "$PWD/live.toml:/etc/fastmm/live.toml:ro" \
  -v "$PWD/runs:/var/lib/fastmm/runs" \
  -e FASTMM_API_KEY -e FASTMM_API_SECRET \
  --stop-timeout 90 \
  "ghcr.io/ziyangliu-666/fastmm:$VERSION" /etc/fastmm/live.toml --duration 60s
```

| Path in the container | Mount |
|---|---|
| `/etc/fastmm/live.toml` | the config, read-only; the default when you pass no argument |
| `/var/lib/fastmm/runs` | journals, the epoch file and the latched kill file; writable by uid 10001 |
| `/dev/shm/fastmm-<name>.status` | the status file; `docker exec fastmm fastmm-top` reads it |

The image runs as uid 10001 under `tini`, which forwards signals. It holds no simulator, test certificates or configuration.

`--stop-timeout 90` gives the cancel-all the same time as the systemd unit. Do not restart a container that exited with 2, 3, 5, 6 or 7. Docker's restart policies cannot exclude exit codes, so use `--restart no`, or an orchestrator that restarts only on a signal or exit 4.

Other programs run through the same entrypoint by name:

```bash
docker run --rm -v "$PWD/runs:/var/lib/fastmm/runs" "ghcr.io/ziyangliu-666/fastmm:$VERSION" \
  fastmm-replay --journal /var/lib/fastmm/runs/<file>.fmj --verify
```

To build the image from a checkout:

```bash
docker build -f deploy/docker/Dockerfile.production -t fastmm:local .
```

`deploy/docker/Dockerfile` and `compose.yml` are the demo instead: root, test certificates and a simulated exchange ([Install](../../getting-started/install.md#docker)).

Next: [Run in production](running-in-production.md), then the [Go-live checklist](go-live-checklist.md).
