<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://ziy.bio/FastMM/assets/brand/fastmm-logo-white.svg">
    <img src="https://ziy.bio/FastMM/assets/brand/fastmm-logo.svg" alt="FastMM" height="52">
  </picture>
</h1>

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
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

This runs a strategy on a simulated market, with no account and no API keys (Linux x86-64, Python 3.10+).

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
