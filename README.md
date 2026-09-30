# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml)
[![Docs](https://github.com/ziyangliu-666/FastMM/actions/workflows/docs.yml/badge.svg)](https://ziy.bio/FastMM/)
[![PyPI](https://img.shields.io/pypi/v/fastmm-engine)](https://pypi.org/project/fastmm-engine/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

FastMM is a low-latency market-making engine for crypto exchanges. You write the quoting logic in Python or C++; FastMM runs it in backtests and live, quotes on one exchange and hedges on another, and recovers from crashes without losing track of a trade.

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

| Exchange | Markets | Test environment |
|---|---|---|
| Binance | spot, perpetual futures | Binance Demo |
| OKX | spot, perpetual futures | OKX Demo |
| Bybit | spot, perpetual futures | Bybit testnet |
| Deribit | options, futures | Deribit testnet |
| Gemini | spot, perpetual futures | Gemini sandbox |
| Coinbase | spot | Coinbase Exchange sandbox |

## Documentation

- [Quickstart](docs/getting-started/quickstart.md)
- [Quote on one exchange, hedge on another](docs/how-to/strategies/xmm.md)
- [Run on a testnet or demo account](docs/how-to/operations/run-on-testnet.md)
- [Run in production](docs/how-to/operations/running-in-production.md)
- [How FastMM works](docs/explanation/how-it-works.md)
- [Benchmarks](docs/explanation/benchmarks.md)

Full documentation: <https://ziy.bio/FastMM/>

## License

[MIT](LICENSE)
