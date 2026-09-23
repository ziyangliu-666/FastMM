# FastMM documentation

FastMM is a market-making engine in C++20. Backtests, replay and live trading run the same strategy code. These pages describe version 0.1.

## Run it in five minutes

```bash
./scripts/bootstrap.sh                                   # checks the toolchain, configures the release preset
cmake --build --preset release -j
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
./scripts/run-sim.sh --duration 30s                      # simulated exchange + live engine on localhost
```

The first command needs Linux, gcc 13+ or clang 16+, CMake 3.25+, Ninja, OpenSSL 3 and zlib ([Install](getting-started/install.md) has the details and the Docker route).

The backtest prints a summary table; `run-sim.sh` runs a full live session against a local exchange and writes a journal you can replay. What those numbers mean: [Quick start](getting-started/quickstart.md).

## Start here

| You are | Read, in this order |
|---|---|
| New to FastMM | [Quick start](getting-started/quickstart.md), then [How FastMM works](explanation/how-it-works.md) |
| Writing a strategy in C++ | [Tutorial: your first market maker](tutorials/first-strategy/README.md), then the [Strategy API](reference/strategy-api.md) |
| Writing a strategy in Python | [Python](python.md), then [Write hot hooks in Python](how-to/strategies/python-hot-hooks.md) |
| Going to run this with real money | [Economics of the shipped strategies](explanation/economics.md), then [Running this in production](how-to/operations/running-in-production.md), then the [Go-live checklist](how-to/operations/go-live-checklist.md) |
| Operating a running session | [Operations runbook](how-to/operations/runbook.md), [Troubleshooting](how-to/operations/troubleshooting.md), [Errors and exit codes](reference/errors.md) |

Terms are defined once, in the [Glossary](reference/glossary.md).

## Before real money

FastMM has not been run with real money, and the shipped strategies are reference implementations of published quoting rules, not an edge.

- [Economics of the shipped strategies](explanation/economics.md): the example backtest's profit is a configured maker rebate; at a real fee it is negative.
- [Running this in production](how-to/operations/running-in-production.md): what breaks, what is not covered, and the mitigation for each.
- [Go-live checklist](how-to/operations/go-live-checklist.md): the list to run before every session.

## Tutorials

- [Your first market maker](tutorials/first-strategy/README.md) (C++): from a strategy header to the simulated exchange and Binance Demo, in nine steps.
- [Quick start](getting-started/quickstart.md): a strategy and a backtest in one page.

## How-to guides

- Strategies: [Register a strategy](how-to/strategies/register-a-strategy.md), [Write hot hooks in Python](how-to/strategies/python-hot-hooks.md), [Run slow methods beside hot hooks](how-to/strategies/python-slow-methods.md), [Run a Python strategy live](how-to/strategies/python-live.md)
- Venues: [Add a venue](how-to/venues/add-a-venue.md)
- Operations: [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md), [Running this in production](how-to/operations/running-in-production.md), [Operations runbook](how-to/operations/runbook.md), [Go-live checklist](how-to/operations/go-live-checklist.md), [Kill switch and shutdown](how-to/operations/kill-switch-and-shutdown.md), [Journals, replay and PnL](how-to/operations/journals-replay-pnl.md), [Monitor a session with fastmm-top](how-to/operations/monitor-with-fastmm-top.md), [Receive a multicast feed](how-to/operations/multicast-feeds.md), [Two-host benchmark](how-to/operations/two-host-benchmark.md), [Troubleshooting](how-to/operations/troubleshooting.md)

## Reference

- Strategies: [Strategy API](reference/strategy-api.md), [Fixed point](reference/fixed-point.md), [Public API and header tiers](reference/public-api.md)
- Programs and files: [Command lines](reference/cli.md), [Configuration](reference/configuration.md), [Errors and exit codes](reference/errors.md), [Journal format](reference/journal-format.md), [Status file](reference/status-file.md)
- Venues and protocols: [Venue connectors](reference/venues.md), [Simulated exchange](reference/sim-exchange.md), [fastmm-sim-itch](reference/sim-itch.md), [Options](reference/options.md), [FIX 4.4](reference/codecs/fix.md), [Nasdaq ITCH and OUCH](reference/codecs/nasdaq.md), [CME MDP 3.0](reference/codecs/cme-mdp3.md)
- Python: [Python](python.md), [Python strategy API](reference/python-api.md)
- [Glossary](reference/glossary.md)

## Explanation

- [How FastMM works](explanation/how-it-works.md): the thread model, the event path, and what the engine does and does not guarantee
- [Economics of the shipped strategies](explanation/economics.md): fees, adverse selection and what the backtest does not model
- [Architecture](explanation/architecture.md): threads, rings, the network reactor, clocks
- [Event flow](explanation/event-flow.md): from a venue message to an order on the wire
- [Determinism](explanation/determinism.md): why replays match, and what breaks them
- [Risk model](explanation/risk-model.md): the pre-trade checks and the kill switch
- [Benchmarks](explanation/benchmarks.md): how the latency numbers are measured
- [Design records](adr/README.md): the 15 decisions behind the code

## Contributing

- [CONTRIBUTING](../CONTRIBUTING.md): build, format, commit conventions
- [Writing docs](contributing/writing-docs.md): page kinds, style, snippets, generated pages, checks
- [Dependencies](contributing/dependencies.md)
- [Python packages](contributing/python-packages.md): development install, type stub, wheels
