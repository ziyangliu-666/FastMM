# Deploy a release

Three forms of a release, from <https://github.com/ziyangliu-666/FastMM/releases> and PyPI. All three carry the same version ([Versions and compatibility](../../reference/compatibility.md)).

| Form | What it holds | Use it for |
|---|---|---|
| `fastmm-<version>-x86_64.tar.gz` | `fastmm-live`, `fastmm-sim-itch`, `fastmm-top`, `fastmm-replay`, the configs, the systemd unit | a host that runs C++ strategies |
| `ghcr.io/ziyangliu-666/fastmm:<version>` | `fastmm-live`, `fastmm-top`, `fastmm-replay`, non-root, no simulator | a container host |
| `pip install "fastmm-engine[live]"` | the engine as a Python package, for strategies written in Python | [Python](../../python.md) |

The tarball and the image are built for x86-64-v2 and need glibc of the build host's version or newer; the tarball also needs libssl 3 (`apt install libssl3` on Ubuntu 24.04). Neither carries a strategy library of your own: a C++ strategy is linked into your own `fastmm-live` build ([Register a strategy](../strategies/register-a-strategy.md)).

## Install the tarball

```bash
curl -LO https://github.com/ziyangliu-666/FastMM/releases/download/v0.2.0/fastmm-0.2.0-x86_64.tar.gz
curl -LO https://github.com/ziyangliu-666/FastMM/releases/download/v0.2.0/fastmm-0.2.0-x86_64.tar.gz.sha256
sha256sum -c fastmm-0.2.0-x86_64.tar.gz.sha256
sudo tar xzf fastmm-0.2.0-x86_64.tar.gz -C /opt
sudo ln -sfn /opt/fastmm-0.2.0-x86_64 /opt/fastmm
/opt/fastmm/bin/fastmm-live --version
```

An upgrade is a second directory and a new symlink target; stop the session before you move the link, because `fastmm-top` refuses a status file written by another build.

Build the tarball yourself with `scripts/package-release.sh`, which also takes `--preset release-dpdk` for a build with the DPDK receive backend linked in ([Receive a multicast feed](multicast-feeds.md)).

## Run under systemd

`deploy/fastmm-live.service` is in the tarball and the repository. One unit per engine: one config, one journal directory, one status file.

```bash
sudo useradd --system --home-dir /var/lib/fastmm --shell /usr/sbin/nologin fastmm
sudo install -d -o fastmm -g fastmm /var/lib/fastmm /var/lib/fastmm/runs
sudo install -d -m 0750 -o root -g fastmm /etc/fastmm
sudo install -m 0640 -o root -g fastmm my-live.toml /etc/fastmm/live.toml
sudo install -m 0644 /opt/fastmm/deploy/fastmm-live.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl start fastmm-live
```

`/etc/fastmm/fastmm-live.env` holds `FASTMM_CONFIG` and the API keys the config reads as `${VAR}`; keep it mode 0600 and root-owned. An unset variable exits 2 before anything connects ([Keys](running-in-production.md#9-keys)).

```ini
FASTMM_CONFIG=/etc/fastmm/live.toml
FASTMM_API_KEY=...
FASTMM_API_SECRET=...
```

What the unit sets, and when to change it:

| Directive | Why |
|---|---|
| `Restart=on-failure`, `RestartPreventExitStatus=2 3 5 6 7` | a crash (any signal, `kill -9` included) or exit 4 restarts after 2 s: the new session restores the position from the store and the venue's executions and cancels the orders the dead one left ([Running this in production](running-in-production.md#1-what-survives-a-restart)). 2 and 3 need a config fix, and 5, 6 and 7 a human ([Errors and exit codes](../../reference/errors.md)); a latched kill switch exits 6 at every start. `StartLimitBurst=5` in ten minutes ends a crash loop |
| `TimeoutStopSec=90` | SIGTERM pulls the quotes and cancels every order over REST before the process exits ([Kill switch and shutdown](kill-switch-and-shutdown.md)) |
| `LimitMEMLOCK=infinity` | `[engine] lock_memory = true` calls `mlockall()`; without the limit the call fails and logs a warning. Drop the line when `lock_memory` is off |
| `CPUAffinity=2 3` | the cores the process may use, a superset of `[engine] cpu` and `net_cpus` and disjoint from everything else on the host. Pair it with `isolcpus`, `nohz_full` and `rcu_nocbs` (`scripts/host-setup.sh tune`), and delete the line on a shared host, where `cpu = -1` ([Go-live checklist](go-live-checklist.md)) |
| `MemorySwapMax=0`, `OOMScoreAdjust=-500` | a swapped-out or OOM-killed engine leaves orders resting at the venue with nothing to cancel them |
| `ReadWritePaths=/var/lib/fastmm` | `ProtectSystem=strict` makes the rest of the filesystem read-only; journals, the epoch file and the kill file live here |

Logs: stderr carries the lines at `[logging] mirror_level` and above, so `journalctl -u fastmm-live -f` shows the warnings and errors, and `[logging] file` takes the full log for a rotation you own. Journal files grow without a cap ([The journal](running-in-production.md#5-the-journal)).

```bash
systemctl status fastmm-live                 # the exit code of the last run
journalctl -u fastmm-live -f
/opt/fastmm/bin/fastmm-top                   # the live session
sudo -u fastmm /opt/fastmm/bin/fastmm-live --config /etc/fastmm/live.toml --clear-kill
```

## Run the container

The image runs as uid 10001 with no shell login, trusts the distro CA bundle, and has `tini` as PID 1 so signals and reaped children behave. It holds no simulator, no test certificates and no configuration: the first argument is a config path.

```bash
mkdir -p runs && sudo chown 10001:10001 runs
docker run --rm --name fastmm \
  -v "$PWD/live.toml:/etc/fastmm/live.toml:ro" \
  -v "$PWD/runs:/var/lib/fastmm/runs" \
  -e FASTMM_API_KEY -e FASTMM_API_SECRET \
  --stop-timeout 90 \
  ghcr.io/ziyangliu-666/fastmm:0.2.0 /etc/fastmm/live.toml --duration 60s
```

| Path in the container | Mount |
|---|---|
| `/etc/fastmm/live.toml` | the config, read-only. `CMD` uses this path when you pass no argument |
| `/var/lib/fastmm/runs` | journals, the epoch file and the latched kill file; the host directory has to be writable by uid 10001 |
| `/dev/shm/fastmm-<name>.status` | the status file. `docker exec fastmm fastmm-top` reads it from inside the container |

`--stop-timeout 90` gives the cancel-all the same room as the systemd unit. A container that exits with 5, 6 or 7 must not be restarted by the orchestrator; set the restart policy to `no` for the same reason the unit does.

Other programs run through the same entrypoint by name: `docker run --rm -v "$PWD/runs:/var/lib/fastmm/runs" ghcr.io/ziyangliu-666/fastmm:0.2.0 fastmm-replay --journal /var/lib/fastmm/runs/<file>.fmj --verify`.

Build it from a checkout with `docker build -f docker/Dockerfile.production -t fastmm:local .`. `docker/Dockerfile` and `docker-compose.yml` are the demo instead: they run as root with test certificates and a simulated exchange ([Install](../../getting-started/install.md#docker)).

## Related

- [Go-live checklist](go-live-checklist.md): the list to run before every session.
- [Operations runbook](runbook.md): first deploy, daily checks, what to do when something fires.
- [Running this in production](running-in-production.md): what breaks, and the mitigation for each.
