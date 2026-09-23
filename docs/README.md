# FastMM documentation

FastMM is a market-making engine in C++20. It receives a venue's market data, keeps an order book,
asks your strategy what it wants resting, and sends the difference to the venue. The trading thread
does not allocate or wait on network I/O; a market-data update becomes an order in about 250 ns
(median, in simulation). Every session is recorded, and replaying the recording sends the same
orders again. These pages describe version 0.1.

## Three ways in

| You want to | Start here |
|---|---|
| See it run | [Install](getting-started/install.md), then [Quick start](getting-started/quickstart.md): a strategy, a backtest and the simulated exchange on localhost. |
| Write a strategy | [Your first market maker](tutorials/first-strategy/README.md) in C++, or [Python](python.md) for hot hooks compiled with Numba. Then the [Strategy API](reference/strategy-api.md). |
| Run a live session | [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md), then the [Go-live checklist](how-to/operations/go-live-checklist.md) and [Kill switch and shutdown](how-to/operations/kill-switch-and-shutdown.md). |

The [API reference](api/cpp.md) is generated from the public headers and from the `fastmm` package
on every build.

## Tutorials

- [Your first market maker](tutorials/first-strategy/README.md) (C++): from a strategy header to the simulated exchange and Binance Demo

## How-to guides

- Strategies: [Register a strategy](how-to/strategies/register-a-strategy.md), [Write hot hooks in Python](how-to/strategies/python-hot-hooks.md), [Run slow methods beside hot hooks](how-to/strategies/python-slow-methods.md), [Run a Python strategy live](how-to/strategies/python-live.md)
- Venues: [Add a venue](how-to/venues/add-a-venue.md)
- Operations: [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md), [Go-live checklist](how-to/operations/go-live-checklist.md), [Kill switch and shutdown](how-to/operations/kill-switch-and-shutdown.md), [Journals, replay and PnL](how-to/operations/journals-replay-pnl.md), [Monitor a session with fastmm-top](how-to/operations/monitor-with-fastmm-top.md), [Receive a multicast feed](how-to/operations/multicast-feeds.md), [Two-host benchmark](how-to/operations/two-host-benchmark.md), [Troubleshooting](how-to/operations/troubleshooting.md)

## Reference

- Strategies: [Strategy API](reference/strategy-api.md), [Fixed point](reference/fixed-point.md), [Public API and header tiers](reference/public-api.md)
- Programs and files: [Command lines](reference/cli.md), [Configuration](reference/configuration.md), [Journal format](reference/journal-format.md), [Status file](reference/status-file.md)
- Venues and protocols: [Venue connectors](reference/venues.md), [Simulated exchange](reference/sim-exchange.md), [fastmm-sim-itch](reference/sim-itch.md), [Options](reference/options.md), [FIX 4.4](reference/codecs/fix.md), [Nasdaq ITCH and OUCH](reference/codecs/nasdaq.md), [CME MDP 3.0](reference/codecs/cme-mdp3.md)
- Python: [Python](python.md), [Python strategy API](reference/python-api.md)
- Generated: [C++ API](api/cpp.md), [Python API](api/python.md)
- [Glossary](reference/glossary.md)

## Explanation

- [Architecture](explanation/architecture.md): threads, rings, the network reactor, clocks
- [Event flow](explanation/event-flow.md): from a venue message to an order on the wire
- [Determinism](explanation/determinism.md): why replays match, and what breaks them
- [Risk model](explanation/risk-model.md): the pre-trade checks and the kill switch
- [Benchmarks](explanation/benchmarks.md): how the latency numbers are measured
- [Design records](adr/0001-fixed-point-int64-price-qty.md): one page per decision

## Contributing

- [CONTRIBUTING](../CONTRIBUTING.md): build, format, commit conventions
- [Writing docs](contributing/writing-docs.md): page kinds, style, snippets, generated pages, checks
- [Dependencies](contributing/dependencies.md)
- [Python packages](contributing/python-packages.md): development install, type stub, wheels
