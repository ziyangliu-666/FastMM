# Tutorial: your first market maker

You build `first_mm`, a market maker that quotes around the microprice, limits its position and reacts to lost connections, and take it from a unit test to a Binance Demo session.

You need a FastMM build ([Install](../../getting-started/install.md)); page 9 also needs a Binance Demo Trading account.

| Page | You |
|---|---|
| [1. Set up](01-set-up.md) | build FastMM and list its strategies |
| [2. The model](02-the-model.md) | learn how the engine calls a strategy |
| [3. Write the strategy](03-write-the-strategy.md) | write the parameters, the quoting function and the hooks |
| [4. Unit-test it](04-unit-test.md) | test the quoting function and the hooks |
| [5. Backtest in C++](05-backtest-in-cpp.md) | run a 60 s backtest from a C++ program |
| [6. Register it](06-register.md) | add `first_mm` to the command-line programs |
| [7. Backtest and replay from the command line](07-cli-backtest-and-replay.md) | record a backtest and verify its replay |
| [8. Trade on the simulated exchange](08-sim-exchange.md) | trade through a disconnect and replay the live journal |
| [9. Trade on Binance Demo](09-binance-demo.md) | dry run, run a keyed session and check the shutdown |

| Material | Where |
|---|---|
| code | [`examples/cpp/tutorial/`](../../../examples/cpp/tutorial/) |
| configurations | `configs/tutorial-sim.toml`, `configs/tutorial-binance-demo.toml` |
| shell commands | [`scripts/docs/tutorial.sh`](../../../scripts/docs/tutorial.sh); CI runs them through page 8 |

Python strategies: [Python](../../python.md).
