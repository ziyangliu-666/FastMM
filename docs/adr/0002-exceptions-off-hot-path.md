# ADR-0002: Exceptions and RTTI stay enabled, but off the hot path

Status: accepted (2026-09)

## Context

simdjson, fmt, toml++ and pybind11 require exceptions/RTTI. With zero-cost exception tables, `-fno-exceptions` does not speed up the non-throwing path.

## Decision

Keep both enabled globally. Hot-path functions are `noexcept` and return `Result<T, E>`. Exceptions are allowed only during startup/config and in the Python layer. `dynamic_cast` is forbidden by a CI grep.

## Consequences

One policy to review instead of a compiler flag; the no-allocation test harness doubles as a no-throw check because throwing allocates.
