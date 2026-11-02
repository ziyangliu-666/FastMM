# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml)
[![Docs](https://github.com/ziyangliu-666/FastMM/actions/workflows/docs.yml/badge.svg)](https://ziy.bio/FastMM/)
[![PyPI](https://img.shields.io/pypi/v/fastmm-engine)](https://pypi.org/project/fastmm-engine/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

FastMM is a low-latency market-making engine. Read the [documentation](https://ziy.bio/FastMM/).

https://github.com/user-attachments/assets/dcd130c5-28fe-482f-a757-d0fc6550c5b9

## Features

- [Low latency](https://ziy.bio/FastMM/explanation/architecture/#memory)
- [High-performance backtesting](https://ziy.bio/FastMM/explanation/backtesting/#many-runs-in-one-pass)
- [Cross-exchange market making](https://ziy.bio/FastMM/how-to/strategies/xmm/)
- [One strategy for backtest, replay and live](https://ziy.bio/FastMM/how-to/strategies/python-hot-hooks/)
- [Multi-account trading](https://ziy.bio/FastMM/how-to/operations/run-behind-a-gateway/)
- [Live A/B tests](https://ziy.bio/FastMM/how-to/strategies/ab-test/)

## Quickstart

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

```text
backtest py:MyMm  seed=1  md_events=7149  steps=12428  wall=0.02s
  net pnl                        -5.5272
  realized / unrealized / fees   0.5198 / 0.0005 / 6.0475
  fills (maker / taker)          1036 (1036 / 0)
  ...
```

This backtests a strategy on a simulated market; the [Quick start](https://ziy.bio/FastMM/getting-started/quickstart/) goes on to a live session.

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

FastMM trades on Binance (spot and USDⓈ-M futures), OKX, Bybit, Gate, Hyperliquid, Lighter, Deribit, Gemini, Coinbase (Advanced Trade and Exchange) and, for US stocks, Alpaca ([Venue connectors](https://ziy.bio/FastMM/reference/venues/)).

> If you are running FastMM on Binance, try [bndesk](https://github.com/ziyangliu-666/bndesk), a real-time dashboard for your accounts.

## License

[MIT](LICENSE)
