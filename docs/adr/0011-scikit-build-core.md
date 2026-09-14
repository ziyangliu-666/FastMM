# ADR-0011: scikit-build-core for Python packaging

Status: accepted (2026-09)

## Context

The Python module must reuse the same CMake tree and flags and support editable installs.

## Decision

`pyproject.toml` with scikit-build-core; `pip install -e .` builds only the backtest library and `_core` module (no OpenSSL dependency in the wheel).

## Consequences

Version is single-sourced from `CMakeLists.txt`.
