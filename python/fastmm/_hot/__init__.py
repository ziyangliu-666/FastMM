"""Python hot hooks (ADR-0013, section 1).

decl      @fastmm.hot, fastmm.State, the checks at class creation and the `self` record layout
          (no numba import)
abi       numpy mirror of include/fastmm/strategies/hot_abi.h
compiler  ctx methods, the generated wrapper, the IR check, the Numba cache and the backtest run
          (imports numba)
"""
