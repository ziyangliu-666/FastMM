# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml)
[![Docs](https://github.com/ziyangliu-666/FastMM/actions/workflows/docs.yml/badge.svg)](https://ziy.bio/FastMM/)
[![PyPI](https://img.shields.io/pypi/v/fastmm-engine)](https://pypi.org/project/fastmm-engine/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

FastMM is a low-latency market-making engine. Read the [documentation](https://ziy.bio/FastMM/).

> If you are running FastMM on Binance, try [bndesk](https://github.com/ziyangliu-666/bndesk), a real-time dashboard for your accounts.

## Features

- **Strategies in C++ or Python.** Python hooks marked `@fastmm.hot` compile to machine code with Numba.
- **One strategy file for backtest and live.** Replays are deterministic, and the simulated exchange models queue position.
- **Account pools.** Several sub-accounts trade behind one venue, and orders spread across them.
- **Gateway.** One `fastmm-gateway` holds the exchange connections; several strategy processes attach to it and can stop or restart without dropping them.
- **Live control.** `fastmm-ctl` changes a running session's parameters without a restart.
- **Low-latency tuning.** Host and network tuning built in, such as core pinning and kernel bypass.

## Quickstart

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

This runs a strategy on a simulated market, with no account and no API keys. On Debian and Ubuntu, `sudo apt install python3-venv` first.

The wheels are for Linux x86-64 and Python 3.10 or newer. On macOS (Apple silicon included) or Windows, run the same steps in a container:

```bash
docker run --rm -it --platform linux/amd64 -v "$PWD":/work -w /work python:3.12-slim \
  sh -c 'pip install "fastmm-engine[hot]" && fastmm init my-mm && cd my-mm && python backtest.py'
```

The C++ programs are in `ghcr.io/ziyangliu-666/fastmm` and in the release tarball ([Deploy](https://ziy.bio/FastMM/how-to/operations/deploy/)).

## A strategy

<!-- snippet: examples/python/strategies/touch_mm.py#example -->
```python
import fastmm
from fastmm import Param


class TouchMM(fastmm.Strategy):
    half_spread_bps = Param(0.01, min=0.0, doc="half spread, bps")
    quote_qty = Param(0.002, min=0.0, doc="size per side")

    @fastmm.hot  # compiled to machine code with Numba
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            return
        half = book.mid * self.half_spread_bps * 1e-4
        ctx.clear()
        ctx.bid(book.mid - half, self.quote_qty)
        ctx.ask(book.mid + half, self.quote_qty)
        ctx.keep_passive()
```

## Exchanges

Binance, OKX, Bybit, Gate, Deribit, Gemini and Coinbase. Nasdaq over TotalView-ITCH and OUCH 5.0.

## License

[MIT](LICENSE)
