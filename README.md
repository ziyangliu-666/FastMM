# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml)
[![Docs](https://github.com/ziyangliu-666/FastMM/actions/workflows/docs.yml/badge.svg)](https://ziy.bio/FastMM/)
[![PyPI](https://img.shields.io/pypi/v/fastmm-engine)](https://pypi.org/project/fastmm-engine/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

FastMM is a low-latency market-making engine. Read the [documentation](https://ziy.bio/FastMM/).

https://github.com/user-attachments/assets/dcd130c5-28fe-482f-a757-d0fc6550c5b9

## Features

- **[Low latency](https://ziy.bio/FastMM/explanation/benchmarks/).** In the engine a book update becomes an order in 143 ns (p50). Core pinning, busy polling, AF_XDP or DPDK receive and OpenOnload or XLIO order entry tune the host around it.
- **[Sweeps in one pass](https://ziy.bio/FastMM/explanation/backtesting/#many-runs-in-one-pass).** Thousands of parameter points share one pass over the data, and each gets the result its own backtest gives: 16,384 points over four hours of Binance BTCUSDT futures took 228 s on 8 threads.
- **[Quote on one venue, hedge on another](https://ziy.bio/FastMM/how-to/strategies/xmm/).** `xmm` makes the market on one exchange and hedges each fill with an IOC on another.
- **[C++ or Python, backtest or live](https://ziy.bio/FastMM/how-to/strategies/python-hot-hooks/).** One strategy file runs in backtests, replays and live sessions; replays are deterministic and the simulated exchange models queue position. Python hooks marked `@fastmm.hot` compile to machine code with Numba.
- **[Many accounts, one gateway](https://ziy.bio/FastMM/how-to/operations/run-behind-a-gateway/).** Several sub-accounts trade behind one venue, and strategy processes attach to one `fastmm-gateway` that keeps the exchange connections while they restart.
- **[A/B tests](https://ziy.bio/FastMM/how-to/strategies/ab-test/).** Parameter sets run side by side, split by instrument, by time block or both, and are compared on markouts and net with paired intervals.

## Quickstart

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

This runs a strategy on a simulated market.

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
