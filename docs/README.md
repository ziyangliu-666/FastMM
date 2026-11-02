# FastMM

- [Low latency](explanation/architecture.md#memory)
- [High-performance backtesting](explanation/backtesting.md#many-runs-in-one-pass)
- [Cross-exchange market making](how-to/strategies/xmm.md)
- [One strategy for backtest, replay and live](how-to/strategies/python-hot-hooks.md)
- [Multi-account trading](how-to/operations/run-behind-a-gateway.md)
- [Live A/B tests](how-to/strategies/ab-test.md)

<video class="fastmm-video" controls preload="none" poster="assets/explainer-poster.jpg" src="https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-explainer.mp4"></video>

FastMM trades on Binance (spot and USDⓈ-M futures), OKX, Bybit, Gate, Hyperliquid, Lighter, Deribit, Gemini, Coinbase (Advanced Trade and Exchange) and, for US stocks, Alpaca ([Venue connectors](reference/venues.md)).

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

The wheel needs Linux x86-64 and Python 3.10 or later; on Debian and Ubuntu, `sudo apt install python3-venv` first. For C++, see the [Quick start in C++](getting-started/cpp-quickstart.md) or [Build from source](getting-started/install.md#build-from-source).

The same strategy trades live against a simulated exchange on this machine:

```bash
pip install "fastmm-engine[live]"
fastmm sim &
python -m fastmm run strategy:MyMm --config sim.toml --duration 60s
```

## Films

<div class="fastmm-films" markdown>

- [![](assets/explainer-poster.jpg)**FastMM in two minutes**](https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-explainer.mp4)
  What a market maker does, and what FastMM does for one.
- [![](assets/intro-poster.jpg)**The fastest trader wins**](explanation/how-fast.md)
  Why the quote that moves first keeps the fill.
- [![](assets/film-poster.jpg)**One tick through the engine**](explanation/how-it-works.md)
  From the market-data packet to the order on the wire.

</div>

## Dashboard

<div class="fastmm-desk">
<div class="fastmm-desk__text">
<p><strong>bndesk</strong>: a real-time, read-only dashboard for FastMM on Binance.</p>
<p><a class="md-button md-button--primary" href="https://ziy.bio/bndesk/">Live demo</a> <a class="md-button" href="https://github.com/ziyangliu-666/bndesk">Source</a></p>
</div>
<div class="fastmm-desk__row">
<a class="fastmm-desk__media" style="--r: 1.6" href="https://ziy.bio/bndesk/"><video autoplay loop muted playsinline preload="metadata" poster="assets/bndesk-desk.jpg" src="assets/bndesk-desk.mp4"></video></a>
<a class="fastmm-desk__media" style="--r: 1.6" href="https://ziy.bio/bndesk/"><video autoplay loop muted playsinline preload="metadata" poster="assets/bndesk-orders.jpg" src="assets/bndesk-orders.mp4"></video></a>
<a class="fastmm-desk__media" style="--r: 0.4615" href="https://ziy.bio/bndesk/"><video autoplay loop muted playsinline preload="metadata" poster="assets/bndesk-phone.jpg" src="assets/bndesk-phone.mp4"></video></a>
</div>
</div>

## Where to go next

- [Quick start](getting-started/quickstart.md): a Python strategy, a backtest and a live session against the local exchange.
- [Quick start in C++](getting-started/cpp-quickstart.md): a C++ strategy backtested from a CMake project.
- [Cross-exchange market making](how-to/strategies/xmm.md): run `xmm` on Binance Demo.
- [Run in production](how-to/operations/running-in-production.md): what to set before a keyed session runs unattended.
- [How FastMM works](explanation/how-it-works.md): the engine thread, the path of one event and what it guarantees.
