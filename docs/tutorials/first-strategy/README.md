# Tutorial: your first market maker

`first_mm` is a market maker that quotes around the microprice, limits its position and reacts to lost connections.

You need a FastMM build ([Install](../../getting-started/install.md)); page 9 also needs a Binance Demo Trading account. The code is in [`examples/cpp/tutorial/`](../../../examples/cpp/tutorial/), the configurations are `configs/tutorial-sim.toml` and `configs/tutorial-binance-demo.toml`, and the shell commands are in [`scripts/docs/tutorial.sh`](../../../scripts/docs/tutorial.sh), which CI runs through page 8.

1. [Set up](01-set-up.md)
2. [The model](02-the-model.md)
3. [Write the strategy](03-write-the-strategy.md)
4. [Unit-test it](04-unit-test.md)
5. [Backtest in C++](05-backtest-in-cpp.md)
6. [Register it](06-register.md)
7. [Backtest and replay from the command line](07-cli-backtest-and-replay.md): record a backtest and verify its replay
8. [Trade on the simulated exchange](08-sim-exchange.md): trade through a disconnect and replay the live journal
9. [Trade on Binance Demo](09-binance-demo.md): dry run, a keyed session, shutdown check

Python: [Python research bindings](../../python.md).
