# Tutorial: your first market maker

In this tutorial you build `first_mm`, a market maker that quotes around the microprice, keeps
its position within a limit and reacts to lost connections. You test it, backtest it, replay it,
trade it on FastMM's simulated exchange with a disconnect, and finally run it on Binance Demo with
tight risk limits.

You need a FastMM build ([Install](../../getting-started/install.md)) and about an hour; the last
page also needs a Binance Demo Trading account. The code is in
[`examples/cpp/tutorial/`](../../../examples/cpp/tutorial/), the configurations are
`configs/tutorial-sim.toml` and `configs/tutorial-binance-demo.toml`, and every shell command is in
[`scripts/docs/tutorial.sh`](../../../scripts/docs/tutorial.sh), which CI runs up to page 8.

| Page | You will |
|---|---|
| [1. Set up](01-set-up.md) | build FastMM and the tutorial programs |
| [2. The model](02-the-model.md) | learn how a strategy talks to the engine |
| [3. Write the strategy](03-write-the-strategy.md) | write parameters, the quoting function and the hooks |
| [4. Unit-test it](04-unit-test.md) | test the quoting function and the hooks with the harness |
| [5. Backtest in C++](05-backtest-in-cpp.md) | run a seeded backtest and check it is deterministic |
| [6. Register it](06-register.md) | make the strategy available to the command-line tools |
| [7. Backtest and replay from the command line](07-cli-backtest-and-replay.md) | record a backtest and verify its replay |
| [8. Trade on the simulated exchange](08-sim-exchange.md) | trade through a disconnect and replay the live journal |
| [9. Trade on Binance Demo](09-binance-demo.md) | dry-run, then a short keyed session, then stop it safely |

Prefer Python? A Python version of this tutorial is planned with ADR-0012 step 6; until then,
[Python research bindings](../../python.md) show how to backtest from Python.
