# 1. Set up

Run the commands in this tutorial from the repository root.

## Build

<!-- snippet: scripts/docs/tutorial.sh#build -->
```bash
./scripts/bootstrap.sh
cmake --build --preset release -j
```

`bootstrap.sh` checks the toolchain and configures the `release` preset ([Install](../../getting-started/install.md) lists the requirements). A top-level build includes the tutorial.

Later pages use these variables:

<!-- snippet: scripts/docs/tutorial.sh#bin -->
```bash
BUILD=build/release
BIN=$BUILD/bin
```

## Check

<!-- snippet: scripts/docs/tutorial.sh#list -->
```bash
"$BIN"/fastmm-backtest --list-strategies
```

Output (abridged):

```text
basic_mm
  half_spread_bps            bps     default=5          [0, 10000]  half spread around mid, basis points
  ...
avellaneda_stoikov
  ...
options_mm
  ...
```

The tutorial's programs are in `build/release/bin/`:

| Program | Source | Page |
|---|---|---|
| `first_mm_test` | `examples/cpp/tutorial/first_mm_test.cpp` | 4 |
| `first_mm_backtest` | `examples/cpp/tutorial/first_mm_backtest.cpp` | 5 |
| `tutorial-backtest`, `tutorial-replay`, `tutorial-live` | `examples/cpp/tutorial/tutorial_*.cpp` | 6 to 9 |

Next: [2. The model](02-the-model.md)
