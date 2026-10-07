# FastMM @VERSION@ (x86_64)

Portable build for x86-64-v2. The programs need glibc @GLIBC@ or newer and nothing else: OpenSSL,
libstdc++ and libgcc are linked in.

| Program | |
|---|---|
| `fastmm-live` | the engine |
| `fastmm-gateway` | shared venue connections for several engines |
| `fastmm-ctl` | commands to a running engine or gateway |
| `fastmm-top` | live dashboard of a session |
| `fastmm-pnl` | P&L from a journal |
| `fastmm-replay` | replays a journal |
| `fastmm-sim-exchange` | Binance Spot-compatible simulated exchange |
| `fastmm-sim-itch` | Nasdaq-style ITCH/OUCH simulator |

## Install

```bash
sudo tar xzf fastmm-@VERSION@-x86_64.tar.gz -C /opt
sudo ln -sfn /opt/fastmm-@VERSION@-x86_64 /opt/fastmm
/opt/fastmm/bin/fastmm-live --version
```

## Run

The simulated exchange and the engine, from the unpacked directory (the session writes `runs/`
there):

```bash
tar xzf fastmm-@VERSION@-x86_64.tar.gz && cd fastmm-@VERSION@-x86_64
bin/fastmm-sim-exchange --config configs/sim.toml > sim.log 2>&1 & sleep 1
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
  bin/fastmm-live --config configs/sim-local.toml --duration 30s; kill $!
```

`bin/fastmm-top --name sim-local` in another terminal watches the session. A Python strategy runs
against the same simulator: <https://ziy.bio/FastMM/how-to/strategies/python-live/>.

The Nasdaq-style feed is multicast on `lo`, so it runs in its own network namespace, as you:

```bash
sudo unshare -n sh -c 'ip link set lo up multicast on && ip route add 224.0.0.0/4 dev lo &&
  exec setpriv --reuid="$SUDO_UID" --regid="$SUDO_GID" --clear-groups sh -c "
    bin/fastmm-sim-itch --config configs/sim-itch.toml > sim-itch.log 2>&1 & sleep 1
    bin/fastmm-live --config configs/nasdaq-itch-sim.toml --duration 30s; kill \$!"'
```

`deploy/fastmm-live.service` is the systemd unit; copy it to `/etc/systemd/system/` and edit the
paths. `deploy/prometheus/fastmm-alerts.yml` holds Prometheus alerting rules for the metrics
`fastmm-top --metrics` serves. Read the deployment guide before pointing this at a venue:
<https://ziy.bio/FastMM/how-to/operations/deploy/> and
<https://ziy.bio/FastMM/how-to/operations/go-live-checklist/>.

Licence: MIT (`LICENSE`). Source: <https://github.com/ziyangliu-666/FastMM>.
