# FastMM documentation

FastMM is a low-latency market-making engine in C++20: one engine thread owns all trading state,
every input is journaled, and the same code runs live, in simulation and in deterministic replay.
These pages are plain Markdown and read on GitHub; each page is one of four kinds: a tutorial
teaches, a how-to reaches a goal, a reference describes, an explanation gives the reasons.

## Start here

| You want to | Read |
|---|---|
| Build FastMM and run something in 10 minutes | [Install](getting-started/install.md), then the [Quick start](getting-started/quickstart.md) |
| Write, test and trade your first strategy | [Tutorial: your first market maker](tutorials/first-strategy/README.md) |
| Look up a hook, a context method or a parameter macro | [Strategy API](reference/strategy-api.md) |
| Run a session on a testnet or Binance Demo | [Run on a testnet](how-to/operations/run-on-testnet.md), then the [Go-live checklist](how-to/operations/go-live-checklist.md) |
| Connect a new venue | [Add a venue](how-to/venues/add-a-venue.md) |
| Understand how an event becomes an order | [Event flow](explanation/event-flow.md) and [Architecture](explanation/architecture.md) |
| Change FastMM or its docs | [CONTRIBUTING](../CONTRIBUTING.md) and [Writing docs](contributing/writing-docs.md) |

## Getting started

- [Install](getting-started/install.md): toolchain, build presets, Docker, the Python package
- [Quick start](getting-started/quickstart.md): one strategy header and a backtest, under 30 lines

## Tutorials

- [Your first market maker](tutorials/first-strategy/README.md) (C++): from a strategy header to
  the simulated exchange and Binance Demo
- Your first strategy in Python: planned with ADR-0012 step 6; until then see
  [Python research bindings](python.md)

## How-to guides

- Strategies: [Register a strategy](how-to/strategies/register-a-strategy.md)
- Venues: [Add a venue](how-to/venues/add-a-venue.md)
- Operations: [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md),
  [Go-live checklist](how-to/operations/go-live-checklist.md),
  [Kill switch and shutdown](how-to/operations/kill-switch-and-shutdown.md),
  [Journals, replay and PnL](how-to/operations/journals-replay-pnl.md),
  [Monitor a session with fastmm-top](how-to/operations/monitor-with-fastmm-top.md),
  [Troubleshooting](how-to/operations/troubleshooting.md)

## Reference

- Strategies: [Strategy API](reference/strategy-api.md), [Fixed point](reference/fixed-point.md),
  [Public API and header tiers](reference/public-api.md)
- Programs and files: [Command lines](reference/cli.md), [Configuration](reference/configuration.md),
  [Journal format](reference/journal-format.md), [Status file](reference/status-file.md)
- Venues and protocols: [Venue connectors](reference/venues.md),
  [Simulated exchange](reference/sim-exchange.md), [Options](reference/options.md),
  [FIX 4.4](reference/codecs/fix.md), [Nasdaq ITCH and OUCH](reference/codecs/nasdaq.md),
  [CME MDP 3.0](reference/codecs/cme-mdp3.md)
- Python: [Python research bindings](python.md), [Python API](reference/python-api.md)
- [Glossary](reference/glossary.md)

## Explanation

- [Architecture](explanation/architecture.md): threads, rings, the network reactor, clocks
- [Event flow](explanation/event-flow.md): from a venue message to an order on the wire
- [Determinism](explanation/determinism.md): why replays match, and what breaks them
- [Risk model](explanation/risk-model.md): the pre-trade checks and the kill switch
- [Benchmarks](explanation/benchmarks.md): how the latency numbers are measured
- [Design records](adr/): the decisions behind the above

## Contributing

- [CONTRIBUTING](../CONTRIBUTING.md): build, format, commit conventions
- [Writing docs](contributing/writing-docs.md): page kinds, snippets, generated pages, the checks
- [Dependencies](contributing/dependencies.md)
